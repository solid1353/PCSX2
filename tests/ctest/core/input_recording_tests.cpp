// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/InputRecording.h"
#include "Recording/InputRecordingControls.h"
#include "Recording/InputRecordingFile.h"
#include "Input/InputManager.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include "common/Path.h"
#include "common/FileSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <tuple>
#include <vector>

namespace
{
	class CaptureResolutionRenderer final : public GSRenderer
	{
	public:
		void SetInternalResolution(const GSVector2i& size) { m_real_size = size; }

	protected:
		void Draw() override {}
		GSTexture* GetOutput(int, float&, int&) override { return nullptr; }
	};

	class InputRecordingResolutionTest : public testing::Test
	{
	protected:
		void TearDown() override { GSConfig = m_original_config; }

		const Pcsx2Config::GSOptions m_original_config = GSConfig;
		CaptureResolutionRenderer m_renderer;
	};

	class InputRecordingFileTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_directory = std::filesystem::current_path() /
			              ("input-recording-file-test-" +
							  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			ASSERT_TRUE(std::filesystem::create_directories(m_directory));
			m_path = (m_directory / "recording.p2m2").string();
		}

		void TearDown() override
		{
			std::error_code error;
			std::filesystem::remove_all(m_directory, error);
		}

		std::filesystem::path m_directory;
		std::string m_path;
	};
} // namespace

// Seed recording state directly so reset tests never boot or run a VM.
class InputRecordingResetTest : public InputRecordingFileTest
{
protected:
	void Prepare(InputRecording::Type type, bool replay = false, bool initial_load_complete = true)
	{
		const auto start_type = type == InputRecording::Type::POWER_ON       ? InputRecordingFile::StartType::PowerOn :
		                        type == InputRecording::Type::FROM_SAVESTATE ? InputRecordingFile::StartType::SaveState :
		                                                                       InputRecordingFile::StartType::None;
		ASSERT_TRUE(m_recording.m_file.openNew(m_path, start_type));
		m_recording.m_file.setAuthor("Test author");
		m_recording.m_file.setGameName("Test game");
		m_recording.m_file.setEmulatorVersion();
		ASSERT_TRUE(m_recording.m_file.writeHeader());
		for (u32 frame = 0; frame <= 8; frame++)
		{
			for (u8 port = 0; port < 2; port++)
				ASSERT_TRUE(m_recording.m_file.writePadData(frame, PadData(port, 0, std::array<u8, 18>{0xaa, 0xbb})));
		}
		m_recording.m_file.setTotalFrames(8);
		m_recording.m_file.incrementUndoCount();
		// Flush the updated counters through the normal sample writer.
		ASSERT_TRUE(m_recording.m_file.writePadData(8, PadData(1, 0, std::array<u8, 18>{0xaa, 0xbb})));
		const auto before = Snapshot();
		ASSERT_TRUE(before);
		m_before = before.value();
		m_recording.m_type = type;
		m_recording.m_is_active = true;
		m_recording.m_initial_load_complete = initial_load_complete;
		m_recording.m_frame_counter = 8;
		m_recording.m_frame_counter_stateless = 12;
		m_recording.m_starting_frame = type == InputRecording::Type::FROM_SAVESTATE ? 100 : 0;
		m_recording.m_watching_for_rerecords = true;
		m_recording.m_controls.setRecordModeEnabled(!replay);
		if (!replay)
			m_recording.m_controls.setRecordMode(false);
		if (replay)
		{
			ASSERT_TRUE(m_recording.m_file.close());
			ASSERT_TRUE(m_recording.m_file.openExisting(m_path));
		}
	}

	void TearDown() override
	{
		m_recording.m_file.close();
		InputRecordingFileTest::TearDown();
	}

	bool IsWatchingForRerecords() const { return m_recording.m_watching_for_rerecords; }
	bool CloseFile() { return m_recording.m_file.close(); }
	s64 FileSize() const { return FileSystem::FSize64(m_recording.m_file.m_recordingFile); }
	std::optional<std::vector<u8>> Snapshot() const
	{
		// Use the writer's handle: Windows does not permit a second open here.
		return FileSystem::ReadBinaryFile(m_recording.m_file.m_recordingFile);
	}

	InputRecording m_recording;
	std::vector<u8> m_before;
};

