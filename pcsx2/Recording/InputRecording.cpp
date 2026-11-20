// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "MTGS.h"
#include "SaveState.h"

bool SaveStateBase::InputRecordingFreeze()
{
	// NOTE - BE CAREFUL
	// CHANGING THIS WILL BREAK BACKWARDS COMPATIBILITY ON SAVESTATES
	if (!FreezeTag("InputRecording"))
		return false;

	Freeze(g_FrameCount);
	return IsOkay();
}

#include "InputRecording.h"

#include "InputRecordingControls.h"
#include "CaptureScript.h"
#include "Utilities/InputRecordingLogger.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "Config.h"
#include "Counters.h"
#include "SaveState.h"
#include "VMManager.h"
#include "Host.h"
#include "ImGui/ImGuiOverlays.h"
#include "ImGui/FullscreenUI.h"
#include "DebugTools/Debug.h"
#include "GameDatabase.h"
#include "fmt/format.h"
#include "fmt/chrono.h"
#include "GS.h"
#include "GS/GSCapture.h"
#include "Host.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <ctime>

std::optional<InputRecordingCaptureMode> ParseInputRecordingCaptureMode(const std::string_view value)
{
	if (value == "full")
		return InputRecordingCaptureMode::Full;
	if (value == "screenshots")
		return InputRecordingCaptureMode::Screenshots;
	if (value == "savestates")
		return InputRecordingCaptureMode::Savestates;
	return std::nullopt;
}

std::optional<InputRecordingVideoFrameRange> ParseInputRecordingVideoFrames(const std::string_view value)
{
	const size_t separator = value.find('-');
	if (separator == std::string_view::npos)
		return std::nullopt;
	InputRecordingVideoFrameRange range;
	const auto parse = [](std::string_view part, u32& result) {
		const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result);
		return !part.empty() && parsed.ec == std::errc() && parsed.ptr == part.data() + part.size();
	};
	if (!parse(value.substr(0, separator), range.first) || !parse(value.substr(separator + 1), range.last) ||
		range.first > range.last)
	{
		return std::nullopt;
	}
	return range;
}

std::optional<InputRecordingCaptureMarkerRanges> ParseInputRecordingCaptureMarkers(const std::string_view value)
{
	if (value.empty() || value.front() == ',' || value.back() == ',' || value.find(",,") != std::string_view::npos)
		return std::nullopt;

	InputRecordingCaptureMarkerRanges ranges;
	for (const std::string_view part : StringUtil::SplitString(value, ',', false))
	{
		if (part.empty())
			return std::nullopt;

		const size_t separator = part.find('-');
		const std::string_view first_part = part.substr(0, separator);
		const std::string_view last_part = (separator == std::string_view::npos) ? first_part : part.substr(separator + 1);
		if (first_part.empty() || last_part.empty() ||
			(separator != std::string_view::npos && last_part.find('-') != std::string_view::npos))
		{
			return std::nullopt;
		}

		auto parse_marker = [](const std::string_view marker) -> std::optional<u32> {
			u32 result;
			const std::from_chars_result parse_result =
				std::from_chars(marker.data(), marker.data() + marker.size(), result);
			if (parse_result.ec != std::errc() || parse_result.ptr != marker.data() + marker.size() || result == 0)
				return std::nullopt;
			return result;
		};

		const std::optional<u32> first = parse_marker(first_part);
		const std::optional<u32> last = parse_marker(last_part);
		if (!first.has_value() || !last.has_value() || first.value() > last.value())
			return std::nullopt;

		ranges.push_back({first.value(), last.value()});
	}

	if (ranges.empty())
		return std::nullopt;

	std::ranges::sort(ranges, {}, &InputRecordingCaptureMarkerRange::first);
	InputRecordingCaptureMarkerRanges normalized_ranges;
	for (const InputRecordingCaptureMarkerRange range : ranges)
	{
		if (!normalized_ranges.empty() &&
			(range.first <= normalized_ranges.back().last ||
				(normalized_ranges.back().last != std::numeric_limits<u32>::max() &&
					range.first == normalized_ranges.back().last + 1)))
		{
			normalized_ranges.back().last = std::max(normalized_ranges.back().last, range.last);
		}
		else
		{
			normalized_ranges.push_back(range);
		}
	}
	return normalized_ranges;
}

