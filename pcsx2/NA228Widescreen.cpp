// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Widescreen.h"

#include <algorithm>
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

	static uint32_t Read(std::span<const uint8_t> elf, size_t offset, size_t size)
	{
		uint32_t value = 0;
		for (size_t i = 0; i < size; i++)
			value |= static_cast<uint32_t>(elf[offset + i]) << (i * 8);
		return value;
	}

	bool Detect(std::span<const uint8_t> elf)
	{
		static constexpr size_t ELF_HEADER_SIZE = 0x34;
		static constexpr size_t PROGRAM_HEADER_SIZE = 0x20;
		if (elf.size() < ELF_HEADER_SIZE)
			return false;

		const size_t program_headers = Read(elf, 0x1c, 4);
		const size_t program_header_count = Read(elf, 0x2c, 2);
		for (size_t i = 0; i < program_header_count; i++)
		{
			const size_t header = program_headers + i * PROGRAM_HEADER_SIZE;
			if (header > elf.size() || elf.size() - header < PROGRAM_HEADER_SIZE)
				return false;

			const uint32_t offset = Read(elf, header + 0x04, 4);
			const uint32_t address = Read(elf, header + 0x08, 4);
			const uint32_t file_size = Read(elf, header + 0x10, 4);
			if (REFRESH_CAMERA_ADDRESS < address ||
				REFRESH_CAMERA_ADDRESS - address + RETAIL_REFRESH_CAMERA.size() > file_size)
				continue;

			const size_t start = static_cast<size_t>(offset) + (REFRESH_CAMERA_ADDRESS - address);
			if (start > elf.size() || elf.size() - start < RETAIL_REFRESH_CAMERA.size())
				return false;

			return !std::equal(RETAIL_REFRESH_CAMERA.begin(), RETAIL_REFRESH_CAMERA.end(), elf.begin() + start);
		}
		return false;
	}
} // namespace NA228Widescreen