TEST_F(InputRecordingResetTest, RestartDiscardsOldRunAndKeepsRecording)
{
	Prepare(InputRecording::Type::POWER_ON);
	ASSERT_FALSE(HasFatalFailure());
	m_recording.handleReset();
	EXPECT_TRUE(m_recording.isActive());
	EXPECT_TRUE(m_recording.getControls().isRecording());
	EXPECT_EQ(m_recording.getFrameCounter(), 0u);
	EXPECT_EQ(m_recording.getFrameCounterStateless(), 0u);
	EXPECT_EQ(m_recording.getData().getTotalFrames(), 0u);
	EXPECT_EQ(m_recording.getData().getUndoCount(), 0u);
	EXPECT_FALSE(IsWatchingForRerecords());
	EXPECT_EQ(FileSize(), 570);
	const auto header = Snapshot();
	ASSERT_TRUE(header);
	ASSERT_EQ(header->size(), 570u);
	EXPECT_TRUE(std::equal(header->begin(), header->begin() + 561, m_before.begin()));
	EXPECT_EQ(header->at(569), 0);

	const std::array<u8, 18> new_input = {0xff, 0xff, 127, 127, 0, 255};
	for (u32 frame = 1; frame <= 2; frame++)
	{
		m_recording.incFrameCounter();
		for (int port = 0; port < 2; port++)
			m_recording.saveControllerData(PadData(port, 0, new_input), port, 0);
	}
	ASSERT_TRUE(CloseFile());
	EXPECT_EQ(FileSystem::GetPathFileSize(m_path.c_str()), 570 + 3 * 36);
	InputRecordingFile saved;
	ASSERT_TRUE(saved.openExisting(m_path));
	EXPECT_EQ(saved.getTotalFrames(), 2u);
	EXPECT_EQ(saved.getUndoCount(), 0u);
	for (u32 frame = 1; frame <= 2; frame++)
	{
		for (uint port = 0; port < 2; port++)
		{
			const auto pad = saved.readPadData(frame, port, 0);
			ASSERT_TRUE(pad);
			EXPECT_EQ(pad->ToArray(), new_input);
		}
	}
	EXPECT_FALSE(saved.readPadData(3, 0, 0));
}

TEST_F(InputRecordingResetTest, RepeatedRestartDropsEachPreviousRun)
{
	Prepare(InputRecording::Type::POWER_ON);
	ASSERT_FALSE(HasFatalFailure());
	for (int restart = 0; restart < 3; restart++)
	{
		m_recording.handleReset();
		EXPECT_TRUE(m_recording.getControls().isRecording());
		EXPECT_EQ(m_recording.getFrameCounter(), 0u);
		EXPECT_EQ(m_recording.getFrameCounterStateless(), 0u);
		EXPECT_EQ(FileSize(), 570);
		m_recording.incFrameCounter();
		m_recording.saveControllerData(PadData(0, 0, std::array<u8, 18>{}), 0, 0);
	}
}

TEST_F(InputRecordingResetTest, PlaybackResetPreservesFileAndReplayMode)
{
	Prepare(InputRecording::Type::POWER_ON, true);
	ASSERT_FALSE(HasFatalFailure());
	m_recording.handleReset();
	m_recording.getControls().processControlQueue();
	EXPECT_TRUE(m_recording.getControls().isReplaying());
	EXPECT_EQ(m_recording.getFrameCounter(), 0u);
	EXPECT_EQ(m_recording.getData().getTotalFrames(), 8u);
	ASSERT_TRUE(CloseFile());
	EXPECT_EQ(FileSystem::ReadBinaryFile(m_path.c_str()).value(), m_before);
}

TEST_F(InputRecordingResetTest, InputOnlyResetPreservesSamplesAndCounters)
{
	Prepare(InputRecording::Type::NO_STATE);
	ASSERT_FALSE(HasFatalFailure());
	m_recording.handleReset();
	EXPECT_TRUE(m_recording.isInputOnly());
	EXPECT_TRUE(m_recording.getControls().isRecording());
	EXPECT_EQ(m_recording.getFrameCounter(), 8u);
	EXPECT_EQ(m_recording.getFrameCounterStateless(), 12u);
	ASSERT_TRUE(CloseFile());
	EXPECT_EQ(FileSystem::ReadBinaryFile(m_path.c_str()).value(), m_before);
}

