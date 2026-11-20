// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CaptureScript.h"

#include "common/FileSystem.h"

#include "fmt/format.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

namespace CaptureScript
{
	std::optional<u64> Read::ResolveAddress(const std::span<const u8> ram) const
	{
		u64 address = offset;
		if (address > std::numeric_limits<u32>::max())
			return std::nullopt;
		if (pointer)
		{
			const auto base = pointer->Evaluate(ram);
			// Reject a null pointer before adding offsets: [null+8] is not RAM at address 8.
			if (!base || *base <= 0 || static_cast<u64>(*base) > std::numeric_limits<u32>::max() - address)
				return std::nullopt;
			address += *base;
		}
		return address;
	}

	std::optional<s64> Read::Evaluate(const std::span<const u8> ram) const
	{
		const auto resolved = ResolveAddress(ram);
		if (!resolved)
			return std::nullopt;
		const u64 address = *resolved;
		const size_t size = type == Type::U32 ? 4 : (type == Type::U8 ? 1 : 2);
		if (address > ram.size() || size > ram.size() - address)
			return std::nullopt;
		u32 bits = 0;
		for (size_t i = 0; i < size; i++)
			bits |= static_cast<u32>(ram[address + i]) << (8 * i);
		s64 value = type == Type::S16 ? static_cast<s16>(bits) : static_cast<s64>(bits);
		for (const Transform& transform : transforms)
			value = transform.shift ? (value >> transform.operand) : (value & transform.operand);
		return value;
	}

	bool Comparison::Evaluate(const std::span<const u8> ram) const
	{
		const auto value = read.Evaluate(ram);
		if (!value)
			return false;
		switch (op)
		{
			case Operator::Equal:
				return *value == number;
			case Operator::NotEqual:
				return *value != number;
			case Operator::Less:
				return *value < number;
			case Operator::LessEqual:
				return *value <= number;
			case Operator::Greater:
				return *value > number;
			case Operator::GreaterEqual:
				return *value >= number;
		}
		return false;
	}

