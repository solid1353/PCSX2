// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/InputRecording.h"
#include "Recording/InputRecordingControls.h"
#include "Recording/InputRecordingFile.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include "common/Path.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <tuple>

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
