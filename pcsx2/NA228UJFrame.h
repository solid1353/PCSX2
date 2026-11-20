// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace NA228UJFrame
{
	// The playing Ultimate Jutsu film, as the overlay shows it. The part is set only for multi-part film ports.
	struct Film
	{
		std::string name;
		std::optional<int16_t> part;
		int32_t frame = 0;
		int16_t skill = 0;
		int16_t attacker = 0;
		int16_t opponent = 0;
	};

	// Retail source games whose films the overlay also reads, by disc CRC. Their films have one part, and only
	// their frame is read.
	static constexpr uint32_t UN3_DISC_CRC = 0xEE3737A4;
	static constexpr uint32_t UN4_DISC_CRC = 0x5C9E1148;

	// Reads the playing film in retail UN3 or UN4, chosen by `disc_crc`, or else in an NA228 build; nothing when no
	// film plays or the game is neither. The span must refer directly to EE RAM and remain valid throughout the
	// call; callers must check VM lifetime before supplying live memory.
	std::optional<Film> Read(std::span<const uint8_t> ram, uint32_t disc_crc = 0);

	// Returns the film's frame, such as "UJ frame 2:118", or an empty string when Read finds no film.
	std::string Format(std::span<const uint8_t> ram, uint32_t disc_crc = 0);

	// The film's position as "part:frame", or "frame" for a single-part film.
	std::string FormatPosition(const Film& film);

	// The text to append to a marks file holding `contents`: the header when it is empty, then one mark line
	// numbered one above the file's highest mark, which is stored in `mark`.
	std::string MarkText(std::string_view contents, const Film& film, std::string_view time, std::string_view build,
		uint32_t* mark);
} // namespace NA228UJFrame