bool IsInputRecordingCaptureMarkerSelected(const InputRecordingCaptureMarkerRanges& ranges, const u32 marker)
{
	if (ranges.empty())
		return true;

	for (const InputRecordingCaptureMarkerRange range : ranges)
	{
		if (marker < range.first)
			return false;
		if (marker <= range.last)
			return true;
	}
	return false;
}

InputRecordingCaptureDirectories GetInputRecordingCaptureDirectories(const std::string_view recording_path,
	const std::string_view capture_directory, const InputRecordingCaptureMode mode)
{
	InputRecordingCaptureDirectories directories;
	const bool capture_savestates = (mode != InputRecordingCaptureMode::Screenshots);
	const bool capture_screenshots = (mode != InputRecordingCaptureMode::Savestates);
	if (capture_directory.empty())
	{
		std::string recording_name = Path::SanitizeFileName(Path::GetFileTitle(recording_path));
		if (recording_name.empty())
			recording_name = "input-recording";

		if (capture_savestates)
			directories.savestates = Path::Combine(EmuFolders::Savestates, recording_name);
		if (capture_screenshots)
			directories.screenshots = Path::Combine(EmuFolders::Snapshots, recording_name);
	}
	else
	{
		if (capture_savestates)
			directories.savestates = Path::Combine(capture_directory, "sstates");
		if (capture_screenshots)
			directories.screenshots = std::string(capture_directory);
	}
	return directories;
}

InputRecording g_InputRecording;

static void BackupInputRecordingSaveState(const std::string& source)
{
	VMManager::WaitForSaveStateFlush();
	if (!FileSystem::DirectoryExists(source.c_str()))
		return;

	const std::string destination = fmt::format("{}.backup", source);
	if (FileSystem::DirectoryExists(destination.c_str()) &&
		!FileSystem::RecursiveDeleteDirectory(destination.c_str()))
	{
		return;
	}
	VMManager::UpdateSaveStateGSDumpPaths(destination, {});
	if (!FileSystem::CreateDirectoryPath(destination.c_str(), false))
		return;

	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(source.c_str(), "*", FILESYSTEM_FIND_FILES, &files))
		return;

	bool copied = true;
	for (const FILESYSTEM_FIND_DATA& file : files)
	{
		copied = FileSystem::CopyFilePath(
					 file.FileName.c_str(), Path::Combine(destination, Path::GetFileName(file.FileName)).c_str(), true) &&
		         copied;
	}
	const std::string screenshot = SaveState_GetScreenshotPath(source);
	if (FileSystem::FileExists(screenshot.c_str()))
		copied = FileSystem::CopyFilePath(screenshot.c_str(), SaveState_GetScreenshotPath(destination).c_str(), true) && copied;
	if (copied)
		VMManager::UpdateSaveStateGSDumpPaths(source, destination, true);
}

