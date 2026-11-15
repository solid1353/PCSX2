// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Widescreen.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
	class NA228WidescreenTest : public testing::Test
	{
	protected:
		static constexpr uint32_t SEGMENT_OFFSET = 0x100;
		static constexpr uint32_t SEGMENT_ADDRESS = 0x00100000;
		static constexpr uint32_t REFRESH_CAMERA = SEGMENT_OFFSET + 0x0010dc58 - SEGMENT_ADDRESS;
		std::vector<uint8_t> elf = std::vector<uint8_t>(0x10000);

		void Write(uint32_t offset, uint32_t value, uint32_t size)
		{
			for (uint32_t i = 0; i < size; i++)
				elf.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
		}

		void SetUp() override
		{
			Write(0x1c, 0x34, 4);
			Write(0x2c, 1, 2);
			Write(0x34 + 0x04, SEGMENT_OFFSET, 4);
			Write(0x34 + 0x08, SEGMENT_ADDRESS, 4);
			Write(0x34 + 0x10, static_cast<uint32_t>(elf.size()) - SEGMENT_OFFSET, 4);
			const uint8_t retail[] = {
				0x06, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x44, 0x00, 0x00, 0x00, 0x00,
				0x60, 0x00, 0x80, 0x46, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x42, 0x18, 0x02, 0x00,
				0x01, 0x00, 0x42, 0x30, 0x25, 0x18, 0x62, 0x00, 0x00, 0x00, 0x83, 0x44, 0x00, 0x00, 0x00, 0x00,
				0x60, 0x00, 0x80, 0x46, 0x40, 0x08, 0x01, 0x46};
			std::copy(std::begin(retail), std::end(retail), elf.begin() + REFRESH_CAMERA);
		}
	};

	TEST_F(NA228WidescreenTest, RetailCameraRefreshIsNotWidescreen)
	{
		EXPECT_FALSE(NA228Widescreen::Detect(elf));
	}

	TEST_F(NA228WidescreenTest, RewrittenCameraRefreshIsWidescreen)
	{
		elf.at(REFRESH_CAMERA + 0x37) ^= 0xff;
		EXPECT_TRUE(NA228Widescreen::Detect(elf));
	}

	TEST_F(NA228WidescreenTest, UnmappedCameraRefreshIsNotWidescreen)
	{
		Write(0x34 + 0x08, 0x00200000, 4);
		elf.at(REFRESH_CAMERA) ^= 0xff;
		EXPECT_FALSE(NA228Widescreen::Detect(elf));
	}
} // namespace
