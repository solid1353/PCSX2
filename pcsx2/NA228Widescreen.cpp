// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Widescreen.h"
#include "NA228ResourceMonitor.h"

#include <array>

namespace NA228Widescreen
{
	// renderer_refresh_camera's 0x0010DC58..0x0010DC8F in the retail NA2 ELF. Only the widescreen setting rewrites
	// this range, so an NA228 build is widescreen exactly when it differs.
	static constexpr uint32_t REFRESH_CAMERA_ADDRESS = 0x0010dc58;
	static constexpr std::array<uint8_t, 0x38> RETAIL_REFRESH_CAMERA = {
		0x06, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x44, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x00, 0x80, 0x46, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x42, 0x18, 0x02, 0x00,
		0x01, 0x00, 0x42, 0x30, 0x25, 0x18, 0x62, 0x00, 0x00, 0x00, 0x83, 0x44, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x00, 0x80, 0x46, 0x40, 0x08, 0x01, 0x46};

	std::optional<bool> Detect(std::span<const uint8_t> ram)
	{
		if (ram.size() < REFRESH_CAMERA_ADDRESS + RETAIL_REFRESH_CAMERA.size() || !NA228ResourceMonitor::Detect(ram))
			return std::nullopt;

		const volatile uint8_t* bytes = ram.data() + REFRESH_CAMERA_ADDRESS;
		for (size_t i = 0; i < RETAIL_REFRESH_CAMERA.size(); i++)
		{
			if (bytes[i] != RETAIL_REFRESH_CAMERA[i])
				return true;
		}
		return false;
	}
} // namespace NA228Widescreen
