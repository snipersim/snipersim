#pragma once
#include "fixed_types.h"
#include <algorithm>
#include <iterator>

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief One section header of a PMD-level Cuckoo Walk Table entry
	 *        [Skarlatos et al., ASPLOS 2020]: for one 16 MB section of the
	 *        address space, which page sizes it holds and, for the 2 MB
	 *        table, in which way the section's translations sit.
	 */
	struct CwtSectionHeader
	{
		uint8_t has_4kb_page;
		uint8_t has_2mb_page;
		uint8_t way;

		constexpr CwtSectionHeader(uint8_t _has_4kb_page = 0,
		                           uint8_t _has_2mb_page = 0,
		                           uint8_t _way = static_cast<uint8_t>(-1))
			: has_4kb_page(_has_4kb_page),
			  has_2mb_page(_has_2mb_page),
			  way(_way) {}

		friend constexpr bool operator==(const CwtSectionHeader &a, const CwtSectionHeader &b) noexcept
		{
			return a.has_4kb_page == b.has_4kb_page && a.has_2mb_page == b.has_2mb_page && a.way == b.way;
		}

		friend constexpr bool operator!=(const CwtSectionHeader &a, const CwtSectionHeader &b) noexcept
		{
			return !(a == b);
		}
	};

	static constexpr CwtSectionHeader EMPTY_CWT_SECTION_HEADER = CwtSectionHeader();

	/**
	 * @brief A PMD-CWT entry: 64 section headers covering 1 GB (address >> 30
	 *        is the tag, bits 29..24 select the section).
	 */
	struct PmdCwtEntry
	{
		IntPtr tag;
		CwtSectionHeader section_header[64];

		PmdCwtEntry() : tag(static_cast<IntPtr>(-1))
		{
			for (int i = 0; i < 64; ++i)
				section_header[i] = CwtSectionHeader();
		}

		explicit PmdCwtEntry(IntPtr _tag) : tag(_tag)
		{
			for (int i = 0; i < 64; ++i)
				section_header[i] = CwtSectionHeader();
		}

		friend bool operator==(const PmdCwtEntry &lhs, const PmdCwtEntry &rhs) noexcept
		{
			if (lhs.tag != rhs.tag)
				return false;
			return std::equal(std::begin(lhs.section_header), std::end(lhs.section_header), std::begin(rhs.section_header));
		}

		friend bool operator!=(const PmdCwtEntry &lhs, const PmdCwtEntry &rhs) noexcept
		{
			return !(lhs == rhs);
		}
	};
}
