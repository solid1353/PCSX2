// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228ResourceMonitor.h"
#include "NA228UJFrame.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	class NA228UJFrameTest : public testing::Test
	{
	protected:
		static constexpr uint32_t OWNER = 0x00700000;
		static constexpr uint32_t CONTAINER = 0x00710000;
		static constexpr uint32_t REQUESTS = 0x00720000;
		static constexpr uint32_t SKILL = 3;
		std::vector<uint8_t> ram = std::vector<uint8_t>(NA228ResourceMonitor::EE_RAM_SIZE);

		void Write(uint32_t address, uint32_t value, uint32_t size = 4)
		{
			for (uint32_t i = 0; i < size; i++)
				ram.at(address + i) = static_cast<uint8_t>(value >> (i * 8));
		}

		void SetUp() override
		{
			Write(0x8f3d00, 0x336f574d);
			Write(0x8f3d08, 0x008f3d00);
			Write(0x8f3d20, 0x2e383232);
			Write(0x8f3d24, 0x006e6962);
			Write(0x00607714, OWNER);
			Write(OWNER + 0x04, CONTAINER);
			Write(OWNER + 0x3c, SKILL, 2);
			Write(OWNER + 0x16a, 2, 2);
			Write(CONTAINER + 0x90, 118);
			Write(0x00607748, REQUESTS);
			Write(REQUESTS + SKILL * 0x18 + 0x09, 3, 1);
		}

		std::string Format() const { return NA228UJFrame::Format(ram); }
	};

	TEST_F(NA228UJFrameTest, ShowsPartAndFrameForMultiPartFilm)
	{
		EXPECT_EQ(Format(), "UJ frame 2:118");
	}

	TEST_F(NA228UJFrameTest, ShowsOnlyFrameForSinglePartFilm)
	{
		Write(REQUESTS + SKILL * 0x18 + 0x09, 1, 1);
		EXPECT_EQ(Format(), "UJ frame 118");
	}

	TEST_F(NA228UJFrameTest, ShowsOnlyFrameWithoutRequestTable)
	{
		Write(0x00607748, 0);
		EXPECT_EQ(Format(), "UJ frame 118");
	}

	TEST_F(NA228UJFrameTest, AcceptsKseg0Pointers)
	{
		Write(0x00607714, OWNER | 0x80000000);
		Write(OWNER + 0x04, CONTAINER | 0x80000000);
		EXPECT_EQ(Format(), "UJ frame 2:118");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingOutsideNA228)
	{
		Write(0x8f3d00, 0);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingWithoutFilm)
	{
		Write(0x00607714, 0);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingBeforeFirstPart)
	{
		Write(OWNER + 0x16a, 0, 2);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingWithoutContainer)
	{
		Write(OWNER + 0x04, 0);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingForOwnerOutsideEERam)
	{
		Write(0x00607714, 0x02000000);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingWhenFrameFallsOutsideEERam)
	{
		Write(OWNER + 0x04, 0x01fffff0);
		EXPECT_EQ(Format(), "");
	}

	TEST_F(NA228UJFrameTest, ShowsNothingForRequestTableOutsideEERam)
	{
		Write(0x00607748, 0x02000000);
		EXPECT_EQ(Format(), "");
	}
} // namespace