bool InputRecording::create(const std::string& fileName, const bool fromSaveState, const std::string& authorName)
{
	m_capture_markers = false;
	m_capture_marker_down = false;
	m_capture_mode = InputRecordingCaptureMode::Full;
	m_capture_marker_ranges.clear();
	m_exit_on_replay_completion = false;
	m_capture_index = 0;
	m_capture_savestate_directory.clear();
	m_capture_snapshot_directory.clear();

	m_controls.setRecordModeEnabled(true);
	if (!m_file.openNew(fileName, fromSaveState))
	{
		return false;
	}

	m_controls.setRecordMode();
	if (fromSaveState)
	{
		std::string savestatePath = fmt::format("{}_SaveState", fileName);
		BackupInputRecordingSaveState(savestatePath);
		m_type = Type::FROM_SAVESTATE;
		m_is_active = true;
		m_initial_load_complete = true;
		m_watching_for_rerecords = true;
		setStartingFrame(g_FrameCount);
		VMManager::SaveState(savestatePath.c_str(), true, false, [](const std::string& error) {
			SaveState_ReportSaveErrorOSD(error, std::nullopt);
		});
	}
	else
	{
		m_starting_frame = 0;
		m_type = Type::POWER_ON;
		m_initial_load_complete = false;
		m_is_active = true;
		// TODO - should this be an explicit [full] boot instead of a reset?
		VMManager::Reset();
	}

	m_file.setEmulatorVersion();
	m_file.setAuthor(authorName);
	m_file.setGameName(VMManager::GetTitle(false));
	m_file.writeHeader();
	initializeState();
	InputRec::log(TRANSLATE_STR("InputRecording", "Started new input recording"), Host::OSD_INFO_DURATION);
	InputRec::consoleLog(fmt::format("Filename {}", m_file.getFilename()));
	return true;
}

void InputRecording::toggleInputRecording()
{
	if (!VMManager::HasValidVM())
		return;
	if (isInputOnly())
	{
		closeActiveFile();
		return;
	}
	if (m_is_active || CaptureScript::Runtime::IsActive())
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "Stop the active recording, replay, or capture script first."), Host::OSD_ERROR_DURATION);
		return;
	}
	std::string title = Path::SanitizeFileName(VMManager::GetTitle(false));
	if (title.empty())
		title = "input-recording";
	const std::time_t now = std::time(nullptr);
	std::tm local_time{};
#ifdef _WIN32
	localtime_s(&local_time, &now);
#else
	localtime_r(&now, &local_time);
#endif
	const std::string base = fmt::format("{} - {:%Y-%m-%d %H.%M.%S}", title, local_time);
	std::string filename = Path::Combine(EmuFolders::InputRecordings, base + ".p2m2");
	for (u32 suffix = 2; FileSystem::FileExists(filename.c_str()); suffix++)
		filename = Path::Combine(EmuFolders::InputRecordings, fmt::format("{} ({}).p2m2", base, suffix));
	if (!createInputOnly(filename))
		InputRec::log(TRANSLATE_STR("InputRecording", "Failed to start input recording."), Host::OSD_ERROR_DURATION);
}

bool InputRecording::createInputOnly(const std::string& filename)
{
	m_osd_messages_enabled = true;
	// Start at the input poll's current frame, without resetting; hotkey boundary timing needs proper runtime testing.
	if (!m_file.openNew(filename, InputRecordingFile::StartType::None))
		return false;
	m_file.setEmulatorVersion();
	m_file.setAuthor({});
	m_file.setGameName(VMManager::GetTitle(false));
	if (!m_file.writeHeader())
	{
		m_file.close();
		return false;
	}
	m_type = Type::NO_STATE;
	m_initial_load_complete = true;
	m_starting_frame = g_FrameCount;
	m_frame_counter_stateless = 0;
	m_capture_markers = false;
	m_capture_marker_down = false;
	m_exit_on_replay_completion = false;
	m_video_frames.reset();
	m_video_audio.clear();
	m_video_started = false;
	m_controls.processControlQueue();
	m_is_active = true;
	m_controls.setRecordModeEnabled(true);
	m_controls.setRecordMode(false);
	initializeState();
	InputRec::log(TRANSLATE_STR("InputRecording", "Recording input"), Host::OSD_INFO_DURATION);
	return true;
}

