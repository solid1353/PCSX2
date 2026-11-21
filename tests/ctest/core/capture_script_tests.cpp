// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/CaptureScript.h"
#include "Recording/PadData.h"
#include "common/FileSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
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
			if (!runner || !runner->Start(m_ram, CAPTURE_OK, STOP_OK))
				return false;
			return runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK) && runner->IsComplete();
		}
		std::array<u8, 1024> m_ram{};
	};

	class CaptureScriptLoadTest : public CaptureScriptTest
	{
	protected:
		void SetUp() override
		{
			m_directory = "capture-script-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			ASSERT_TRUE(std::filesystem::create_directory(m_directory));
			m_path = (std::filesystem::path(m_directory) / "memory state.txt").string();
		}
		void TearDown() override
		{
			std::error_code error;
			std::filesystem::remove_all(m_directory, error);
		}
		std::string m_directory;
		std::string m_path;
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
		"load state.txt; wait [0x100] changes within 120",
		"wait sh[[8]+4]>>1&0xffff changes within 0x10",
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
		"wait [8] changes within 0",
		"wait [8] changes within -1",
		"wait [8] changes",
		"wait [8] changes within",
		"wait [8] changes within 1 trailing",
		"wait [8] changes == 1 within 1",
		"wait [8] == 1 changes within 1",
		"wait [8] changes || [12] changes within 1",
		"wait [8]>>32 changes within 1",
		"wait 8 changes within 1",
		"load",
		"load \"\"",
		"hold invalid",
		"hold cross+",
		"hold none+cross",
		"hold cross run 1",
		"sticks",
		"sticks 0 1 2",
		"sticks 0 1 2 256",
		"sticks -1 127 127 127",
		"sticks 0 1 2 3 4",
		"pad1",
		"pad1 run 1",
		"pad2 hold cross",
		"pad1 pad1 hold cross",
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
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
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
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 1);

	runner = MakeRunner("wait [8] == 1 within 2; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
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
	ASSERT_TRUE(runner->Start(m_ram, capture, stop));
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
	EXPECT_FALSE(runner->Start(m_ram, [](const auto&) { return false; }, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_NE(runner->GetError().find("could not start capture"), std::string::npos);

	runner = MakeRunner("capture a.mkv; run 1; stop; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, []() { return false; }));
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_NE(runner->GetError().find("could not finalize capture"), std::string::npos);
}

TEST_F(CaptureScriptTest, RawSticksPersistIndependentlyOfHeldButtonsAtFrameBoundaries)
{
	auto runner = MakeRunner("run 1; sticks 0 255 64 192; hold cross; run 2; hold none; run 1; sticks 127 127 127 127; run 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
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
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{19, 237, 0, 255}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetSticks(), (std::array<u8, 4>{255, 0, 127, 128}));
	EXPECT_TRUE(runner->IsComplete());
}

TEST_F(CaptureScriptTest, PadPrefixDrivesEachPadIndependently)
{
	auto runner = MakeRunner("pad1 hold start; pad1 sticks 0 1 2 3; hold cross; run 1; pad0 hold none; run 1; pad1 hold none; pad1 sticks 127 127 127 127");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetButtons(0), 1u << 14);
	EXPECT_EQ(runner->GetButtons(1), 1u << 3);
	EXPECT_EQ(runner->GetSticks(0), (std::array<u8, 4>{127, 127, 127, 127}));
	EXPECT_EQ(runner->GetSticks(1), (std::array<u8, 4>{0, 1, 2, 3}));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetButtons(0), 0);
	EXPECT_EQ(runner->GetButtons(1), 1u << 3);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetButtons(1), 0);
	EXPECT_EQ(runner->GetSticks(1), (std::array<u8, 4>{127, 127, 127, 127}));
	EXPECT_TRUE(runner->IsComplete());
}

