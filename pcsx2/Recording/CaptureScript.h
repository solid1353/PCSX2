// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CaptureScript
{

	struct Read
	{
		enum class Type
		{
			U32,
			U16,
			S16,
			U8
		};
		struct Transform
		{
			bool shift;
			u32 operand;
		};
		Type type = Type::U32;
		std::unique_ptr<Read> pointer;
		u64 offset = 0;
		std::vector<Transform> transforms;
		std::optional<s64> Evaluate(std::span<const u8> ram) const;
	};

	struct Comparison
	{
		enum class Operator
		{
			Equal,
			NotEqual,
			Less,
			LessEqual,
			Greater,
			GreaterEqual
		};
		Read read;
		Operator op;
		s64 number;
		bool Evaluate(std::span<const u8> ram) const;
	};

	// OR of AND groups: && binds more tightly than ||.
	using Condition = std::vector<std::vector<Comparison>>;
	struct Statement
	{
		enum class Type
		{
			Wait,
			Run,
			Hold,
			Capture,
			Stop
		};
		Type type;
		u64 frames = 0;
		u16 buttons = 0;
		Condition condition;
		std::string path;
	};
	using Program = std::vector<Statement>;
	std::optional<Program> Parse(std::string_view text, std::string* error);

	class Runner
	{
	public:
		// Capture arms the NEXT interval. Stop finalizes the already completed intervals.
		using CaptureAction = std::function<bool(const std::string&)>;
		using StopAction = std::function<bool()>;
		explicit Runner(Program program);
		bool Start(const CaptureAction& capture, const StopAction& stop);
		bool EndFrame(std::span<const u8> ram, const CaptureAction& capture, const StopAction& stop);
		bool IsComplete() const { return m_statement == m_program.size() && m_error.empty(); }
		bool IsCapturing() const { return m_capturing; }
		u16 GetButtons() const { return m_buttons; }
		u64 GetFrameCount() const { return m_frame; }
		const std::string& GetError() const { return m_error; }

	private:
		bool Execute(const CaptureAction& capture, const StopAction& stop);
		bool Fail(std::string_view reason);
		Program m_program;
		size_t m_statement = 0;
		u64 m_frame = 0;
		u64 m_elapsed = 0;
		u16 m_buttons = 0;
		bool m_capturing = false;
		std::string m_error;
	};

	// Launch-only runtime. Configure before CPU reset so RTC starts deterministically.
	namespace Runtime
	{
		bool Configure(std::string_view text, std::string* error);
		bool IsActive();
		bool DidComplete();
		void Start(bool exit_on_completion);
		void PrepareVideoFrame();
		void EndFrame();
		void ApplyInput();
		void CollectAudioSample(const float* sample);
		void Shutdown();
	} // namespace Runtime
} // namespace CaptureScript