TEST_F(InputRecordingResetTest, SavestateRecordingResetKeepsExistingReplayBehavior)
{
	Prepare(InputRecording::Type::FROM_SAVESTATE);
	ASSERT_FALSE(HasFatalFailure());
	m_recording.handleReset();
	m_recording.getControls().processControlQueue();
	EXPECT_TRUE(m_recording.getControls().isReplaying());
	EXPECT_EQ(m_recording.getFrameCounter(), 0u);
	ASSERT_TRUE(CloseFile());
	EXPECT_EQ(FileSystem::ReadBinaryFile(m_path.c_str()).value(), m_before);
}

TEST_F(InputRecordingResetTest, InitialBootResetDoesNotRestartTheRecording)
{
	Prepare(InputRecording::Type::POWER_ON, false, false);
	ASSERT_FALSE(HasFatalFailure());
	m_recording.handleReset();
	EXPECT_TRUE(m_recording.getControls().isRecording());
	EXPECT_EQ(m_recording.getFrameCounter(), 8u);
	ASSERT_TRUE(CloseFile());
	EXPECT_EQ(FileSystem::ReadBinaryFile(m_path.c_str()).value(), m_before);
}

TEST_F(InputRecordingResolutionTest, ReplayUsesInternalResolutionRegardlessOfDesktopCaptureSettings)
{
	for (const GSVector2i internal_size : {GSVector2i(2560, 2240), GSVector2i(1280, 896), GSVector2i(512, 448)})
	{
		m_renderer.SetInternalResolution(internal_size);
		for (const bool automatic : {false, true})
		{
			GSConfig.VideoCaptureAutoResolution = automatic;
			GSConfig.VideoCaptureWidth = 1920;
			GSConfig.VideoCaptureHeight = 1080;
			for (const GSVector2i requested_size : {GSVector2i(0, 0), GSVector2i(640, 480)})
			{
				const GSVector2i size = m_renderer.GetCaptureResolution(requested_size, true);
				EXPECT_EQ(size.x, internal_size.x);
				EXPECT_EQ(size.y, internal_size.y);
			}
		}
	}
}

TEST_F(InputRecordingResolutionTest, ReplayMatchesPineAutomaticResolution)
{
	m_renderer.SetInternalResolution(GSVector2i(2560, 2240));
	GSConfig.VideoCaptureAutoResolution = true;
	const GSVector2i pine_size = m_renderer.GetCaptureResolution(GSVector2i(0, 0), false);
	GSConfig.VideoCaptureAutoResolution = false;
	const GSVector2i replay_size = m_renderer.GetCaptureResolution(GSVector2i(0, 0), true);
	EXPECT_EQ(replay_size.x, pine_size.x);
	EXPECT_EQ(replay_size.y, pine_size.y);
	EXPECT_EQ(replay_size.x, 2560);
	EXPECT_EQ(replay_size.y, 2240);
}

TEST_F(InputRecordingResolutionTest, DesktopCaptureKeepsCustomAndExplicitResolutions)
{
	m_renderer.SetInternalResolution(GSVector2i(2560, 2240));
	GSConfig.VideoCaptureAutoResolution = false;
	GSConfig.VideoCaptureWidth = 1920;
	GSConfig.VideoCaptureHeight = 1080;
	const GSVector2i desktop_size = m_renderer.GetCaptureResolution(GSVector2i(0, 0), false);
	EXPECT_EQ(desktop_size.x, 1920);
	EXPECT_EQ(desktop_size.y, 1080);
	const GSVector2i explicit_size = m_renderer.GetCaptureResolution(GSVector2i(1280, 720), false);
	EXPECT_EQ(explicit_size.x, 1280);
	EXPECT_EQ(explicit_size.y, 720);
}

TEST_F(InputRecordingFileTest, AllowsConcurrentReadOnlyPlayback)
{
	const PadData pad_data(0, 0, std::array<u8, 18>{});
	{
		InputRecordingFile recording;
		ASSERT_TRUE(recording.openNew(m_path, false));
		ASSERT_TRUE(recording.writeHeader());
		ASSERT_TRUE(recording.writePadData(0, pad_data));
	}

	InputRecordingFile first_playback;
	InputRecordingFile second_playback;
	ASSERT_TRUE(first_playback.openExisting(m_path));
	EXPECT_TRUE(second_playback.openExisting(m_path));
	EXPECT_FALSE(first_playback.writePadData(0, pad_data));
}

