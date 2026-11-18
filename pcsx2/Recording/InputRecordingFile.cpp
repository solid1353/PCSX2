// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "InputRecordingFile.h"
#include "InputRecording.h"

#include "BuildVersion.h"
#include "Utilities/InputRecordingLogger.h"

#include "common/FileSystem.h"
#include "common/Error.h"
#include "common/Path.h"

#include <fmt/format.h>

#include <vector>
#include <array>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

void InputRecordingFile::InputRecordingFileHeader::init() noexcept
{
	m_fileVersion = 1;
}

void InputRecordingFile::setEmulatorVersion()
{
	snprintf(m_header.m_emulatorVersion, sizeof(m_header.m_emulatorVersion), "PCSX2-%s", BuildVersion::GitRev);
}

void InputRecordingFile::setAuthor(const std::string& _author)
{
	strncpy(m_header.m_author, _author.data(), sizeof(m_header.m_author) - 1);
}

void InputRecordingFile::setGameName(const std::string& _gameName)
{
	strncpy(m_header.m_gameName, _gameName.data(), sizeof(m_header.m_gameName) - 1);
}

const char* InputRecordingFile::getEmulatorVersion() const noexcept
{
	return m_header.m_emulatorVersion;
}

const char* InputRecordingFile::getAuthor() const noexcept
{
	return m_header.m_author;
}

const char* InputRecordingFile::getGameName() const noexcept
{
	return m_header.m_gameName;
}

bool InputRecordingFile::close() noexcept
{
	if (m_recordingFile == nullptr)
	{
		return false;
	}
	const bool okay = !ferror(m_recordingFile);
	const int result = fclose(m_recordingFile);
	m_recordingFile = nullptr;
	m_writable = false;
	m_filename.clear();
	return okay && result == 0;
}

const std::string& InputRecordingFile::getFilename() const noexcept
{
	return m_filename;
}

u32 InputRecordingFile::getTotalFrames() const noexcept
{
	return m_totalFrames;
}

u32 InputRecordingFile::getUndoCount() const noexcept
{
	return m_undoCount;
}

bool InputRecordingFile::fromSaveState() const noexcept
{
	return m_start_type == StartType::SaveState;
}

void InputRecordingFile::incrementUndoCount()
{
	if (m_recordingFile == nullptr || !m_writable)
	{
		return;
	}

	m_undoCount++;
	fseek(m_recordingFile, s_seekpointUndoCount, SEEK_SET);
	fwrite(&m_undoCount, 4, 1, m_recordingFile);
	InputRecording::InformGSThread();
}

bool InputRecordingFile::openNew(const std::string& path, bool fromSavestate)
{
	return openNew(path, fromSavestate ? StartType::SaveState : StartType::PowerOn);
}

bool InputRecordingFile::openNew(const std::string& path, StartType start_type)
{
	const std::string directory(Path::GetDirectory(path));
	Error error;
	if (!directory.empty() && !FileSystem::CreateDirectoryPath(directory.c_str(), true, &error))
	{
		InputRec::consoleLog(fmt::format("Input recording directory creation failed: {}", error.GetDescription()));
		return false;
	}

	if ((m_recordingFile = FileSystem::OpenCFile(path.data(), "wb+")) == nullptr)
	{
		InputRec::consoleLog(fmt::format("Input recording file opening failed. Error - {}", strerror(errno)));
		return false;
	}

	m_filename = path;
	m_writable = true;
	m_totalFrames = 0;
	m_undoCount = 0;
	m_header = {};
	m_header.init();
	m_header.m_fileVersion = start_type == StartType::None ? 2 : 1;
	m_start_type = start_type;
	InputRecording::InformGSThread();
	return true;
}

bool InputRecordingFile::restartPowerOnRecording()
{
	if (!m_recordingFile || !m_writable || m_start_type != StartType::PowerOn ||
		fflush(m_recordingFile) != 0)
	{
		return false;
	}

	const size_t header_size = getRecordingBlockSeekPoint(0);
#ifdef _WIN32
	if (_chsize_s(_fileno(m_recordingFile), header_size) != 0)
#else
	if (ftruncate(fileno(m_recordingFile), static_cast<off_t>(header_size)) != 0)
#endif
	{
		return false;
	}

	m_totalFrames = 0;
	m_undoCount = 0;
	return writeHeader() && fflush(m_recordingFile) == 0;
}

bool InputRecordingFile::openExisting(const std::string& path)
{
	if ((m_recordingFile = FileSystem::OpenSharedCFile(
			 path.data(), "rb", FileSystem::FileShareMode::DenyWrite)) == nullptr)
	{
		InputRec::consoleLog(fmt::format("Input recording file opening failed. Error - {}", strerror(errno)));
		return false;
	}

	if (!verifyRecordingFileHeader())
	{
		close();
		InputRec::consoleLog("Input recording file header is invalid");
		return false;
	}

	m_filename = path;
	m_writable = false;
	InputRecording::InformGSThread();
	return true;
}