bool InputRecording::play(const std::string& filename, bool capture_markers, const std::string& capture_directory,
	const InputRecordingCaptureMode capture_mode, const InputRecordingCaptureMarkerRanges& capture_marker_ranges,
	const std::string& video_path, std::optional<InputRecordingVideoFrameRange> video_frames)
{
	m_video_complete = false;
	m_capture_markers = false;
	m_capture_marker_down = false;
	m_capture_mode = capture_mode;
	m_capture_marker_ranges = capture_marker_ranges;
	m_exit_on_replay_completion = false;
	m_capture_index = 0;
	m_capture_savestate_directory.clear();
	m_capture_snapshot_directory.clear();

	m_controls.setRecordModeEnabled(false);
	if (!m_file.openExisting(filename))
	{
		return false;
	}
	if (!m_file.hasStartingState())
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "This input recording has no starting state and cannot be replayed."), Host::OSD_ERROR_DURATION);
		m_file.close();
		return false;
	}
	if (video_path.empty() != !video_frames.has_value() ||
		(video_frames && (m_file.fromSaveState() || video_frames->first > video_frames->last ||
							 video_frames->last > m_file.getTotalFrames() || !StringUtil::compareNoCase(Path::GetExtension(video_path), "mkv"))))
	{
		Console.Error("Input recording video capture requires a power-on replay, an .mkv path, and a frame range within 0..total.");
		m_file.close();
		return false;
	}
	m_video_path = video_path;
	m_video_frames = video_frames;
	m_video_audio.clear();
	m_video_started = false;

	if (capture_markers && m_file.fromSaveState())
	{
		InputRec::consoleLog("Replay marker capture requires a power-on input recording.");
		InputRec::log(TRANSLATE_STR("InputRecording", "Replay marker capture requires a power-on input recording"),
			Host::OSD_ERROR_DURATION);
		m_file.close();
		return false;
	}

	if (capture_markers)
	{
		InputRecordingCaptureDirectories directories =
			GetInputRecordingCaptureDirectories(filename, capture_directory, capture_mode);
		m_capture_savestate_directory = std::move(directories.savestates);
		m_capture_snapshot_directory = std::move(directories.screenshots);
		if ((!m_capture_savestate_directory.empty() &&
				!FileSystem::CreateDirectoryPath(m_capture_savestate_directory.c_str(), false)) ||
			(!m_capture_snapshot_directory.empty() &&
				!FileSystem::CreateDirectoryPath(m_capture_snapshot_directory.c_str(), false)))
		{
			InputRec::consoleLog(fmt::format("Failed to create replay marker capture directories for {}", filename));
			InputRec::log(TRANSLATE_STR("InputRecording", "Failed to create replay marker capture directories"),
				Host::OSD_ERROR_DURATION);
			m_file.close();
			m_capture_savestate_directory.clear();
			m_capture_snapshot_directory.clear();
			return false;
		}
		m_capture_markers = true;
		m_exit_on_replay_completion = !capture_directory.empty();
	}
	m_exit_on_replay_completion |= video_frames.has_value();

	// Either load the savestate, or restart the game
	if (m_file.fromSaveState())
	{
		std::string savestatePath = fmt::format("{}_SaveState", m_file.getFilename());
		if (!FileSystem::DirectoryExists(savestatePath.c_str()))
		{
			InputRec::consoleLog(fmt::format("Could not locate savestate directory at location - {}", savestatePath));
			InputRec::log(TRANSLATE_STR("InputRecording", "Failed to load state for input recording"), Host::OSD_ERROR_DURATION);
			m_file.close();
			return false;
		}
		m_type = Type::FROM_SAVESTATE;
		m_initial_load_complete = false;
		m_is_active = true;
		const auto loaded = VMManager::LoadState(savestatePath.c_str());
		if (!loaded)
		{
			InputRec::log(TRANSLATE_STR("InputRecording", "Failed to load state for input recording, unsupported version?"), Host::OSD_ERROR_DURATION);
			m_file.close();
			m_is_active = false;
			return false;
		}
	}
	else
	{
		m_starting_frame = 0;
		m_type = Type::POWER_ON;
		m_initial_load_complete = false;
		m_is_active = true;
		// TODO - should this be an explicit [full] boot instead of a reset?
		VMManager::Reset();
	}
	m_controls.setReplayMode();
	initializeState();
	InputRec::log(TRANSLATE_STR("InputRecording", "Replaying input recording"), Host::OSD_INFO_DURATION);
	m_file.logRecordingMetadata();
	if (VMManager::GetTitle(false) != m_file.getGameName())
	{
		InputRec::consoleLog(fmt::format("Input recording was possibly constructed for a different game. Expected: {}, Actual: {}", m_file.getGameName(), VMManager::GetTitle(false)));
	}
	return true;
}

