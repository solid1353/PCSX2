// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228UJFrame.h"
#include "NA228ResourceMonitor.h"

#include "fmt/format.h"

#include <algorithm>
#include <charconv>

namespace NA228UJFrame
{
	static constexpr uint32_t PHYSICAL_MASK = 0x1fffffff;

	// NA2.28 resident addresses and structure offsets.
	static constexpr uint32_t JUTSU_SKILL_PLAY_OWNER = 0x00607714;
	static constexpr uint32_t JUTSU_SKILL_REQUESTS = 0x00607748;
	static constexpr int64_t OWNER_CONTAINER = 0x04;
	static constexpr int64_t OWNER_ATTACKER = 0x36;
	static constexpr int64_t OWNER_OPPONENT = 0x38;
	static constexpr int64_t OWNER_SKILL = 0x3c;
	static constexpr int64_t OWNER_REQUEST_ORDINAL = 0x16a;
	static constexpr int64_t CONTAINER_NAME = 0x04;
	static constexpr uint32_t CONTAINER_NAME_SIZE = 32;
	static constexpr int64_t CONTAINER_FRAME = 0x90;
	static constexpr int64_t REQUEST_SIZE = 0x18;
	static constexpr int64_t REQUEST_STREAM_COUNT = 0x09;

	static constexpr std::string_view MARKS_HEADER = "mark\ttime\tbuild\tfilm\tpart\tframe\tskill\tattacker\topponent\n";

	// Reads a little-endian value at base + offset. Fails when base is null or outside EE RAM, or the value
	// does not lie entirely inside it.
	static bool ReadValue(std::span<const uint8_t> ram, uint32_t base, int64_t offset, uint32_t size, uint32_t& value)
	{
		const int64_t physical_base = base & PHYSICAL_MASK;
		const int64_t physical = physical_base + offset;
		const int64_t ram_size = static_cast<int64_t>(ram.size());
		if (base == 0 || physical_base >= ram_size || physical < 0 || physical + size > ram_size)
			return false;

		// Live EE memory can change between reads, as in the Resource Monitor.
		const volatile uint8_t* bytes = ram.data() + physical;
		value = 0;
		for (uint32_t i = 0; i < size; i++)
			value |= uint32_t(bytes[i]) << (i * 8);
		return true;
	}

	// A source game's skill-play owner and the offsets that lead to its film's frame (SLUS_217.27 and
	// SLUS_218.62 annotations: sp_skill_play_owner and jutsu_skill_play_owner).
	struct SourceGameFilm
	{
		uint32_t owner;
		int64_t parts_begun;
		int64_t frame;
	};
	static constexpr int64_t SOURCE_OWNER_CONTAINER = 0x04;
	static constexpr SourceGameFilm UN3_FILM = {0x006a3a18, 0x86, 0x1ec};
	static constexpr SourceGameFilm UN4_FILM = {0x00609800, 0x16a, 0x1bc};

	static std::optional<Film> ReadSourceGame(std::span<const uint8_t> ram, const SourceGameFilm& layout)
	{
		uint32_t owner, parts, container, frame;
		if (!ReadValue(ram, layout.owner, 0, 4, owner) ||
			!ReadValue(ram, owner, layout.parts_begun, 2, parts) || static_cast<int16_t>(parts) <= 0 ||
			!ReadValue(ram, owner, SOURCE_OWNER_CONTAINER, 4, container) ||
			!ReadValue(ram, container, layout.frame, 4, frame))
		{
			return std::nullopt;
		}

		Film film;
		film.frame = static_cast<int32_t>(frame);
		return film;
	}

	std::optional<Film> Read(std::span<const uint8_t> ram, uint32_t disc_crc)
	{
		ram = ram.first(std::min<size_t>(ram.size(), NA228ResourceMonitor::EE_RAM_SIZE));
		if (disc_crc == UN3_DISC_CRC)
			return ReadSourceGame(ram, UN3_FILM);
		if (disc_crc == UN4_DISC_CRC)
			return ReadSourceGame(ram, UN4_FILM);
		if (!NA228ResourceMonitor::Detect(ram))
			return std::nullopt;

		uint32_t owner, part, container, frame, skill, attacker, opponent, requests;
		if (!ReadValue(ram, JUTSU_SKILL_PLAY_OWNER, 0, 4, owner) ||
			!ReadValue(ram, owner, OWNER_REQUEST_ORDINAL, 2, part) || static_cast<int16_t>(part) <= 0 ||
			!ReadValue(ram, owner, OWNER_CONTAINER, 4, container) ||
			!ReadValue(ram, container, CONTAINER_FRAME, 4, frame) ||
			!ReadValue(ram, owner, OWNER_SKILL, 2, skill) ||
			!ReadValue(ram, owner, OWNER_ATTACKER, 2, attacker) ||
			!ReadValue(ram, owner, OWNER_OPPONENT, 2, opponent) ||
			!ReadValue(ram, JUTSU_SKILL_REQUESTS, 0, 4, requests))
		{
			return std::nullopt;
		}

		// Retail request rows hold at most one stream; only multi-part film ports show the part.
		uint32_t streams = 0;
		if (requests != 0 &&
			!ReadValue(ram, requests, static_cast<int16_t>(skill) * REQUEST_SIZE + REQUEST_STREAM_COUNT, 1, streams))
		{
			return std::nullopt;
		}

		Film film;
		for (uint32_t i = 0; i < CONTAINER_NAME_SIZE; i++)
		{
			uint32_t character;
			if (!ReadValue(ram, container, CONTAINER_NAME + i, 1, character) || character == 0)
				break;
			// Keep the marks file's columns and lines intact.
			film.name.push_back((character < 0x20 || character == 0x7f) ? ' ' : static_cast<char>(character));
		}
		if (streams > 1)
			film.part = static_cast<int16_t>(part);
		film.frame = static_cast<int32_t>(frame);
		film.skill = static_cast<int16_t>(skill);
		film.attacker = static_cast<int16_t>(attacker);
		film.opponent = static_cast<int16_t>(opponent);
		return film;
	}

	std::string FormatPosition(const Film& film)
	{
		return film.part.has_value() ? fmt::format("{}:{}", *film.part, film.frame) : fmt::format("{}", film.frame);
	}

	std::string Format(std::span<const uint8_t> ram, uint32_t disc_crc)
	{
		const std::optional<Film> film = Read(ram, disc_crc);
		return film.has_value() ? fmt::format("UJ frame {}", FormatPosition(*film)) : std::string();
	}

	std::string MarkText(std::string_view contents, const Film& film, std::string_view time, std::string_view build,
		uint32_t* mark)
	{
		uint32_t highest = 0;
		for (size_t start = 0; start < contents.size();)
		{
			const size_t end = std::min(contents.find('\n', start), contents.size());
			const std::string_view line = contents.substr(start, end - start);
			uint32_t value;
			const auto result = std::from_chars(line.data(), line.data() + line.size(), value);
			if (result.ec == std::errc() && (result.ptr == line.data() + line.size() || *result.ptr == '\t'))
				highest = std::max(highest, value);
			start = end + 1;
		}
		*mark = highest + 1;

		std::string text;
		if (contents.empty())
			text = MARKS_HEADER;
		else if (contents.back() != '\n')
			text = "\n";
		text += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", *mark, time, build, film.name,
			film.part.has_value() ? fmt::format("{}", *film.part) : std::string(), film.frame, film.skill,
			film.attacker, film.opponent);
		return text;
	}
} // namespace NA228UJFrame