TEST(InputRecordingControls, PreventsRecordModeForReadOnlyPlayback)
{
	InputRecordingControls controls;
	controls.setRecordModeEnabled(false);
	controls.setRecordMode(false);
	EXPECT_TRUE(controls.isReplaying());

	controls.setRecordModeEnabled(true);
	controls.setRecordMode(false);
	EXPECT_TRUE(controls.isRecording());
}

TEST(InputRecordingHotkey, IsListedInSystemWithoutADefaultBinding)
{
	const HotkeyInfo* hotkey = InputManager::GetHotkeyInfo("ToggleInputRecording");
	ASSERT_NE(hotkey, nullptr);
	EXPECT_STREQ(hotkey->display_name, "Toggle Input Recording");
	EXPECT_STREQ(hotkey->category, "System");
	EXPECT_EQ(hotkey->default_binding, nullptr);
	EXPECT_NE(hotkey->handler, nullptr);
}

TEST_F(InputRecordingFileTest, NoStateFormatStoresRawPadsAndAnExplicitOrigin)
{
	const std::array<u8, 18> first = {0xff, 0xbf, 0, 255, 64, 192, 0, 0, 0, 0, 0, 0, 255};
	const std::array<u8, 18> second = {0xff, 0xff, 127, 127, 127, 127};
	InputRecordingFile recording;
	ASSERT_TRUE(recording.openNew(m_path, InputRecordingFile::StartType::None));
	ASSERT_TRUE(recording.writeHeader());
	ASSERT_TRUE(recording.writePadData(0, PadData(0, 0, first)));
	ASSERT_TRUE(recording.writePadData(0, PadData(1, 0, second)));
	ASSERT_TRUE(recording.writePadData(1, PadData(0, 0, second)));
	ASSERT_TRUE(recording.writePadData(1, PadData(1, 0, first)));
	recording.setTotalFrames(2);
	ASSERT_TRUE(recording.close());

	const auto bytes = FileSystem::ReadBinaryFile(m_path.c_str());
	ASSERT_TRUE(bytes);
	ASSERT_EQ(bytes->size(), 570u + 2u * 36u);
	EXPECT_EQ((*bytes)[0], 2); // Version 2.
	EXPECT_EQ((*bytes)[561], 2); // Little-endian sample count.
	EXPECT_EQ((*bytes)[562], 0);
	EXPECT_EQ((*bytes)[565], 0); // Undo count.
	EXPECT_EQ((*bytes)[569], 2); // No starting state.
	EXPECT_FALSE(std::filesystem::exists(m_path + "_SaveState"));

	ASSERT_TRUE(recording.openExisting(m_path));
	EXPECT_FALSE(recording.hasStartingState());
	EXPECT_FALSE(recording.fromSaveState());
	EXPECT_EQ(recording.getTotalFrames(), 2);
	const auto port0 = recording.readPadData(0, 0, 0);
	const auto port1 = recording.readPadData(1, 1, 0);
	ASSERT_TRUE(port0);
	ASSERT_TRUE(port1);
	EXPECT_EQ(port0->ToArray(), first);
	EXPECT_EQ(port1->ToArray(), first);
	EXPECT_FALSE(recording.readPadData(2, 0, 0));
	EXPECT_FALSE(recording.readPadData(0, 2, 0));
	EXPECT_FALSE(recording.readPadData(0, 0, 1));
}

TEST_F(InputRecordingFileTest, RefusesNoStateReplayBeforeResetOrSavestateLoading)
{
	InputRecordingFile recording;
	ASSERT_TRUE(recording.openNew(m_path, InputRecordingFile::StartType::None));
	ASSERT_TRUE(recording.writeHeader());
	ASSERT_TRUE(recording.close());
	for (bool capture_markers : {false, true})
	{
		InputRecording playback;
		EXPECT_FALSE(playback.play(m_path, capture_markers));
		EXPECT_FALSE(playback.isActive());
		EXPECT_FALSE(playback.isCapturingVideo());
	}
	InputRecording playback;
	EXPECT_FALSE(playback.play(m_path, false, {}, InputRecordingCaptureMode::Full, {}, "capture.mkv", InputRecordingVideoFrameRange{0, 0}));
	EXPECT_FALSE(playback.isActive());
}

