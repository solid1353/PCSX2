// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "INISettingsInterface.h"
#include "LayeredSettingsInterface.h"

#include "common/Error.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace
{
	TEST(LaunchSetting, SplitsNestedSectionAtLastSlashBeforeFirstEquals)
	{
		LayeredSettingsInterface settings;
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/EECycleRate=-1"));
		EXPECT_EQ(settings.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), -1);
		ASSERT_TRUE(settings.SetCommandLineSetting("Folders/Videos=directory with spaces/take=1"));
		EXPECT_EQ(settings.GetStringValue("Folders", "Videos"), "directory with spaces/take=1");
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/EnablePINE=true"));
		EXPECT_TRUE(settings.GetBoolValue("EmuCore", "EnablePINE", false));
	}

	TEST(LaunchSetting, AllowsEmptyStringValue)
	{
		MemorySettingsInterface base;
		base.SetStringValue("Folders", "Videos", "configured");
		LayeredSettingsInterface settings;
		settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
		ASSERT_TRUE(settings.SetCommandLineSetting("Folders/Videos="));
		EXPECT_EQ(settings.GetStringValue("Folders", "Videos", "default"), "");
	}

	TEST(LaunchSetting, LaterValueWinsAndOtherKeysRemain)
	{
		LayeredSettingsInterface settings;
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/EECycleRate=-1"));
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/EECycleSkip=1"));
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/EECycleRate=0"));
		EXPECT_EQ(settings.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), 0);
		EXPECT_EQ(settings.GetIntValue("EmuCore/Speedhacks", "EECycleSkip", 99), 1);
		EXPECT_EQ(settings.GetKeyValueList("EmuCore/Speedhacks").size(), 2u);
	}

	class InvalidLaunchSetting : public testing::TestWithParam<const char*>
	{
	};

	TEST_P(InvalidLaunchSetting, RejectsMalformedArgumentWithoutChangingOverrides)
	{
		LayeredSettingsInterface settings;
		ASSERT_TRUE(settings.SetCommandLineSetting("Section/Key=original"));
		Error error;
		EXPECT_FALSE(settings.SetCommandLineSetting(GetParam(), &error));
		EXPECT_NE(error.GetDescription().find("-setting requires <Section>/<Key>=<Value>"), std::string::npos);
		EXPECT_EQ(settings.GetStringValue("Section", "Key"), "original");
		EXPECT_EQ(settings.GetLayer(LayeredSettingsInterface::LAYER_CMDLINE)->GetKeyValueList("Section").size(), 1u);
	}

	INSTANTIATE_TEST_SUITE_P(Parsing, InvalidLaunchSetting,
		testing::Values("", "Section/Key", "Section=Value", "=Value", "/Key=Value", "Section/=Value",
			"Section/Key=bad\nvalue", "Section/Key=bad\rvalue", "-batch", "Key=path/with/slashes"));

	TEST(LaunchSetting, RejectsEmbeddedNull)
	{
		LayeredSettingsInterface settings;
		Error error;
		constexpr char argument[] = "Section/Key=bad\0value";
		EXPECT_FALSE(settings.SetCommandLineSetting(std::string_view(argument, sizeof(argument) - 1), &error));
		EXPECT_FALSE(error.GetDescription().empty());
	}

	// Capture/replay determinism still needs proper testing with actual recorded takes.
	TEST(LaunchSetting, OverridesBaseAndGameSettingsAcrossReloadsWithoutBeingSaved)
	{
		const std::filesystem::path directory = std::filesystem::current_path() /
		                                        ("launch-setting-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ASSERT_TRUE(std::filesystem::create_directory(directory));
		INISettingsInterface base((directory / "PCSX2.ini").string());
		INISettingsInterface game((directory / "game.ini").string());
		base.SetIntValue("EmuCore/Speedhacks", "EECycleRate", -1);
		game.SetIntValue("EmuCore/Speedhacks", "EECycleRate", 2);
		base.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", 1);
		Error error;
		ASSERT_TRUE(base.Save(&error)) << error.GetDescription();
		ASSERT_TRUE(game.Save(&error)) << error.GetDescription();

		LayeredSettingsInterface settings;
		// Parse before the persisted layers are loaded, as at process startup.
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/EECycleRate=0"));
		ASSERT_TRUE(settings.SetCommandLineSetting("EmuCore/Speedhacks/vuThread=false"));
		settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
		for (int reload = 0; reload < 2; reload++)
		{
			ASSERT_TRUE(base.Load());
			ASSERT_TRUE(game.Load());
			settings.SetLayer(LayeredSettingsInterface::LAYER_GAME, nullptr);
			EXPECT_EQ(settings.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), 0);
			settings.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
			SettingsLoadWrapper wrapper(settings);
			Pcsx2Config::SpeedhackOptions speedhacks;
			speedhacks.LoadSave(wrapper);
			EXPECT_EQ(speedhacks.EECycleRate, 0);
			EXPECT_EQ(speedhacks.EECycleSkip, 1);
			EXPECT_FALSE(speedhacks.vuThread);
			EXPECT_EQ(base.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), -1);
			EXPECT_EQ(game.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), 2);
			// Exercise real writes, rather than read-only-settings suppressing them.
			base.SetBoolValue("UI", "StartPaused", true);
			game.SetBoolValue("EmuCore", "EnableCheats", true);
			ASSERT_TRUE(base.Save(&error)) << error.GetDescription();
			ASSERT_TRUE(game.Save(&error)) << error.GetDescription();
			ASSERT_TRUE(base.Load());
			ASSERT_TRUE(game.Load());
			EXPECT_FALSE(base.ContainsValue("EmuCore/Speedhacks", "vuThread"));
			EXPECT_FALSE(game.ContainsValue("EmuCore/Speedhacks", "vuThread"));
			EXPECT_EQ(base.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), -1);
			EXPECT_EQ(game.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), 2);
		}

		LayeredSettingsInterface next_process;
		next_process.SetLayer(LayeredSettingsInterface::LAYER_BASE, &base);
		EXPECT_EQ(next_process.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), -1);
		next_process.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game);
		EXPECT_EQ(next_process.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 99), 2);
		EXPECT_FALSE(settings.GetLayer(LayeredSettingsInterface::LAYER_CMDLINE)->Save(&error));
		EXPECT_FALSE(error.GetDescription().empty());
	}
} // namespace
