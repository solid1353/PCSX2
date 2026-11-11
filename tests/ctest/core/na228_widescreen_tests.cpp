// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228ResourceMonitor.h"
#include "NA228Widescreen.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
	class NA228WidescreenTest : public testing::Test
	{
	protected:
		static constexpr uint32_t REFRESH_CAMERA = 0x0010dc58;
		std::vector<uint8_t> ram = std::vector<uint8_t>(NA228ResourceMonitor::EE_RAM_SIZE);

		void Write32(uint32_t address, uint32_t value)
		{
			for (uint32_t i = 0; i < 4; i++)
				ram.at(address + i) = static_cast<uint8_t>(value >> (i * 8));
		}

		void SetUp() override
		{
			Write32(0x8f3d00, 0x336f574d);
			Write32(0x8f3d08, 0x008f3d00);
			Write32(0x8f3d20, 0x2e383232);
			Write32(0x8f3d24, 0x006e6962);
			const uint8_t retail[] = {
				0x06, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x44, 0x00, 0x00, 0x00, 0x00,
				0x60, 0x00, 0x80, 0x46, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x42, 0x18, 0x02, 0x00,
				0x01, 0x00, 0x42, 0x30, 0x25, 0x18, 0x62, 0x00, 0x00, 0x00, 0x83, 0x44, 0x00, 0x00, 0x00, 0x00,
				0x60, 0x00, 0x80, 0x46, 0x40, 0x08, 0x01, 0x46};
			std::copy(std::begin(retail), std::end(retail), ram.begin() + REFRESH_CAMERA);
		}
	};

	TEST_F(NA228WidescreenTest, RetailCameraRefreshIsNotWidescreen)
	{
		EXPECT_EQ(NA228Widescreen::Detect(ram), std::optional<bool>(false));
	}

	TEST_F(NA228WidescreenTest, RewrittenCameraRefreshIsWidescreen)
	{
		ram.at(REFRESH_CAMERA + 0x37) ^= 0xff;
		EXPECT_EQ(NA228Widescreen::Detect(ram), std::optional<bool>(true));
	}

	TEST_F(NA228WidescreenTest, UnknownBefore228BinLoads)
	{
		Write32(0x8f3d00, 0);
		ram.at(REFRESH_CAMERA) ^= 0xff;
		EXPECT_EQ(NA228Widescreen::Detect(ram), std::nullopt);
	}
} // namespace