TEST(CaptureScriptParser, LoadPathsUseCaptureQuotingAndSpaces)
{
	for (const char* script : {"load \"D:\\state files\\fight.txt\"; run 1", "load D:\\state files\\fight.txt   ; run 1"})
	{
		std::string error;
		const auto program = Parse(script, &error);
		ASSERT_TRUE(program) << error;
		ASSERT_EQ(program->size(), 2);
		EXPECT_EQ(program->front().type, Statement::Type::Load);
		EXPECT_EQ(program->front().path, "D:\\state files\\fight.txt");
	}
	std::string error;
	EXPECT_FALSE(Parse(std::string("load a\0b", 8), &error));
}

TEST_F(CaptureScriptLoadTest, LoadsBytesInOrderWithNestedAddressesOffsetsAndWhitespace)
{
	Write(8, 100);
	Write(104, 200);
	ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(),
		"\r\n \t\r\n  0 1234aBcD \r\n 16 + 1 + 2 00fF\n[[8]+4]+2+1 102030\n[8]>>1+2 55\n1023 7e"));
	auto runner = MakeRunner("load \"" + m_path + "\"; run 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK)) << runner->GetError();
	EXPECT_EQ(runner->GetFrameCount(), 0);
	EXPECT_FALSE(runner->IsComplete());
	EXPECT_EQ(m_ram[0], 0x12);
	EXPECT_EQ(m_ram[1], 0x34);
	EXPECT_EQ(m_ram[2], 0xab);
	EXPECT_EQ(m_ram[3], 0xcd);
	EXPECT_EQ(m_ram[19], 0);
	EXPECT_EQ(m_ram[20], 0xff);
	EXPECT_EQ(m_ram[203], 0x10);
	EXPECT_EQ(m_ram[204], 0x20);
	EXPECT_EQ(m_ram[205], 0x30);
	EXPECT_EQ(m_ram[52], 0x55);
	EXPECT_EQ(m_ram[1023], 0x7e);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
}

TEST_F(CaptureScriptLoadTest, InvalidFilesWriteNothingAndReportFileAndLine)
{
	const char* invalid_lines[] = {
		"16",
		"16 ",
		"16 0",
		"16 abc",
		"16 0x12",
		"16 gg",
		"16 12 34",
		"16 12 trailing",
		"16 -12",
		"16 12;",
		"16 12#comment",
		"-1 12",
		"0x 12",
		"[8 12",
		"[8]12",
		"[8]+ 12",
		"[8]>>32 12",
		"[8]>>1>>1 12",
		"0x100000000 12",
		"0xffffffff+1 12",
		"[8]+0xffffffff+1 12",
		"1024 12",
		"1023 1234",
		"0x80000000 12",
		"[12]+8 12",
		"[1021] 12",
		"[16]+16 12",
		"[[12]+8] 12",
	};
	Write(8, 100);
	Write(16, 0xfffffff8);
	const auto before = m_ram;
	for (const char* line : invalid_lines)
	{
		SCOPED_TRACE(line);
		ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "32 aabbccdd\n\n" + std::string(line) + "\n"));
		auto runner = MakeRunner("load \"" + m_path + "\"; hold cross");
		ASSERT_TRUE(runner);
		EXPECT_FALSE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_FALSE(runner->IsComplete());
		EXPECT_EQ(m_ram, before);
		EXPECT_EQ(runner->GetButtons(), 0);
		EXPECT_NE(runner->GetError().find(m_path), std::string::npos);
		EXPECT_NE(runner->GetError().find("line 3"), std::string::npos);
		EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_EQ(runner->GetFrameCount(), 0);
	}
	for (const std::string& line : {std::string("16 12\0", 6), std::string(65, '[') + "8" + std::string(65, ']') + " 12"})
	{
		ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "32 aa\n" + line));
		auto runner = MakeRunner("load " + m_path);
		ASSERT_TRUE(runner);
		EXPECT_FALSE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_EQ(m_ram, before);
	}
}

