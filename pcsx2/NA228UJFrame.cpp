// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228UJFrame.h"
#include "NA228ResourceMonitor.h"

#include "fmt/format.h"

#include <algorithm>

namespace NA228UJFrame
{
	static constexpr uint32_t PHYSICAL_MASK = 0x1fffffff;

	// NA2.28 resident addresses and structure offsets.
	static constexpr uint32_t JUTSU_SKILL_PLAY_OWNER = 0x00607714;
	static constexpr uint32_t JUTSU_SKILL_REQUESTS = 0x00607748;
	static constexpr int64_t OWNER_CONTAINER = 0x04;
	static constexpr int64_t OWNER_SKILL = 0x3c;
	static constexpr int64_t OWNER_REQUEST_ORDINAL = 0x16a;
	static constexpr int64_t CONTAINER_FRAME = 0x90;
	static constexpr int64_t REQUEST_SIZE = 0x18;
	static constexpr int64_t REQUEST_STREAM_COUNT = 0x09;

	// Reads a little-endian value at base + offset. Fails when base is null or outside EE RAM, or the value
	// does not lie entirely inside it.
	static bool Read(std::span<const uint8_t> ram, uint32_t base, int64_t offset, uint32_t size, uint32_t& value)
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

	std::string Format(std::span<const uint8_t> ram)
	{
		ram = ram.first(std::min<size_t>(ram.size(), NA228ResourceMonitor::EE_RAM_SIZE));
		if (!NA228ResourceMonitor::Detect(ram))
			return {};

		uint32_t owner, part, container, frame, skill, requests;
		if (!Read(ram, JUTSU_SKILL_PLAY_OWNER, 0, 4, owner) ||
			!Read(ram, owner, OWNER_REQUEST_ORDINAL, 2, part) || static_cast<int16_t>(part) <= 0 ||
			!Read(ram, owner, OWNER_CONTAINER, 4, container) ||
			!Read(ram, container, CONTAINER_FRAME, 4, frame) ||
			!Read(ram, owner, OWNER_SKILL, 2, skill) ||
			!Read(ram, JUTSU_SKILL_REQUESTS, 0, 4, requests))
		{
			return {};
		}

		// Retail request rows hold at most one stream; only multi-part film ports show the part.
		uint32_t streams = 0;
		if (requests != 0 &&
			!Read(ram, requests, static_cast<int16_t>(skill) * REQUEST_SIZE + REQUEST_STREAM_COUNT, 1, streams))
		{
			return {};
		}

		const int32_t film_frame = static_cast<int32_t>(frame);
		return streams > 1 ? fmt::format("UJ frame {}:{}", static_cast<int16_t>(part), film_frame) :
		                     fmt::format("UJ frame {}", film_frame);
	}
} // namespace NA228UJFrame