	namespace
	{
		class Parser
		{
		public:
			explicit Parser(std::string_view text)
				: m_text(text)
			{
			}
			std::optional<Program> ParseProgram()
			{
				Program result;
				bool capturing = false;
				while (!AtEnd())
				{
					Statement statement{};
					std::string_view command = Word();
					if (command == "pad0" || command == "pad1")
					{
						statement.pad = command == "pad1" ? 1 : 0;
						command = Word();
						if (command != "hold" && command != "sticks")
							return Error("a pad prefix must be followed by hold or sticks");
					}
					if (command == "wait")
					{
						statement.type = Statement::Type::Wait;
						Read first_read;
						if (!ParseRead(first_read, 0))
							return std::nullopt;
						if (Word() == "changes")
						{
							statement.type = Statement::Type::WaitChanges;
							statement.change_read = std::move(first_read);
						}
						else
						{
							statement.condition.emplace_back();
							for (;;)
							{
								Comparison comparison{};
								comparison.read = std::move(first_read);
								if (!ParseOperator(comparison.op) || !SignedNumber(comparison.number))
									return std::nullopt;
								statement.condition.back().push_back(std::move(comparison));
								if (Take("&&"))
								{
									first_read = Read{};
									if (!ParseRead(first_read, 0))
										return std::nullopt;
									continue;
								}
								if (Take("||"))
								{
									statement.condition.emplace_back();
									first_read = Read{};
									if (!ParseRead(first_read, 0))
										return std::nullopt;
									continue;
								}
								break;
							}
						}
						if (Word() != "within" || !Number(statement.frames) || statement.frames == 0)
							return Error("expected 'within' and a positive frame count");
					}
					else if (command == "run")
					{
						statement.type = Statement::Type::Run;
						if (!Number(statement.frames))
							return std::nullopt;
					}
					else if (command == "hold")
					{
						statement.type = Statement::Type::Hold;
						// Bits use the DualShock wire order, with one meaning pressed here.
						static constexpr std::string_view names[] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
							"l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
						const std::string_view first = Word();
						if (first != "none")
						{
							std::string_view button = first;
							for (;;)
							{
								const auto found = std::find(std::begin(names), std::end(names), button);
								if (found == std::end(names))
									return Error("unknown button");
								statement.buttons |= static_cast<u16>(1u << (found - std::begin(names)));
								if (!Take("+"))
									break;
								button = Word();
							}
						}
					}
					else if (command == "sticks")
					{
						statement.type = Statement::Type::Sticks;
						for (u8& axis : statement.sticks)
						{
							u64 value;
							if (!Number(value) || value > 255)
								return Error("sticks requires four axis values in 0..255 (right X/Y, left X/Y)");
							axis = static_cast<u8>(value);
						}
					}
					else if (command == "capture" || command == "load")
					{
						statement.type = command == "load" ? Statement::Type::Load : Statement::Type::Capture;
						if (command == "capture" && capturing)
							return Error("capture is already active");
						SkipSpace();
						const size_t begin = m_pos;
						while (m_pos < m_text.size() && m_text[m_pos] != ';')
							m_pos++;
						std::string_view path = m_text.substr(begin, m_pos - begin);
						while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back())))
							path.remove_suffix(1);
						if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
							path = path.substr(1, path.size() - 2);
						if (path.empty() || path.find('\0') != std::string_view::npos)
							return Error("expected a non-empty file path");
						if (command == "capture" && (path.size() <= 4 || path.substr(path.size() - 4) != ".mkv"))
							return Error("capture requires an .mkv path");
						statement.path = path;
						if (command == "capture")
							capturing = true;
					}
					else if (command == "stop")
					{
						statement.type = Statement::Type::Stop;
						if (!capturing)
							return Error("stop requires an active capture");
						capturing = false;
					}
					else
					{
						return Error("expected wait, load, run, hold, sticks, capture, or stop");
					}
					result.push_back(std::move(statement));
					if (!AtEnd() && !Take(";"))
						return Error("expected ';' between statements");
				}
				if (result.empty() || capturing)
					return Error(capturing ? "capture requires a final stop" : "script is empty");
				return result;
			}
			std::string GetError() const { return fmt::format("at byte {}: {}", m_pos + 1, m_error); }

			bool ParseMemoryLine(Read& address, std::vector<u8>& bytes)
			{
				if (!ParseAddress(address, 0))
					return false;
				if (m_pos == m_text.size() || !std::isspace(static_cast<unsigned char>(m_text[m_pos])))
				{
					Error("expected whitespace and hex bytes after address");
					return false;
				}
				SkipSpace();
				const size_t begin = m_pos;
				while (m_pos < m_text.size() && std::isxdigit(static_cast<unsigned char>(m_text[m_pos])))
					m_pos++;
				const size_t digits = m_pos - begin;
				if (digits == 0 || digits % 2 != 0 || !AtEnd())
				{
					Error("expected a non-empty, even number of hex digits");
					return false;
				}
				for (size_t i = 0; i < digits; i += 2)
				{
					unsigned byte;
					const char* end = m_text.data() + begin + i + 2;
					const auto parsed = std::from_chars(end - 2, end, byte, 16);
					if (parsed.ec != std::errc() || parsed.ptr != end)
					{
						Error("invalid hex byte");
						return false;
					}
					bytes.push_back(static_cast<u8>(byte));
				}
				return true;
			}

		private:
			std::nullopt_t Error(std::string_view reason)
			{
				m_error = reason;
				return std::nullopt;
			}
			void SkipSpace()
			{
				while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
					m_pos++;
			}
			bool AtEnd()
			{
				SkipSpace();
				return m_pos == m_text.size();
			}
			bool Take(std::string_view token)
			{
				SkipSpace();
				if (m_text.substr(m_pos, token.size()) != token)
					return false;
				m_pos += token.size();
				return true;
			}
			std::string_view Word()
			{
				SkipSpace();
				const size_t begin = m_pos;
				while (m_pos < m_text.size() && std::isalnum(static_cast<unsigned char>(m_text[m_pos])))
					m_pos++;
				return m_text.substr(begin, m_pos - begin);
			}
			bool Number(u64& value)
			{
				SkipSpace();
				int base = Take("0x") ? 16 : 10;
				const char* begin = m_text.data() + m_pos;
				const auto parsed = std::from_chars(begin, m_text.data() + m_text.size(), value, base);
				if (parsed.ec != std::errc() || value > static_cast<u64>(std::numeric_limits<s64>::max()))
				{
					Error("invalid or overflowing number");
					return false;
				}
				m_pos += parsed.ptr - begin;
				return true;
			}
			bool SignedNumber(s64& value)
			{
				const bool negative = Take("-");
				u64 number;
				if (!Number(number))
					return false;
				value = negative ? -static_cast<s64>(number) : static_cast<s64>(number);
				return true;
			}
			bool ParseRead(Read& read, unsigned depth)
			{
				if (depth >= 64)
				{
					Error("pointer nesting exceeds 64 reads");
					return false;
				}
				if (Take("sh"))
					read.type = Read::Type::S16;
				else if (Take("h"))
					read.type = Read::Type::U16;
				else if (Take("b"))
					read.type = Read::Type::U8;
				if (!Take("["))
				{
					Error("expected a memory read");
					return false;
				}
				if (!ParseAddress(read, depth))
					return false;
				if (!Take("]"))
				{
					Error("invalid address or missing ']'");
					return false;
				}
				bool shifted = false, masked = false;
				for (;;)
				{
					const size_t before_space = m_pos;
					bool shift;
					if (Take(">>"))
						shift = true;
					else if (m_text.substr(m_pos, 2) != "&&" && Take("&"))
						shift = false;
					else
					{
						m_pos = before_space;
						break;
					}
					u64 operand;
					if (!Number(operand))
						return false;
					if ((shift && (shifted || operand > 31)) || (!shift && (masked || operand > std::numeric_limits<u32>::max())))
					{
						Error("invalid or repeated shift/mask");
						return false;
					}
					(shift ? shifted : masked) = true;
					read.transforms.push_back({shift, static_cast<u32>(operand)});
				}
				return true;
			}
			bool ParseAddress(Read& read, unsigned depth)
			{
				SkipSpace();
				if (m_pos < m_text.size() && (m_text[m_pos] == '[' || m_text[m_pos] == 'h' || m_text[m_pos] == 's' || m_text[m_pos] == 'b'))
				{
					read.pointer = std::make_unique<Read>();
					if (!ParseRead(*read.pointer, depth + 1))
						return false;
				}
				else if (!Number(read.offset))
				{
					return false;
				}
				for (;;)
				{
					const size_t before_space = m_pos;
					if (!Take("+"))
					{
						m_pos = before_space;
						break;
					}
					u64 offset;
					if (!Number(offset))
						return false;
					if (offset > std::numeric_limits<u32>::max() || read.offset > std::numeric_limits<u32>::max() - offset)
					{
						Error("address offset overflows EE address space");
						return false;
					}
					read.offset += offset;
				}
				if (read.offset > std::numeric_limits<u32>::max())
				{
					Error("address overflows EE address space");
					return false;
				}
				return true;
			}
			bool ParseOperator(Comparison::Operator& op)
			{
				static constexpr std::string_view tokens[] = {"==", "!=", "<=", ">=", "<", ">"};
				static constexpr Comparison::Operator ops[] = {Comparison::Operator::Equal, Comparison::Operator::NotEqual,
					Comparison::Operator::LessEqual, Comparison::Operator::GreaterEqual, Comparison::Operator::Less, Comparison::Operator::Greater};
				for (size_t i = 0; i < std::size(tokens); i++)
				{
					if (Take(tokens[i]))
					{
						op = ops[i];
						return true;
					}
				}
				Error("expected a comparison operator");
				return false;
			}
			std::string_view m_text;
			size_t m_pos = 0;
			std::string m_error;
		};

		bool LoadMemory(const std::string& path, std::span<u8> ram, std::string* error)
		{
			const auto text = FileSystem::ReadFileToString(path.c_str());
			if (!text)
			{
				*error = fmt::format("could not read load file '{}'", path);
				return false;
			}
			struct Write
			{
				u64 address;
				std::vector<u8> bytes;
			};
			std::vector<Write> writes;
			std::string_view remaining(*text);
			for (size_t line_number = 1; !remaining.empty(); line_number++)
			{
				const size_t newline = remaining.find('\n');
				const std::string_view line = remaining.substr(0, newline);
				remaining.remove_prefix(newline == std::string_view::npos ? remaining.size() : newline + 1);
				if (std::ranges::all_of(line, [](unsigned char ch) { return std::isspace(ch); }))
					continue;
				Parser parser(line);
				Read destination;
				Write write{};
				std::string reason;
				if (!parser.ParseMemoryLine(destination, write.bytes))
					reason = parser.GetError();
				else if (const auto address = destination.ResolveAddress(ram); !address)
					reason = "address pointer read failed (null, overflowing, or outside EE RAM)";
				else if (*address > ram.size() || write.bytes.size() > ram.size() - *address)
					reason = "write would leave exposed EE RAM";
				else
					write.address = *address;
				if (!reason.empty())
				{
					*error = fmt::format("load file '{}', line {}: {}", path, line_number, reason);
					return false;
				}
				writes.push_back(std::move(write));
			}
			// Resolve every pointer against the original RAM before applying any writes.
			// Cross-build replay determinism needs proper testing with actual game takes.
			for (const Write& write : writes)
				std::copy(write.bytes.begin(), write.bytes.end(), ram.begin() + write.address);
			return true;
		}
	} // namespace

	std::optional<Program> Parse(std::string_view text, std::string* error)
	{
		Parser parser(text);
		auto result = parser.ParseProgram();
		if (error)
			*error = result ? std::string() : parser.GetError();
		return result;
	}

	Runner::Runner(Program program)
		: m_program(std::move(program))
	{
	}

	bool Runner::Fail(std::string_view reason)
	{
		m_error = fmt::format("statement {}, frame {}: {}", m_statement + 1, m_frame, reason);
		return false;
	}

	bool Runner::Execute(std::span<u8> ram, const CaptureAction& capture, const StopAction& stop)
	{
		while (m_statement < m_program.size())
		{
			const Statement& statement = m_program[m_statement];
			switch (statement.type)
			{
				case Statement::Type::Wait:
					return true;
				case Statement::Type::WaitChanges:
					m_initial_value = statement.change_read.Evaluate(ram);
					return true;
				case Statement::Type::Load:
				{
					std::string error;
					if (!LoadMemory(statement.path, ram, &error))
						return Fail(error);
					break;
				}
				case Statement::Type::Run:
					if (statement.frames != 0)
						return true;
					break;
				case Statement::Type::Hold:
					m_buttons[statement.pad] = statement.buttons;
					break;
				case Statement::Type::Sticks:
					m_sticks[statement.pad] = statement.sticks;
					break;
				case Statement::Type::Capture:
					if (!capture(statement.path))
						return Fail("could not start capture");
					m_capturing = true;
					break;
				case Statement::Type::Stop:
					if (!stop())
						return Fail("could not finalize capture");
					m_capturing = false;
					break;
			}
			m_statement++;
		}
		return true;
	}

	bool Runner::Start(std::span<u8> ram, const CaptureAction& capture, const StopAction& stop)
	{
		return Execute(ram, capture, stop);
	}

	bool Runner::EndFrame(std::span<u8> ram, const CaptureAction& capture, const StopAction& stop)
	{
		if (!m_error.empty())
			return false;
		if (IsComplete())
			return true;
		m_frame++;
		m_elapsed++;
		const Statement& statement = m_program[m_statement];
		if (statement.type == Statement::Type::Wait)
		{
			const bool satisfied = std::ranges::any_of(statement.condition, [ram](const auto& group) {
				return std::ranges::all_of(group, [ram](const Comparison& comparison) { return comparison.Evaluate(ram); });
			});
			if (!satisfied)
				return m_elapsed < statement.frames || Fail("wait timed out");
		}
		else if (statement.type == Statement::Type::WaitChanges)
		{
			const auto value = statement.change_read.Evaluate(ram);
			if (!m_initial_value || !value || *value == *m_initial_value)
				return m_elapsed < statement.frames || Fail("wait for change timed out");
		}
		else if (m_elapsed < statement.frames)
		{
			return true;
		}
		m_statement++;
		m_elapsed = 0;
		return Execute(ram, capture, stop);
	}
} // namespace CaptureScript