TEST_F(CaptureScriptLoadTest, MissingAndUnreadableFilesFailWithoutWrites)
{
	const auto before = m_ram;
	for (const std::string& path : {m_path, m_directory})
	{
		auto runner = MakeRunner("load " + path + "; hold cross");
		ASSERT_TRUE(runner);
		EXPECT_FALSE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_EQ(m_ram, before);
		EXPECT_EQ(runner->GetButtons(), 0);
		EXPECT_NE(runner->GetError().find("could not read load file"), std::string::npos);
		EXPECT_NE(runner->GetError().find(path), std::string::npos);
	}
}

TEST_F(CaptureScriptLoadTest, ReadsAndValidatesFileOnlyWhenStatementRuns)
{
	auto runner = MakeRunner("run 1; load " + m_path + "; hold cross; run 1; load " + m_path);
	ASSERT_TRUE(runner); // The file does not exist at parse or start time.
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "0 12"));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(m_ram[0], 0x12);
	EXPECT_EQ(runner->GetButtons(), 1u << 14);
	ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "0 34\n1024 ff"));
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(m_ram[0], 0x12);
	EXPECT_NE(runner->GetError().find("statement 5, frame 2"), std::string::npos);
	EXPECT_NE(runner->GetError().find("write would leave exposed EE RAM"), std::string::npos);
}

TEST_F(CaptureScriptLoadTest, ResolvesPointersBeforeWritesAndAppliesOverlapsInFileOrder)
{
	Write(8, 100);
	ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "8 c8000000\n[8] 1234\n101 ab\n"));
	auto runner = MakeRunner("load " + m_path);
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 0);
	EXPECT_EQ(m_ram[8], 200);
	EXPECT_EQ(m_ram[100], 0x12);
	EXPECT_EQ(m_ram[101], 0xab);
	EXPECT_EQ(m_ram[200], 0);
}

TEST_F(CaptureScriptLoadTest, EmptyAndWhitespaceFilesAreValidNoOps)
{
	const auto before = m_ram;
	for (const char* text : {"", "\r\n \t\n"})
	{
		ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), text));
		auto runner = MakeRunner("load " + m_path);
		ASSERT_TRUE(runner);
		EXPECT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_TRUE(runner->IsComplete());
		EXPECT_EQ(m_ram, before);
	}
}

TEST_F(CaptureScriptTest, ChangeWaitSnapshotsAtEntryAndAcceptsFirstAndLastAllowedFrames)
{
	auto runner = MakeRunner("run 1; wait [8] changes within 3; wait [8] changes within 1; hold cross");
	ASSERT_TRUE(runner);
	Write(8, 99);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 10);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetFrameCount(), 1);
	EXPECT_FALSE(runner->IsComplete());
	for (int i = 0; i < 2; i++)
		ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 11);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsComplete()); // Next wait snapshots 11 and must execute a frame.
	Write(8, 12);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 5);
	EXPECT_EQ(runner->GetButtons(), 1u << 14);
}

TEST_F(CaptureScriptTest, ChangeWaitUsesReadWidthsSignedValuesAndTransforms)
{
	for (const char* read : {"[100]", "h[100]", "sh[100]", "b[101]", "[[8]+4]>>8&0xff", "[100]&0xff00>>8"})
	{
		SCOPED_TRACE(read);
		Write(8, 96);
		Write(100, 0x12347fff);
		auto runner = MakeRunner("wait " + std::string(read) + " changes within 2");
		ASSERT_TRUE(runner);
		ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_FALSE(runner->IsComplete());
		Write(100, 0x123480ff);
		ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_TRUE(runner->IsComplete());
	}

	Write(100, 0x100);
	auto runner = MakeRunner("wait [100]>>8&0xff changes within 2");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	Write(100, 0x10001ff); // Only masked-out bits change.
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsComplete());
	Write(100, 0x200);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
}

TEST_F(CaptureScriptTest, ChangeWaitFailedReadsCountAsUnchangedAndTimeoutAtLimit)
{
	Write(8, 100);
	Write(100, 7);
	auto runner = MakeRunner("wait [[8]] changes within 4; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 0);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 1021);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 200);
	Write(200, 7); // A different pointer to the same value is unchanged.
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(8, 100);
	Write(100, 8);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());

	runner = MakeRunner("wait [8] changes within 2; hold cross");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetFrameCount(), 2);
	EXPECT_EQ(runner->GetButtons(), 0);
	EXPECT_NE(runner->GetError().find("wait for change timed out"), std::string::npos);
	EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(runner->GetFrameCount(), 2);
}