TEST_F(InputRecordingFileTest, PreservesVersionOnePowerOnAndSavestateMarkers)
{
	for (bool from_state : {false, true})
	{
		InputRecordingFile recording;
		ASSERT_TRUE(recording.openNew(m_path, from_state));
		ASSERT_TRUE(recording.writeHeader());
		ASSERT_TRUE(recording.close());
		const auto bytes = FileSystem::ReadBinaryFile(m_path.c_str());
		ASSERT_TRUE(bytes);
		ASSERT_EQ(bytes->size(), 570u);
		EXPECT_EQ((*bytes)[0], 1);
		EXPECT_EQ((*bytes)[569], from_state ? 1 : 0);
		ASSERT_TRUE(recording.openExisting(m_path));
		EXPECT_TRUE(recording.hasStartingState());
		EXPECT_EQ(recording.fromSaveState(), from_state);
	}
}

TEST_F(InputRecordingFileTest, RejectsUnknownOriginMarkersAndTruncatedSamples)
{
	InputRecordingFile recording;
	ASSERT_TRUE(recording.openNew(m_path, InputRecordingFile::StartType::None));
	ASSERT_TRUE(recording.writeHeader());
	ASSERT_TRUE(recording.close());
	auto bytes = FileSystem::ReadBinaryFile(m_path.c_str());
	ASSERT_TRUE(bytes);
	(*bytes)[569] = 3;
	ASSERT_TRUE(FileSystem::WriteBinaryFile(m_path.c_str(), bytes->data(), bytes->size()));
	EXPECT_FALSE(recording.openExisting(m_path));
	(*bytes)[569] = 2;
	(*bytes)[0] = 1; // The old boolean must never be interpreted as a no-state marker.
	ASSERT_TRUE(FileSystem::WriteBinaryFile(m_path.c_str(), bytes->data(), bytes->size()));
	EXPECT_FALSE(recording.openExisting(m_path));
	(*bytes)[0] = 2;
	bytes->push_back(0xff); // One byte cannot constitute an 18-byte pad sample.
	ASSERT_TRUE(FileSystem::WriteBinaryFile(m_path.c_str(), bytes->data(), bytes->size()));
	ASSERT_TRUE(recording.openExisting(m_path));
	EXPECT_FALSE(recording.readPadData(0, 0, 0));
}

TEST_F(InputRecordingFileTest, RejectsInvalidVideoCaptureBeforeStartingVM)
{
	InputRecordingFile recording;
	ASSERT_TRUE(recording.openNew(m_path, false));
	ASSERT_TRUE(recording.writeHeader());
	recording.setTotalFrames(10);
	ASSERT_TRUE(recording.close());

	for (const auto& [path, frames] : {
			 std::tuple{"capture.mkv", std::optional<InputRecordingVideoFrameRange>{}},
			 std::tuple{"", std::optional<InputRecordingVideoFrameRange>{{0, 0}}},
			 std::tuple{"capture.mp4", std::optional<InputRecordingVideoFrameRange>{{0, 0}}},
			 std::tuple{"capture.mkv", std::optional<InputRecordingVideoFrameRange>{{0, 11}}},
			 std::tuple{"capture.mkv", std::optional<InputRecordingVideoFrameRange>{{5, 4}}}})
	{
		InputRecording playback;
		EXPECT_FALSE(playback.play(m_path, false, {}, InputRecordingCaptureMode::Full, {}, path, frames));
		EXPECT_FALSE(playback.isActive());
		EXPECT_FALSE(playback.isCapturingVideo());
		EXPECT_FALSE(playback.didVideoCaptureComplete());
	}
}

TEST_F(InputRecordingFileTest, RejectsSavestateVideoCaptureBeforeStartingVM)
{
	InputRecordingFile recording;
	ASSERT_TRUE(recording.openNew(m_path, true));
	ASSERT_TRUE(recording.writeHeader());
	ASSERT_TRUE(recording.close());

	InputRecording playback;
	EXPECT_FALSE(playback.play(m_path, false, {}, InputRecordingCaptureMode::Full, {}, "capture.mkv",
		InputRecordingVideoFrameRange{0, 0}));
	EXPECT_FALSE(playback.isActive());
	EXPECT_FALSE(playback.didVideoCaptureComplete());
}

