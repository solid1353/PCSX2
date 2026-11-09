// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"

#include <gtest/gtest.h>

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
} // namespace