std::optional<PadData> InputRecordingFile::readPadData(const uint frame, const uint port, const uint slot)
{
	if (m_recordingFile == nullptr || port >= s_controllerPortsSupported || slot != 0)
	{
		return std::nullopt;
	}

	std::array<u8, s_controllerInputBytes> data{};

	// TODO - slot unused, use it in the new format
	const size_t seek = getRecordingBlockSeekPoint(frame) + s_controllerInputBytes * port;
	if (fseek(m_recordingFile, seek, SEEK_SET) != 0 || fread(data.data(), 1, data.size(), m_recordingFile) != data.size())
	{
		return std::nullopt;
	}

	return PadData(port, slot, data);
}

void InputRecordingFile::setTotalFrames(u32 frame)
{
	if (m_recordingFile == nullptr || !m_writable)
	{
		return;
	}
	m_totalFrames = frame;
	fseek(m_recordingFile, s_seekpointTotalFrames, SEEK_SET);
	fwrite(&m_totalFrames, 4, 1, m_recordingFile);
	InputRecording::InformGSThread();
}

bool InputRecordingFile::writeHeader() const
{
	if (m_recordingFile == nullptr || !m_writable)
	{
		return false;
	}
	rewind(m_recordingFile);
	if (fwrite(&m_header, sizeof(InputRecordingFileHeader), 1, m_recordingFile) != 1 ||
		fwrite(&m_totalFrames, 4, 1, m_recordingFile) != 1 ||
		fwrite(&m_undoCount, 4, 1, m_recordingFile) != 1 ||
		fwrite(&m_start_type, 1, 1, m_recordingFile) != 1)
	{
		return false;
	}
	return true;
}

bool InputRecordingFile::writePadData(const uint frame, const PadData data) const
{
	if (m_recordingFile == nullptr || !m_writable)
	{
		return false;
	}

	// TODO - use the slot in the future
	const size_t seek = getRecordingBlockSeekPoint(frame) + s_controllerInputBytes * data.m_port;

	// seek to the correct position and write data to the file
	if (fseek(m_recordingFile, seek, SEEK_SET) != 0 ||
		fwrite(&data.m_compactPressFlagsGroupOne, 1, 1, m_recordingFile) != 1 ||
		fwrite(&data.m_compactPressFlagsGroupTwo, 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<0>(data.m_rightAnalog), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_rightAnalog), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<0>(data.m_leftAnalog), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_leftAnalog), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_right), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_left), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_up), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_down), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_triangle), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_circle), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_cross), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_square), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_l1), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_r1), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_l2), 1, 1, m_recordingFile) != 1 ||
		fwrite(&std::get<1>(data.m_r2), 1, 1, m_recordingFile) != 1)
	{
		return false;
	}

	fflush(m_recordingFile);
	return true;
}

void InputRecordingFile::logRecordingMetadata()
{
	InputRec::consoleMultiLog({fmt::format("File: {}", getFilename()),
		fmt::format("PCSX2 Version Used: {}", m_header.m_emulatorVersion),
		fmt::format("Recording File Version: {}", m_header.m_fileVersion),
		fmt::format("Associated Game Name or ISO Filename: {}", m_header.m_gameName),
		fmt::format("Author: {}", m_header.m_author),
		fmt::format("Total Frames: {}", getTotalFrames()),
		fmt::format("Undo Count: {}", getUndoCount())});
}

std::vector<PadData> InputRecordingFile::bulkReadPadData(u32 frameStart, u32 frameEnd, const uint port)
{
	std::vector<PadData> data;

	if (m_recordingFile == nullptr || frameEnd < frameStart)
	{
		return data;
	}

	// TODO - no multi-tap support
	for (u32 currFrame = frameStart; currFrame < frameEnd; currFrame++)
	{
		const auto padData = readPadData(currFrame, port, 0);
		if (padData)
		{
			data.push_back(padData.value());
		}
	}
	return data;
}

size_t InputRecordingFile::getRecordingBlockSeekPoint(const u32 frame) const noexcept
{
	return s_headerSize + sizeof(StartType) + static_cast<size_t>(frame) * s_inputBytesPerFrame;
}

bool InputRecordingFile::verifyRecordingFileHeader()
{
	if (m_recordingFile == nullptr)
	{
		return false;
	}
	// Verify header contents
	rewind(m_recordingFile);
	if (fread(&m_header, sizeof(InputRecordingFileHeader), 1, m_recordingFile) != 1 ||
		fread(&m_totalFrames, 4, 1, m_recordingFile) != 1 ||
		fread(&m_undoCount, 4, 1, m_recordingFile) != 1 ||
		fread(&m_start_type, sizeof(StartType), 1, m_recordingFile) != 1)
	{
		return false;
	}

	if (m_header.m_fileVersion != 1 && m_header.m_fileVersion != 2)
	{
		InputRec::consoleLog(fmt::format("Input recording file is not a supported version - {}", m_header.m_fileVersion));
		return false;
	}
	if (static_cast<u8>(m_start_type) > static_cast<u8>(StartType::None) ||
		(m_header.m_fileVersion == 1 && m_start_type == StartType::None))
	{
		InputRec::consoleLog("Input recording has an invalid starting-state type");
		return false;
	}
	return true;
}
