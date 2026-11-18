// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "Recording/InputRecordingFile.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace
{
	class SavestatesOverrideTest : public testing::Test
	{
	protected:
		std::string old_data_root = EmuFolders::DataRoot;
		std::string old_savestates = EmuFolders::Savestates;
		MemorySettingsInterface si;

		void SetUp() override
		{
			EmuFolders::DataRoot = "data";
			si.SetStringValue("Folders", "Savestates", "configured");
		}

		void TearDown() override
		{
			EmuFolders::SetSavestatesOverride({});
			EmuFolders::DataRoot = old_data_root;
			EmuFolders::Savestates = old_savestates;
		}
	};

	TEST_F(SavestatesOverrideTest, ReplacesConfiguredFolderOnEveryLoad)
	{
		EmuFolders::SetSavestatesOverride("override");
		EmuFolders::LoadConfig(si);
		EXPECT_EQ(EmuFolders::Savestates, "override");
		EmuFolders::LoadConfig(si);
		EXPECT_EQ(EmuFolders::Savestates, "override");
		EXPECT_EQ(si.GetStringValue("Folders", "Savestates"), "configured");
	}

	TEST_F(SavestatesOverrideTest, ConfiguredFolderAppliesWithoutOverride)
	{
		EmuFolders::LoadConfig(si);
		EXPECT_EQ(EmuFolders::Savestates, Path::Combine("data", "configured"));
	}

	class InputRecordingsFolderTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			for (std::string* folder : {&EmuFolders::DataRoot, &EmuFolders::Settings, &EmuFolders::Bios,
					 &EmuFolders::Snapshots, &EmuFolders::GSDumps, &EmuFolders::Prints, &EmuFolders::Savestates,
					 &EmuFolders::MemoryCards, &EmuFolders::Logs, &EmuFolders::Cheats, &EmuFolders::Patches,
					 &EmuFolders::Covers, &EmuFolders::GameSettings, &EmuFolders::UserResources, &EmuFolders::Cache,
					 &EmuFolders::Textures, &EmuFolders::InputProfiles, &EmuFolders::InputRecordings,
					 &EmuFolders::Videos, &EmuFolders::DebuggerLayouts, &EmuFolders::DebuggerSettings})
			{
				m_old_folders.emplace_back(folder, *folder);
			}
			m_directory = std::filesystem::current_path() /
			              ("input-recordings-folder-test-" +
							  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			ASSERT_TRUE(std::filesystem::create_directories(m_directory));
			EmuFolders::DataRoot = m_directory.string();
			EmuFolders::Settings = (m_directory / "inis").string();
			EmuFolders::SetDefaults(m_settings);
			m_settings.SetStringValue("Folders", "InputRecordings", "configured");
		}

		void TearDown() override
		{
			EmuFolders::SetInputRecordingsOverride({});
			for (auto& [folder, old_value] : m_old_folders)
				*folder = std::move(old_value);
			EmuFolders::AdditionalContentFolders = m_old_content_folders;
			EmuFolders::ContentAliases = m_old_aliases;
			std::error_code error;
			std::filesystem::remove_all(m_directory, error);
		}

		std::filesystem::path m_directory;
		MemorySettingsInterface m_settings;
		std::vector<std::pair<std::string*, std::string>> m_old_folders;
		const std::vector<std::string> m_old_content_folders = EmuFolders::AdditionalContentFolders;
		const std::vector<std::pair<std::string, std::string>> m_old_aliases = EmuFolders::ContentAliases;
	};

	TEST_F(InputRecordingsFolderTest, OverrideSurvivesReloadsWithoutChangingSettingsOrCreatingFolders)
	{
		const std::string override_path = (m_directory / "command" / "recordings").string();
		EmuFolders::SetInputRecordingsOverride(override_path);
		for (int reload = 0; reload < 2; reload++)
		{
			EmuFolders::LoadConfig(m_settings);
			EXPECT_EQ(EmuFolders::InputRecordings, override_path);
			EXPECT_EQ(m_settings.GetStringValue("Folders", "InputRecordings"), "configured");
			ASSERT_TRUE(EmuFolders::EnsureFoldersExist());
			EXPECT_FALSE(std::filesystem::exists(override_path));
			EXPECT_FALSE(std::filesystem::exists(m_directory / "command"));
			EXPECT_FALSE(std::filesystem::exists(m_directory / "configured"));
		}
		EXPECT_EQ(EmuFolders::ResolveInputRecordingPath("take.p2m2"), Path::Combine(override_path, "take.p2m2"));
		const std::string absolute = (m_directory / "elsewhere" / "take.p2m2").string();
		EXPECT_EQ(EmuFolders::ResolveInputRecordingPath(absolute), absolute);

		EmuFolders::SetInputRecordingsOverride({});
		EmuFolders::LoadConfig(m_settings);
		EXPECT_EQ(EmuFolders::InputRecordings, (m_directory / "configured").string());
	}

	TEST_F(InputRecordingsFolderTest, ConfiguredFolderIsCreatedOnlyWhenWriting)
	{
		EmuFolders::LoadConfig(m_settings);
		ASSERT_TRUE(EmuFolders::EnsureFoldersExist());
		const std::string filename = EmuFolders::ResolveInputRecordingPath("take.p2m2");
		InputRecordingFile recording;
		EXPECT_FALSE(recording.openExisting(filename));
		EXPECT_FALSE(std::filesystem::exists(EmuFolders::InputRecordings));
		ASSERT_TRUE(recording.openNew(filename, false));
		ASSERT_TRUE(recording.writeHeader());
		ASSERT_TRUE(recording.close());
		EXPECT_TRUE(std::filesystem::is_regular_file(filename));
		ASSERT_TRUE(recording.openExisting(filename));
	}

	TEST_F(InputRecordingsFolderTest, OverrideCreatesMissingParentsOnlyWhenWriting)
	{
		const std::string override_path = (m_directory / "command" / "nested" / "recordings").string();
		EmuFolders::SetInputRecordingsOverride(override_path);
		EmuFolders::LoadConfig(m_settings);
		ASSERT_TRUE(EmuFolders::EnsureFoldersExist());
		EXPECT_FALSE(std::filesystem::exists(override_path));
		const std::string filename = EmuFolders::ResolveInputRecordingPath("take.p2m2");
		InputRecordingFile recording;
		ASSERT_TRUE(recording.openNew(filename, InputRecordingFile::StartType::None));
		ASSERT_TRUE(recording.writeHeader());
		ASSERT_TRUE(recording.close());
		EXPECT_TRUE(std::filesystem::is_regular_file(filename));
		EXPECT_FALSE(std::filesystem::exists(m_directory / "configured"));
	}
} // namespace