void InputRecording::closeActiveFile()
{
	if (!m_is_active)
	{
		return;
	}
	if (m_video_frames)
	{
		if (m_video_started)
		{
			MTGS::RunOnGSThread(&GSEndCapture);
			MTGS::WaitGS(false);
		}
		Console.Error("Input recording video capture stopped before its final frame.");
		m_video_started = false;
	}
	m_video_frames.reset();
	m_video_audio.clear();
	const bool input_only = isInputOnly();
	const std::string filename = m_file.getFilename();
	const bool saved = m_file.close();
	m_is_active = false;
	m_capture_markers = false;
	m_capture_marker_down = false;
	InformGSThread();
	if (saved)
	{
		if (input_only)
			InputRec::log(fmt::format(TRANSLATE_FS("InputRecording", "Saved input recording: {}"), Path::GetFileName(filename)), Host::OSD_INFO_DURATION);
		else
			InputRec::log(TRANSLATE_STR("InputRecording", "Input recording stopped"), Host::OSD_ERROR_DURATION);
		MTGS::PresentCurrentFrame();
	}
	else
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "Unable to stop input recording"), Host::OSD_ERROR_DURATION);
	}
}

void InputRecording::stop()
{
	if (VMManager::GetState() == VMState::Paused || isInputOnly())
	{
		closeActiveFile();
	}
	else
	{
		// Don't stop immediately, close the file after the current frame completes
		m_recordingQueue.push([&]() {
			closeActiveFile();
		});
	}
}

void InputRecording::handleControllerDataUpdate()
{
	if (isInputOnly() && m_frame_counter == std::numeric_limits<u32>::max())
	{
		closeActiveFile();
		return;
	}
	bool capture_marker_down = false;

	// TODO - multi-tap support with new file format, for now just controller 0 and 1
	for (int i = 0; i < 2; i++)
	{
		// Fetch the current frame's data
		PadData frameData(i, 0);
		if (m_is_active)
		{
			if (m_controls.isRecording())
			{
				saveControllerData(frameData, i, 0);
			}
			else if (m_controls.isReplaying())
			{
				const auto& modifiedFrameData = updateControllerData(i, 0);
				if (modifiedFrameData)
				{
					frameData = modifiedFrameData.value();
					capture_marker_down |= frameData.m_l3 && frameData.m_r3;
				}
			}
		}
		// Log the data we have gathered, useful for debugging our use-case
		frameData.LogPadData();
	}

	if (m_capture_markers)
	{
		if (capture_marker_down && !m_capture_marker_down)
			captureReplayMarker();
		m_capture_marker_down = capture_marker_down;
	}
	if (isInputOnly())
	{
		// No-state logs are contiguous samples, independent of resets or state-load frame counters.
		m_frame_counter++;
		m_frame_counter_stateless = m_frame_counter;
		m_file.setTotalFrames(m_frame_counter);
	}
}

