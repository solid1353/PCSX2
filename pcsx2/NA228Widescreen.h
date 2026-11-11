// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace NA228Widescreen
{
	// Whether the running NA228 build has the widescreen setting, known once its 228.BIN is loaded; nothing before
	// that or in other games. The span must refer directly to EE RAM and remain valid throughout the call.
	std::optional<bool> Detect(std::span<const uint8_t> ram);
} // namespace NA228Widescreen
