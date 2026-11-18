// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/CaptureScript.h"
#include "Recording/PadData.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace
{
	using namespace CaptureScript;
	constexpr auto CAPTURE_OK = [](const std::string&) { return true; };
	constexpr auto STOP_OK = []() { return true; };

	class CaptureScriptTest : public testing::Test
	{
	protected:
		std::optional<Runner> MakeRunner(std::string_view script)
		{
			std::string error;
			auto program = Parse(script, &error);
			EXPECT_TRUE(program) << error;
			if (!program)
				return std::nullopt;
			return Runner(std::move(*program));
		}
		void Write(u32 address, u32 value)
		{
			for (unsigned i = 0; i < 4; i++)
				m_ram[address + i] = static_cast<u8>(value >> (8 * i));
		}
		bool Evaluate(std::string_view condition)
		{
			auto runner = MakeRunner("wait " + std::string(condition) + " within 1");
			if (!runner || !runner->Start(CAPTURE_OK, STOP_OK))
				return false;
			return runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK) && runner->IsComplete();
		}
		std::array<u8, 1024> m_ram{};
	};
} // namespace

TEST(CaptureScriptParser, ParsesTrailerExamples)
{
	const char* scripts[] = {
		"wait [[0x00607604]+8]>>16 == 4 within 7200",
		"wait [[[0x00607620]+0x34]] != 0 && [[[0x00607620]+0x34]] != 1 && [[[0x00607620]+0x34]] != 6 within 7200",
		"wait [[0x00607604]+8]>>16 == 4 within 7200; run 60; capture D:\\out\\take.mkv; run 30; hold cross; run 4; hold none; run 300; stop",
		"hold cross+circle+square+triangle+up+down+left+right+l1+l2+l3+r1+r2+r3+start+select; run 0x10; hold none;",
		"capture \"/out/a shot.mkv\"; run 1; stop; capture /out/b.mkv; run 1; stop",
	};
	for (const char* script : scripts)
	{
		std::string error;
		EXPECT_TRUE(Parse(script, &error)) << script << ": " << error;
	}
}

TEST(CaptureScriptParser, RejectsMalformedScriptsBeforeExecution)
{
	const char* scripts[] = {
		"",
		" ",
		";run 1",
		"run",
		"run -1",
		"run 18446744073709551616",
		"run 1 trailing",
		"run 1;;run 1",
		"wait [8] == 1 within 0",
		"wait [8] == 1",
		"wait [8] = 1 within 1",
		"wait [8 == 1 within 1",
		"wait [0x100000000] == 1 within 1",
		"wait [[8]+0xffffffff+1] == 1 within 1",
		"wait [8]>>32 == 1 within 1",
		"wait [8]&0x100000000 == 1 within 1",
		"wait [8]>>1>>1 == 1 within 1",
		"wait [8]&1&2 == 1 within 1",
		"wait [8] == 1 || within 1",
		"wait [8] == 0x within 1",
		"hold invalid",
		"hold cross+",
		"hold none+cross",
		"hold cross run 1",
		"sticks",
		"sticks 0 1 2",
		"sticks 0 1 2 256",
		"sticks -1 127 127 127",
		"sticks 0 1 2 3 4",
		"stop",
		"capture a.mp4; run 1; stop",
		"capture a.mkv",
		"capture a.mkv; capture b.mkv; run 1; stop",
		"fly 1",
	};
	for (const char* script : scripts)
	{
		std::string error;
		EXPECT_FALSE(Parse(script, &error)) << script;
		EXPECT_FALSE(error.empty()) << script;
	}
	std::string error;
	EXPECT_FALSE(Parse("wait " + std::string(65, '[') + "8" + std::string(65, ']') + " == 1 within 1", &error));
	EXPECT_NE(error.find("nesting"), std::string::npos);
}

TEST_F(CaptureScriptTest, ReadsWidthsSignedValuesShiftsMasksAndUnalignedAddresses)
{
	Write(17, 0x89abfedc);
	EXPECT_TRUE(Evaluate("[17] == 0x89abfedc"));
	EXPECT_TRUE(Evaluate("h[17] == 0xfedc"));
	EXPECT_TRUE(Evaluate("sh[17] == -292"));
	EXPECT_TRUE(Evaluate("b[18] == 0xfe"));
	EXPECT_TRUE(Evaluate("[17]>>16&0xff == 0xab"));
	EXPECT_TRUE(Evaluate("[17]&0xffff0000>>16 == 0x89ab"));
	EXPECT_TRUE(Evaluate("sh[17]>>1 == -146"));
	EXPECT_TRUE(Evaluate("sh[17]&0xffff == 0xfedc"));
	EXPECT_TRUE(Evaluate("[17] != 0"));
	EXPECT_TRUE(Evaluate("[17] < 0x89abfedd"));
	EXPECT_TRUE(Evaluate("[17] <= 0x89abfedc"));
	EXPECT_TRUE(Evaluate("[17] > 0x89abfedb"));
	EXPECT_TRUE(Evaluate("[17] >= 0x89abfedc"));
}

TEST_F(CaptureScriptTest, EvaluatesNestedReadsAndMultipleOffsets)
{
	Write(8, 100);
	Write(100 + 0x34, 200);
	Write(200, 7);
	Write(100 + 8, 4u << 16);
	EXPECT_TRUE(Evaluate("[[8]+4+4]>>16 == 4"));
	EXPECT_TRUE(Evaluate("[[[8]+0x30+4]] != 0 && [[[8]+0x34]] != 1 && [[[8]+0x34]] != 6"));
	EXPECT_TRUE(Evaluate("h[b[8]+8] == 0"));
}

