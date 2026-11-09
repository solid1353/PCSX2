// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace NA228UJFrame
{
	// Returns the playing Ultimate Jutsu film's frame, such as "UJ frame 2:118", or an empty string when
	// no film plays or the game is not an NA228 build. The span must refer directly to EE RAM and remain
	// valid throughout the call; callers must check VM lifetime before supplying live memory.
	std::string Format(std::span<const uint8_t> ram);
} // namespace NA228UJFrame