void InputRecording::captureReplayMarker()
{
	const u32 capture_index = ++m_capture_index;
	if (!IsInputRecordingCaptureMarkerSelected(m_capture_marker_ranges, capture_index))
		return;

	const std::string capture_name = fmt::format("{:03}", capture_index);
	const std::string snapshot_path = m_capture_snapshot_directory.empty() ? std::string() :
	                                                                         Path::Combine(m_capture_snapshot_directory, fmt::format("{}.png", capture_name));

	if (m_capture_mode == InputRecordingCaptureMode::Screenshots)
	{
		Error error;
		if (!SaveState_SaveScreenshotToFile(snapshot_path.c_str(), &error))
			InputRec::consoleLog(fmt::format("Failed to capture replay marker {}: {}", capture_name, error.GetDescription()));
	}
	else
	{
		const std::string savestate_path = Path::Combine(m_capture_savestate_directory, capture_name);
		VMManager::SaveState(savestate_path.c_str(), true, false, [capture_name](const std::string& error) {
			if (!error.empty())
				InputRec::consoleLog(fmt::format("Failed to save replay marker {}: {}", capture_name, error)); }, snapshot_path);
	}
	InputRec::consoleLog(fmt::format("Captured replay marker {}", capture_name));
}

void InputRecording::saveControllerData(const PadData& data, const int port, const int slot)
{
	// Save the frame's data to the file
	if (!m_file.writePadData(m_frame_counter, data))
	{
		InputRec::consoleLog(fmt::format("Failed to write input data at [{}:{}:{}]", m_frame_counter, port, slot));
	}
}
std::optional<PadData> InputRecording::updateControllerData(const int port, const int slot)
{
	// Get the PadData from the file
	const auto frameData = m_file.readPadData(m_frame_counter, port, slot);
	if (frameData)
	{
		// Update the g_key_status appropriately
		frameData->OverrideActualController();
	}
	else
	{
		InputRec::consoleLog(fmt::format("Failed to read input data at [{}:{}:{}]", m_frame_counter, port, slot));
	}
	return frameData;
}

void InputRecording::processRecordQueue()
{
	while (!m_recordingQueue.empty())
	{
		m_recordingQueue.front()();
		m_recordingQueue.pop();
	}
}

void InputRecording::prepareVideoFrame()
{
	if (!isCapturingVideo() || m_frame_counter < m_video_frames->first || m_frame_counter > m_video_frames->last)
		return;

	m_video_frame_okay = false;
	MTGS::RunOnGSThread([this, samples = std::move(m_video_audio)]() mutable {
		auto deliver = [this, samples = std::move(samples)]() {
			m_video_frame_okay = (m_video_started || GSBeginReplayCapture(m_video_path)) &&
			                     GSCapture::DeliverReplayAudio(samples);
		};
		if (m_video_started)
			deliver();
		else
			GSQueueReplayCapture(std::move(deliver));
	});
	m_video_audio.clear();
}

void InputRecording::finishVideoFrame()
{
	if (!isCapturingVideo() || m_frame_counter < m_video_frames->first || m_frame_counter > m_video_frames->last)
		return;

	// Startup and the first audio packet run after Merge, before this VSync's video delivery.
	MTGS::WaitGS(false);
	if (!m_video_frame_okay)
	{
		Console.Error("Input recording video capture failed to start or write audio.");
		// Close a partially opened capture too, before batch shutdown.
		MTGS::RunOnGSThread(&GSEndCapture);
		MTGS::WaitGS(false);
		m_video_started = false;
		m_video_frames.reset();
		closeActiveFile();
		Host::RequestVMShutdown(false, false, false);
		Host::RequestExitApplication(false);
		return;
	}
	m_video_started = true;
	if (m_frame_counter != m_video_frames->last)
		return;

	const s64 expected = static_cast<s64>(m_video_frames->last) - m_video_frames->first + 1;
	bool okay = false;
	MTGS::RunOnGSThread([expected, &okay]() {
		const bool complete = GSCapture::IsCapturing() && GSCapture::GetVideoFrameCount() == expected;
		okay = GSEndCapture() && complete;
	});
	MTGS::WaitGS(false);
	m_video_complete = okay;
	if (okay)
		Console.WriteLnFmt("Input recording video capture complete: {} frames written to '{}'.", expected, m_video_path);
	else
		Console.Error("Input recording video capture failed to write and finalize every requested frame.");
	m_video_started = false;
	m_video_frames.reset();
	closeActiveFile();
	Host::RequestVMShutdown(false, false, false);
	if (!okay)
		Host::RequestExitApplication(false);
}

