// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"
#include "common/Image.h"
#include <zlib.h>

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
	EXPECT_EQ(options.ScreenshotPNGCompression, 9);
	EXPECT_EQ(options.ScreenshotJPEGQuality, 100);
	EXPECT_EQ(options.ScreenshotWebPQuality, 100);
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

TEST(ScreenshotSettings, FormatValuesRemainIndependentAcrossSaveAndReload)
{
	MemorySettingsInterface settings;
	Pcsx2Config::GSOptions original;
	original.ScreenshotPNGCompression = 0;
	original.ScreenshotJPEGQuality = 75;
	original.ScreenshotWebPQuality = 95;
	SettingsSaveWrapper saver(settings);
	original.LoadSave(saver);
	Pcsx2Config::GSOptions loaded;
	SettingsLoadWrapper loader(settings);
	loaded.LoadSave(loader);
	EXPECT_EQ(loaded.ScreenshotPNGCompression, 0);
	EXPECT_EQ(loaded.ScreenshotJPEGQuality, 75);
	EXPECT_EQ(loaded.ScreenshotWebPQuality, 95);
	EXPECT_TRUE(original.OptionsAreEqual(loaded));
	loaded.ScreenshotPNGCompression = 9;
	EXPECT_FALSE(original.OptionsAreEqual(loaded));
}

TEST(ScreenshotEncoding, PNGCompressionPreservesEveryPixel)
{
	RGBA8Image image(128, 96);
	for (u32 y = 0; y < image.GetHeight(); y++)
	{
		for (u32 x = 0; x < image.GetWidth(); x++)
			image.GetPixels()[y * image.GetWidth() + x] = 0xff000000u | ((x / 8) << 16) | ((y / 8) << 8) | (x + y);
	}
	size_t uncompressed_size = 0;
	for (u8 level : {0, 1, 5, 9})
	{
		const auto encoded = image.SaveToBuffer("screenshot.png", level * 10);
		ASSERT_TRUE(encoded.has_value());
		RGBA8Image decoded;
		ASSERT_TRUE(decoded.LoadFromBuffer("screenshot.png", encoded->data(), encoded->size()));
		ASSERT_EQ(decoded.GetWidth(), image.GetWidth());
		ASSERT_EQ(decoded.GetHeight(), image.GetHeight());
		for (u32 i = 0; i < image.GetWidth() * image.GetHeight(); i++)
			ASSERT_EQ(decoded.GetPixels()[i], image.GetPixels()[i]);
		if (level == 0)
			uncompressed_size = encoded->size();
		else
			EXPECT_LT(encoded->size(), uncompressed_size);
	}
#ifdef _WIN32
	EXPECT_NE(std::string_view(zlibVersion()).find("zlib-ng"), std::string_view::npos);
#endif
}
