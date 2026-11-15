// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <span>

namespace NA228Widescreen
{
	// Every NA228 disc has this file; retail NA2 and other games do not.
	static constexpr const char* DISC_MARKER_PATH = "228/228.BIN";

	// Whether this boot ELF of an NA228 disc has the widescreen setting.
	bool Detect(std::span<const uint8_t> elf);
} // namespace NA228Widescreen
