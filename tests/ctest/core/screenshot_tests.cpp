// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

TEST(ScreenshotSettings, EmptySettingsSelect1000By750)
{
	MemorySettingsInterface settings;
	SettingsLoadWrapper loader(settings);
	Pcsx2Config::GSOptions options;
	options.LoadSave(loader);
	EXPECT_EQ(options.ScreenshotSize, GSScreenshotSize::CustomResolution);
	EXPECT_EQ(options.ScreenshotWidth, 1000);
	EXPECT_EQ(options.ScreenshotHeight, 750);
}

TEST(ScreenshotSettings, CustomDimensionsSurviveSaveAndReload)
{
	MemorySettingsInterface settings;
	Pcsx2Config::GSOptions original;
	original.ScreenshotWidth = 1920;
	original.ScreenshotHeight = 1080;
	SettingsSaveWrapper saver(settings);
	original.LoadSave(saver);

	Pcsx2Config::GSOptions loaded;
	SettingsLoadWrapper loader(settings);
	loaded.LoadSave(loader);
	EXPECT_EQ(loaded.ScreenshotSize, GSScreenshotSize::CustomResolution);
	EXPECT_EQ(loaded.ScreenshotWidth, 1920);
	EXPECT_EQ(loaded.ScreenshotHeight, 1080);
	EXPECT_TRUE(original.OptionsAreEqual(loaded));
	loaded.ScreenshotWidth = 1280;
	EXPECT_FALSE(original.OptionsAreEqual(loaded));
	loaded.ScreenshotWidth = original.ScreenshotWidth;
	loaded.ScreenshotHeight = 720;
	EXPECT_FALSE(original.OptionsAreEqual(loaded));
}

TEST(ScreenshotSettings, InvalidDimensionsCannotProduceAnEmptyOrUnboundedCapture)
{
	MemorySettingsInterface settings;
	settings.SetIntValue("EmuCore/GS", "ScreenshotWidth", -1);
	settings.SetIntValue("EmuCore/GS", "ScreenshotHeight", 1000000);
	SettingsLoadWrapper loader(settings);
	Pcsx2Config::GSOptions options;
	options.LoadSave(loader);
	EXPECT_EQ(options.ScreenshotWidth, 1);
	EXPECT_EQ(options.ScreenshotHeight, Pcsx2Config::GSOptions::MAX_SCREENSHOT_DIMENSION);
}