TEST_F(CaptureScriptTest, ChangeWaitWithInvalidInitialReadCannotObserveAChange)
{
	for (u32 pointer : {0u, 1021u, 0xfffffff8u})
	{
		Write(8, pointer);
		auto runner = MakeRunner("wait [[8]+16] changes within 1");
		ASSERT_TRUE(runner);
		ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
		Write(8, 100);
		Write(116, 7);
		EXPECT_FALSE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
		EXPECT_EQ(runner->GetFrameCount(), 1);
	}
}

TEST_F(CaptureScriptTest, ChangeWaitAcceptsZeroAsAnInitialValue)
{
	auto runner = MakeRunner("wait [0] changes within 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	Write(0, 1);
	EXPECT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 1);
}

TEST_F(CaptureScriptLoadTest, ChangeWaitSnapshotsLoadedValueBeforeTheNextInterval)
{
	ASSERT_TRUE(FileSystem::WriteStringToFile(m_path.c_str(), "8 2a000000"));
	auto runner = MakeRunner("load " + m_path + "; wait [8] changes within 2; capture take.mkv; run 1; stop");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_EQ(m_ram[8], 42);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_FALSE(runner->IsCapturing());
	Write(8, 43);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsCapturing());
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(runner->GetFrameCount(), 3);
}

TEST(CaptureScriptParser, RejectsMalformedLogs)
{
	const char* scripts[] = {
		"log",
		"log values.txt",
		"log \"values.txt [0x10]",
		"log values.txt f[0x10]>>1",
		"log values.txt 0x10",
		"wait f[0x10] == 1 within 1",
		"wait [f[0x10]] == 1 within 1",
	};
	for (const char* script : scripts)
	{
		std::string error;
		EXPECT_FALSE(Parse(script, &error)) << script;
		EXPECT_FALSE(error.empty()) << script;
	}
	EXPECT_TRUE(Parse("log stop", nullptr));
	EXPECT_TRUE(Parse("log values.txt [0x10] f[0x14]; log stop", nullptr));
}

TEST_F(CaptureScriptLoadTest, LogsEachCompletedFrameFromTheNextInterval)
{
	Write(0x10, 5);
	Write(0x14, 0xfffe);
	Write(0x30, 0x3fc00000); // 1.5f
	Write(0x40, 0);
	auto runner = MakeRunner("run 1; log \"" + m_path + "\" [0x10] sh[0x14] f[0x30] [[0x40]+4]; run 2; log stop; run 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(0x10, 6);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	Write(0x10, 7);
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(FileSystem::ReadFileToString(m_path.c_str()), std::optional<std::string>("0\t5\t-2\t1.5\t-\n1\t6\t-2\t1.5\t-\n"));
}

TEST_F(CaptureScriptLoadTest, NewLogAndScriptEndCloseTheOpenLog)
{
	const std::string second = (std::filesystem::path(m_directory) / "second.txt").string();
	Write(0x10, 3);
	auto runner = MakeRunner("log \"" + m_path + "\" b[0x10]; run 1; log " + second + " h[0x10]; run 1");
	ASSERT_TRUE(runner);
	ASSERT_TRUE(runner->Start(m_ram, CAPTURE_OK, STOP_OK));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	ASSERT_TRUE(runner->EndFrame(m_ram, CAPTURE_OK, STOP_OK));
	EXPECT_TRUE(runner->IsComplete());
	EXPECT_EQ(FileSystem::ReadFileToString(m_path.c_str()), std::optional<std::string>("0\t3\n"));
	EXPECT_EQ(FileSystem::ReadFileToString(second.c_str()), std::optional<std::string>("0\t3\n"));
}