void InputRecording::incFrameCounter()
{
	if (!m_is_active || isInputOnly())
	{
		return;
	}

	if (m_frame_counter == std::numeric_limits<u32>::max())
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "Congratulations, you've been playing for far too long and thus have reached the limit of input recording! Stopping recording now..."), Host::OSD_CRITICAL_ERROR_DURATION);
		stop();
		return;
	}
	m_frame_counter++;

	if (m_controls.isReplaying())
	{
		InformGSThread();
		// If we've reached the end of the recording while replaying, pause or close after the final frame completes.
		if (!m_video_frames && m_frame_counter == m_file.getTotalFrames())
		{
			if (m_exit_on_replay_completion)
			{
				m_recordingQueue.push([this]() {
					closeActiveFile();
					Host::RequestVMShutdown(false, false, false);
				});
			}
			else
			{
				VMManager::SetPaused(true);
			}
			// Can also stop watching for re-records, they've watched to the end of the recording
			m_watching_for_rerecords = false;
		}
	}
	if (m_controls.isRecording())
	{
		m_frame_counter_stateless++;
		m_file.setTotalFrames(m_frame_counter);
		// If we've been in record mode and moved to the next frame, we've overrote something
		// if this was following a save-state loading, this is considered a re-record, a.k.a an undo
		if (m_watching_for_rerecords)
		{
			m_file.incrementUndoCount();
			m_watching_for_rerecords = false;
		}
		InformGSThread();
	}
}

u32 InputRecording::getFrameCounter() const
{
	return m_frame_counter;
}

u32 InputRecording::getFrameCounterStateless() const
{
	return m_frame_counter_stateless;
}

bool InputRecording::isActive() const
{
	return m_is_active;
}

void InputRecording::stopReadOnlyReplayAtEndOnResume()
{
	if (m_is_active && m_controls.isReplaying() && !m_controls.isRecordModeEnabled() &&
		!m_exit_on_replay_completion && m_frame_counter >= m_file.getTotalFrames())
	{
		stop();
	}
}

void InputRecording::handleExceededFrameCounter()
{
	// Writable recordings continue past the end in record mode. Read-only playback stops there.
	if (!m_exit_on_replay_completion && m_frame_counter >= m_file.getTotalFrames() && m_controls.isReplaying())
	{
		if (m_controls.isRecordModeEnabled())
			m_controls.setRecordMode(false);
		else
			VMManager::SetPaused(true);
	}
}

bool InputRecording::needsPowerOnRestart() const
{
	return m_is_active && m_initial_load_complete && m_type == Type::POWER_ON && m_controls.isRecording();
}

bool InputRecording::restartPowerOnRecording(const std::function<void()>& shutdown, const std::function<bool()>& boot)
{
	pxAssert(needsPowerOnRestart());
	// Preserve the session across shutdown without closing it through stop() or queuing a deferred close.
	m_is_active = false;
	shutdown();
	m_is_active = true;
	m_recordingQueue = {};
	m_controls.clearControlQueue();
	if (!m_file.restartPowerOnRecording())
	{
		closeActiveFile();
		InputRec::log(TRANSLATE_STR("InputRecording", "Failed to restart input recording; recording stopped."), Host::OSD_ERROR_DURATION);
		return false;
	}
	m_starting_frame = 0;
	m_frame_counter_stateless = 0;
	m_video_audio.clear();
	initializeState();
	m_initial_load_complete = false;
	if (!boot())
	{
		closeActiveFile();
		return false;
	}
	m_initial_load_complete = true;
	InputRec::log(TRANSLATE_STR("InputRecording", "Input recording restarted from power-on."), Host::OSD_INFO_DURATION);
	return true;
}

void InputRecording::handleReset()
{
	if (isInputOnly())
		return;
	m_video_audio.clear();
	if (m_initial_load_complete)
	{
		adjustFrameCounterOnReRecord(0);
	}
	m_initial_load_complete = true;
}