TEST(InputRecordingCapture, ParsesSupportedModes)
{
	const std::optional<InputRecordingCaptureMode> full = ParseInputRecordingCaptureMode("full");
	const std::optional<InputRecordingCaptureMode> screenshots = ParseInputRecordingCaptureMode("screenshots");
	const std::optional<InputRecordingCaptureMode> savestates = ParseInputRecordingCaptureMode("savestates");
	ASSERT_TRUE(full.has_value());
	ASSERT_TRUE(screenshots.has_value());
	ASSERT_TRUE(savestates.has_value());
	EXPECT_EQ(full.value(), InputRecordingCaptureMode::Full);
	EXPECT_EQ(screenshots.value(), InputRecordingCaptureMode::Screenshots);
	EXPECT_EQ(savestates.value(), InputRecordingCaptureMode::Savestates);
	EXPECT_FALSE(ParseInputRecordingCaptureMode("invalid").has_value());
}

TEST(InputRecordingCapture, ParsesAndNormalizesMarkerSelection)
{
	const std::optional<InputRecordingCaptureMarkerRanges> markers =
		ParseInputRecordingCaptureMarkers("9,2-5,1,4-7");
	ASSERT_TRUE(markers.has_value());
	ASSERT_EQ(markers->size(), 2u);
	EXPECT_EQ(markers->at(0).first, 1u);
	EXPECT_EQ(markers->at(0).last, 7u);
	EXPECT_EQ(markers->at(1).first, 9u);
	EXPECT_EQ(markers->at(1).last, 9u);
}

TEST(InputRecordingCapture, ParsesInclusiveVideoFrameCounters)
{
	for (const auto [text, first, last] : {
			 std::tuple{"0-0", 0u, 0u}, std::tuple{"1-1", 1u, 1u}, std::tuple{"120-179", 120u, 179u},
			 std::tuple{"0-4294967295", 0u, 4294967295u}})
	{
		const auto range = ParseInputRecordingVideoFrames(text);
		ASSERT_TRUE(range.has_value()) << text;
		EXPECT_EQ(range->first, first);
		EXPECT_EQ(range->last, last);
	}
}

TEST(InputRecordingCapture, RejectsInvalidVideoFrameRanges)
{
	for (const std::string_view value : {"", "1", "-1", "1-", "2-1", "1--2", "-1-2", "+1-2", "1-2,4-5",
			 "1-2-3", " 1-2", "1-2 ", "1 -2", "1- 2", "a-b", "0-4294967296", "4294967296-4294967296"})
	{
		EXPECT_FALSE(ParseInputRecordingVideoFrames(value).has_value()) << value;
	}
}

TEST(InputRecordingCapture, RejectsInvalidMarkerSelection)
{
	for (const std::string_view value : {"", "0", "1,", ",1", "1,,2", "1--2", "5-3", "a", "4294967296"})
		EXPECT_FALSE(ParseInputRecordingCaptureMarkers(value).has_value()) << value;
}

TEST(InputRecordingCapture, SelectsRequestedMarkerOrdinals)
{
	const InputRecordingCaptureMarkerRanges all_markers;
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(all_markers, 1));

	const std::optional<InputRecordingCaptureMarkerRanges> markers = ParseInputRecordingCaptureMarkers("2-4,7");
	ASSERT_TRUE(markers.has_value());
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 1));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 2));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 4));
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 5));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 7));
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 8));
}

TEST(InputRecordingCapture, SelectsDirectoriesForEachMode)
{
	const std::string capture_root = Path::Combine("capture", "case");
	const InputRecordingCaptureDirectories full =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Full);
	const InputRecordingCaptureDirectories screenshots =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Screenshots);
	const InputRecordingCaptureDirectories savestates =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Savestates);

	EXPECT_EQ(full.savestates, Path::Combine(capture_root, "sstates"));
	EXPECT_EQ(full.screenshots, capture_root);
	EXPECT_TRUE(screenshots.savestates.empty());
	EXPECT_EQ(screenshots.screenshots, capture_root);
	EXPECT_EQ(savestates.savestates, Path::Combine(capture_root, "sstates"));
	EXPECT_TRUE(savestates.screenshots.empty());
}
