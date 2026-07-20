#pragma once

#include <cstddef>
#include <cstdint>

namespace cemuextend::bootstrap
{
	inline constexpr std::uint32_t kMagic = 0x434d4231U; // CMB1
	inline constexpr std::uint16_t kVersion = 1;

	struct Header
	{
		std::uint32_t magic{kMagic};
		std::uint16_t version{kVersion};
		std::uint16_t recordSize{24};
		std::uint32_t recordCount{};
	};
	static_assert(sizeof(Header) == 12);

	struct Record
	{
		std::uint32_t moduleHash{};
		std::uint32_t targetVirtualAddress{};
		std::uint32_t expectedInstruction{};
		std::uint32_t expectedMask{0xffffffffU};
		std::uint32_t handlerVirtualAddress{};
		std::uint32_t flags{};
	};
	static_assert(sizeof(Record) == 24);

	template<std::size_t Count>
	struct Table
	{
		Header header{kMagic, kVersion, sizeof(Record), Count};
		Record records[Count];
	};
}

#if defined(__powerpc__) || defined(__PPC__)
#define CEMOD_BOOTSTRAP_HANDLER_ADDRESS(handler) \
	static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&(handler)))
#endif