TEST_F(CaptureScriptTest, NullInvalidAndOverflowingPointersMakeEvenNotEqualFalse)
{
	Write(8, 0);
	Write(16, 0xfffffff8);
	EXPECT_FALSE(Evaluate("[[8]+8] != 1"));
	EXPECT_FALSE(Evaluate("[[16]+16] != 1"));
	EXPECT_FALSE(Evaluate("[1021] != 1"));
	EXPECT_FALSE(Evaluate("h[1023] != 1"));
	EXPECT_FALSE(Evaluate("b[1024] != 1"));
	EXPECT_TRUE(Evaluate("b[1023] == 0"));
	EXPECT_TRUE(Evaluate("[0] == 0")); // Literal RAM address zero is not a dereferenced null pointer.
	EXPECT_TRUE(Evaluate("[[8]+8] != 1 || b[1023] == 0"));
}

TEST_F(CaptureScriptTest, AndBindsTighterThanOr)
{
	EXPECT_TRUE(Evaluate("[8] == 0 || [8] == 1 && [8] == 2"));
	EXPECT_FALSE(Evaluate("[8] == 1 || [8] == 0 && [8] == 2"));
	EXPECT_TRUE(Evaluate("[8] == 1 && [8] == 2 || [8] == 0 && [8] != 1"));
}

TEST_F(CaptureScriptTest, WaitChecksCompletedFramesAndAcceptsFinalDeadline)
{
	auto runner = MakeRunner("wait [8] == 1 within 3; hold cross; run 2; hold none");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	for (int i = 0; i < 2; i++)
	{
		EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_EQ(runner->GetButtons(), 0);
	}
	Write(8, 1);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetButtons(), 1u << 14);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_EQ(runner->GetFrameCount(), 5);
}

TEST_F(CaptureScriptTest, WaitAlwaysRunsAtLeastOneFrameAndTimeoutStopsOnDeadline)
{
	auto runner = MakeRunner("wait [8] == 0 within 2; run 0");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 1);

	runner = MakeRunner("wait [8] == 1 within 2; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetFrameCount(), 2);
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_NE(runner->GetError().find("wait timed out"), std::string::npos);
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetFrameCount(), 2);
}

TEST_F(CaptureScriptTest, CapturesExactlyNFramesWithNextFrameInputAndMultipleTakes)
{
	auto runner = MakeRunner("run 2; capture a.mkv; run 30; hold cross; run 4; hold none; run 300; stop; capture b.mkv; run 1; stop");
	ASSERT_TRUE(runner);
	std::vector<std::pair<u64, std::string>> events;
	const auto capture = [&](const std::string& path) { events.emplace_back(runner->GetFrameCount(), path); return true; };
	const auto stop = [&]() { events.emplace_back(runner->GetFrameCount(), "stop"); return true; };
	ASSERT_TRUE(runner->Start(capture, stop));
	u64 video_frames = 0;
	std::vector<u64> pressed_frames;
	for (u64 frame = 1; !runner->IsComplete() && frame < 1000; frame++)
	{
		if (runner->IsCapturing())
			video_frames++;
		if (runner->GetButtons())
			pressed_frames.push_back(frame);
		ASSERT_TRUE(runner->EndFrame(m_ram, capture, stop)) << runner->GetError();
	}
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 337);
	EXPECT_EQ(video_frames, 335); // First file: 334; second: 1.
	EXPECT_EQ(pressed_frames, (std::vector<u64>{33, 34, 35, 36}));
	EXPECT_EQ(events, (std::vector<std::pair<u64, std::string>>{{2, "a.mkv"}, {336, "stop"}, {336, "b.mkv"}, {337, "stop"}}));
}

TEST_F(CaptureScriptTest, CaptureAndFinalizationErrorsAbortRemainingStatements)
{
	auto runner = MakeRunner("capture a.mkv; run 1; stop; hold cross");
	ASSERT_TRUE(runner);
	EXPECT_FALSE(runner->Start([](const auto&) { return false; }, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_NE(runner->GetError().find("could not start capture"), std::string::npos);

	runner = MakeRunner("capture a.mkv; run 1; stop; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, []() { return false; }));
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_NE(runner->GetError().find("could not finalize capture"), std::string::npos);
}

TEST_F(CaptureScriptTest, RawSticksPersistIndependentlyOfHeldButtonsAtFrameBoundaries)
{
	auto runner = MakeRunner("run 1; sticks 0 255 64 192; hold cross; run 2; hold none; run 1; sticks 127 127 127 127; run 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{127, 127, 127, 127}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{0, 255, 64, 192}));
	EXPECT_EQ(runner->GetButtons(), 1u << 14);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{0, 255, 64, 192}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{0, 255, 64, 192}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{127, 127, 127, 127}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 5);
}

TEST_F(CaptureScriptTest, CopiesRecordedStickBytesWithoutConversionOrReordering)
{
	const PadData recorded(0, 0, {0xff, 0xff, 19, 237, 0, 255});
	const auto bytes = recorded.ToArray();
	auto runner = MakeRunner("sticks " + std::to_string(bytes[2]) + " " + std::to_string(bytes[3]) + " " +
							 std::to_string(bytes[4]) + " " + std::to_string(bytes[5]) + "; run 1; sticks 0xff 0x00 0x7f 0x80");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{19, 237, 0, 255}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{255, 0, 127, 128}));
	EXPECT_TRUE(runner->IsComplete());
}
