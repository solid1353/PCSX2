// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CaptureScript.h"

#include "GS.h"
#include "GS/GSCapture.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "MTGS.h"
#include "MemoryTypes.h"
#include "Recording/PadData.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <array>
#include <algorithm>

namespace CaptureScript::Runtime
{
	namespace
	{
		std::optional<Runner> s_runner;
		bool s_running = false;
		bool s_complete = false;
		bool s_exit_on_completion = false;
		bool s_video_started = false;
		bool s_video_frame_okay = false;
		s64 s_video_frames = 0;
		std::string s_video_path;
		std::vector<float> s_audio;

		bool ArmCapture(const std::string& path)
		{
			const std::string absolute = Path::IsAbsolute(path) ? path : Path::Combine(FileSystem::GetWorkingDirectory(), path);
			const std::string parent(Path::GetDirectory(absolute));
			if (!FileSystem::DirectoryExists(parent.c_str()))
			{
				Console.Error("Capture script: output directory does not exist: %s", parent.c_str());
				return false;
			}
			s_video_path = path;
			s_video_frames = 0;
			s_audio.clear();
			return true;
		}

		bool StopCapture()
		{
			bool okay = false;
			MTGS::RunOnGSThread([&okay]() {
				const bool complete = s_video_started && GSCapture::IsRecordingReplay() &&
				                      GSCapture::GetVideoFrameCount() == s_video_frames && s_video_frames > 0;
				okay = GSEndCapture() && complete;
			});
			MTGS::WaitGS(false);
			if (okay)
				Console.WriteLnFmt("Capture script: finalized {} frames in '{}'.", s_video_frames, s_video_path);
			s_video_started = false;
			s_video_path.clear();
			s_audio.clear();
			return okay;
		}

		void Finish(const std::string_view error = {})
		{
			s_running = false;
			s_complete = error.empty();
			if (s_complete)
				Console.WriteLnFmt("Capture script complete: {} emulated frames.", s_runner->GetFrameCount());
			else
			{
				Console.Error("Capture script failed: %.*s", static_cast<int>(error.size()), error.data());
				if (s_video_started)
					StopCapture();
			}
			if (!s_complete || s_exit_on_completion)
			{
				Host::RequestVMShutdown(false, false, false);
				if (!s_complete)
					Host::RequestExitApplication(false);
			}
		}
	} // namespace

	bool Configure(const std::string_view text, std::string* error)
	{
		auto program = Parse(text, error);
		if (!program)
			return false;
		s_runner.emplace(std::move(*program));
		s_complete = false;
		return true;
	}

	bool IsActive() { return s_runner.has_value(); }
	bool DidComplete() { return s_complete; }

	void Start(bool exit_on_completion)
	{
		if (!s_runner)
			return;
		s_exit_on_completion = exit_on_completion;
		s_running = true;
		Console.WriteLn("Capture script: starting at power-on.");
		if (!s_runner->Start(ArmCapture, StopCapture))
			Finish(s_runner->GetError());
		else if (s_runner->IsComplete())
			Finish();
		ApplyInput();
	}

	void ApplyInput()
	{
		if (!s_runner)
			return;
		// Full-pressure digital buttons and raw stick bytes, using the replay pad format.
		std::array<u8, 18> state = {0xff, 0xff, 127, 127, 127, 127};
		const auto& sticks = s_runner->GetSticks();
		std::copy(sticks.begin(), sticks.end(), state.begin() + 2);
		const u16 buttons = s_runner->GetButtons();
		state[0] = static_cast<u8>(~buttons);
		state[1] = static_cast<u8>(~(buttons >> 8));
		static constexpr unsigned pressure_bits[] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
		for (size_t i = 0; i < std::size(pressure_bits); i++)
			state[6 + i] = (buttons & (1u << pressure_bits[i])) ? 255 : 0;
		PadData(0, 0, state).OverrideActualController();
		state = {0xff, 0xff, 127, 127, 127, 127};
		PadData(1, 0, state).OverrideActualController();
	}

	void CollectAudioSample(const float* sample)
	{
		if (s_running && s_runner->IsCapturing())
			s_audio.insert(s_audio.end(), sample, sample + 2);
	}

	void PrepareVideoFrame()
	{
		if (!s_running || !s_runner->IsCapturing())
			return;
		s_video_frame_okay = false;
		MTGS::RunOnGSThread([samples = std::move(s_audio)]() mutable {
			auto deliver = [samples = std::move(samples)]() {
				s_video_frame_okay = (s_video_started || (!GSCapture::IsCapturing() && GSBeginReplayCapture(s_video_path))) &&
				                     GSCapture::DeliverReplayAudio(samples);
			};
			if (s_video_started)
				deliver();
			else
				GSQueueReplayCapture(std::move(deliver));
		});
		s_audio.clear();
	}

	void EndFrame()
	{
		if (!s_running)
			return;
		if (s_runner->IsCapturing())
		{
			bool okay = false;
			// This queue entry follows the frame's VSync, including its video delivery.
			MTGS::RunOnGSThread([&okay]() {
				okay = s_video_frame_okay && GSCapture::IsRecordingReplay() &&
				       GSCapture::GetVideoFrameCount() == s_video_frames + 1;
			});
			MTGS::WaitGS(false);
			if (!okay)
			{
				// BeginCapture may have opened a partial file before returning an error.
				MTGS::RunOnGSThread(&GSEndCapture);
				MTGS::WaitGS(false);
				s_video_started = false;
				Finish("could not start capture or write this frame's video/audio");
				return;
			}
			s_video_started = true;
			s_video_frames++;
		}
		if (!s_runner->EndFrame(std::span<const u8>(eeMem->Main, Ps2MemSize::ExposedRam), ArmCapture, StopCapture))
			Finish(s_runner->GetError());
		else if (s_runner->IsComplete())
			Finish();
	}

	void Shutdown()
	{
		if (s_running)
		{
			Console.Error("Capture script interrupted before completion.");
			s_complete = false;
		}
		if (s_video_started)
			StopCapture();
		s_running = false;
		s_audio.clear();
		s_runner.reset();
	}
} // namespace CaptureScript::Runtime
