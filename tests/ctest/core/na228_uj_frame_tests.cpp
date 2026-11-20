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
			Write(0x6029c8, 0x008dd080);
			Write(0x8dd080, 0x336f574d);
			Write(0x8dd088, 0x008dd080);
			Write(0x8dd0a0, 0x2e383232);
			Write(0x8dd0a4, 0x006e6962);
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
		Write(0x8dd080, 0);
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
	TEST_F(NA228UJFrameTest, ReadsFilmNameAndFighters)
	{
		const char name[] = "skill_uj_naruto";
		for (uint32_t i = 0; i < sizeof(name); i++)
			Write(CONTAINER + 0x04 + i, static_cast<uint8_t>(name[i]), 1);
		Write(OWNER + 0x36, 12, 2);
		Write(OWNER + 0x38, 0xffff, 2);
		const auto film = NA228UJFrame::Read(ram);
		ASSERT_TRUE(film.has_value());
		EXPECT_EQ(film->name, "skill_uj_naruto");
		EXPECT_EQ(film->part, std::optional<int16_t>(2));
		EXPECT_EQ(film->frame, 118);
		EXPECT_EQ(film->skill, SKILL);
		EXPECT_EQ(film->attacker, 12);
		EXPECT_EQ(film->opponent, -1);
		EXPECT_EQ(NA228UJFrame::FormatPosition(*film), "2:118");
	}

	TEST_F(NA228UJFrameTest, LimitsFilmNameTo32Bytes)
	{
		for (uint32_t i = 0; i < 40; i++)
			Write(CONTAINER + 0x04 + i, 'a', 1);
		EXPECT_EQ(NA228UJFrame::Read(ram)->name, std::string(32, 'a'));
	}

	TEST(NA228UJFrameMarks, WritesHeaderAndFirstMarkToEmptyFile)
	{
		NA228UJFrame::Film film{"skill_uj", 2, 118, 3, 12, -1};
		uint32_t mark = 0;
		EXPECT_EQ(NA228UJFrame::MarkText("", film, "2026-11-20 10:00:00", "NA2.28 - dev", &mark),
			"mark\ttime\tbuild\tfilm\tpart\tframe\tskill\tattacker\topponent\n"
			"1\t2026-11-20 10:00:00\tNA2.28 - dev\tskill_uj\t2\t118\t3\t12\t-1\n");
		EXPECT_EQ(mark, 1u);
	}

	TEST(NA228UJFrameMarks, NumbersAboveHighestMarkAndLeavesPartEmptyForSinglePartFilm)
	{
		NA228UJFrame::Film film{"skill_uj", std::nullopt, 7, 3, 1, 2};
		uint32_t mark = 0;
		const std::string contents = "mark\ttime\n4\ta\n9\tb\n2\tc";
		EXPECT_EQ(NA228UJFrame::MarkText(contents, film, "t", "b", &mark), "\n10\tt\tb\tskill_uj\t\t7\t3\t1\t2\n");
		EXPECT_EQ(mark, 10u);
	}

	class SourceGameUJFrameTest : public testing::Test
	{
	protected:
		static constexpr uint32_t OWNER = 0x00700000;
		static constexpr uint32_t CONTAINER = 0x00710000;
		std::vector<uint8_t> ram = std::vector<uint8_t>(NA228ResourceMonitor::EE_RAM_SIZE);

		void Write(uint32_t address, uint32_t value, uint32_t size = 4)
		{
			for (uint32_t i = 0; i < size; i++)
				ram.at(address + i) = static_cast<uint8_t>(value >> (i * 8));
		}

		void SetFilm(uint32_t owner_slot, uint32_t parts_begun, uint32_t frame_offset, int32_t frame)
		{
			Write(owner_slot, OWNER);
			Write(OWNER + parts_begun, 1, 2);
			Write(OWNER + 0x04, CONTAINER);
			Write(CONTAINER + frame_offset, static_cast<uint32_t>(frame));
		}
	};

	TEST_F(SourceGameUJFrameTest, ShowsUN3FilmFrame)
	{
		SetFilm(0x006a3a18, 0x86, 0x1ec, 231);
		EXPECT_EQ(NA228UJFrame::Format(ram, NA228UJFrame::UN3_DISC_CRC), "UJ frame 231");
		EXPECT_EQ(NA228UJFrame::Format(ram), "");
	}

	TEST_F(SourceGameUJFrameTest, ShowsUN4FilmFrame)
	{
		SetFilm(0x00609800, 0x16a, 0x1bc, 50);
		EXPECT_EQ(NA228UJFrame::Format(ram, NA228UJFrame::UN4_DISC_CRC), "UJ frame 50");
	}

	TEST_F(SourceGameUJFrameTest, ShowsNothingBeforeTheFilmBegins)
	{
		SetFilm(0x00609800, 0x16a, 0x1bc, 50);
		Write(OWNER + 0x16a, 0, 2);
		EXPECT_EQ(NA228UJFrame::Format(ram, NA228UJFrame::UN4_DISC_CRC), "");
		Write(0x00609800, 0);
		EXPECT_EQ(NA228UJFrame::Format(ram, NA228UJFrame::UN4_DISC_CRC), "");
	}
} // namespace