void InputRecording::handleLoadingSavestate()
{
	if (isInputOnly())
		return;
	// We need to keep track of the starting internal frame of the recording
	// - For a power-on recording this should already be done - it starts at 0
	// - For save state recordings, this is stored inside the initial save-state
	//
	// Why?
	// - When you re-record you load another save-state which has it's own frame counter
	//   stored within, we use this to adjust the frame we are replaying/recording to
	if (isTypeSavestate() && !m_initial_load_complete)
	{
		setStartingFrame(g_FrameCount);
		m_initial_load_complete = true;
	}
	else
	{
		adjustFrameCounterOnReRecord(g_FrameCount);
		m_watching_for_rerecords = true;
	}
}

bool InputRecording::isTypeSavestate() const
{
	return m_type == Type::FROM_SAVESTATE;
}

void InputRecording::setStartingFrame(u32 startingFrame)
{
	if (m_type == Type::POWER_ON)
	{
		return;
	}
	InputRec::consoleLog(fmt::format("Internal Starting Frame: {}", startingFrame));
	m_starting_frame = startingFrame;
	InformGSThread();
}

u32 InputRecording::getStartingFrame()
{
	return m_starting_frame;
}

void InputRecording::adjustFrameCounterOnReRecord(u32 newFrameCounter)
{
	if (newFrameCounter > m_starting_frame + m_file.getTotalFrames())
	{
		InputRec::consoleLog("Warning, you've loaded PCSX2 emulation to a point after the end of the original recording. This should be avoided.");
		InputRec::consoleLog("Save state's framecount has been ignored, using the max length of the recording instead.");
		m_frame_counter = m_file.getTotalFrames();
		if (getControls().isReplaying())
		{
			getControls().setRecordMode();
		}
		return;
	}
	if (newFrameCounter < m_starting_frame)
	{
		InputRec::consoleLog("Warning, you've loaded PCSX2 emulation to a point before the start of the original recording. This should be avoided.");
		InputRec::consoleLog("Save state's framecount has been ignored, starting from the beginning in replay mode.");
		m_frame_counter = 0;
		if (getControls().isRecording())
		{
			getControls().setReplayMode();
		}
		return;
	}
	else if (newFrameCounter == 0 && getControls().isRecording())
	{
		getControls().setReplayMode();
	}
	m_frame_counter = newFrameCounter - m_starting_frame;
	m_frame_counter_stateless--;
	if (m_controls.isRecording())
		m_file.setTotalFrames(m_frame_counter);
	InformGSThread();
}

InputRecordingControls& InputRecording::getControls()
{
	return m_controls;
}

const InputRecordingFile& InputRecording::getData() const
{
	return m_file;
}

void InputRecording::initializeState()
{
	m_frame_counter = 0;
	m_watching_for_rerecords = false;
	InformGSThread();
}

void InputRecording::InformGSThread()
{
	const bool input_only = g_InputRecording.isInputOnly();
	TinyString recording_active_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Input Recording Active: {}"), g_InputRecording.getData().getFilename());
	TinyString frame_data_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Frame: {}/{} ({})"), g_InputRecording.getFrameCounter(), g_InputRecording.getData().getTotalFrames(), g_InputRecording.getFrameCounterStateless());
	TinyString undo_count_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Undo Count: {}"), g_InputRecording.getData().getUndoCount());

	MTGS::RunOnGSThread([recording_active_message, frame_data_message, undo_count_message, input_only](bool is_recording = g_InputRecording.getControls().isRecording()) {
		g_InputRecordingData.is_recording = is_recording;
		g_InputRecordingData.is_input_only = input_only;
		g_InputRecordingData.recording_active_message = recording_active_message;
		g_InputRecordingData.frame_data_message = frame_data_message;
		g_InputRecordingData.undo_count_message = undo_count_message;
	});
}
