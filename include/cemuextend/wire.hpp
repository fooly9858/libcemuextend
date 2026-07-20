#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace cemuextend::wire {

constexpr std::uint16_t ByteSwap16(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
}

constexpr std::uint32_t ByteSwap32(std::uint32_t value) noexcept {
    return ((value & 0x000000ffU) << 24U) | ((value & 0x0000ff00U) << 8U) |
           ((value & 0x00ff0000U) >> 8U) | ((value & 0xff000000U) >> 24U);
}

constexpr std::uint16_t ToBigEndian(std::uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return ByteSwap16(value);
    return value;
}

constexpr std::uint32_t ToBigEndian(std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return ByteSwap32(value);
    return value;
}

constexpr std::uint16_t FromBigEndian(std::uint16_t value) noexcept {
    return ToBigEndian(value);
}

constexpr std::uint32_t FromBigEndian(std::uint32_t value) noexcept {
    return ToBigEndian(value);
}

struct Be16 {
    std::uint16_t storage{};
    constexpr Be16() = default;
    constexpr Be16(std::uint16_t value) : storage(ToBigEndian(value)) {}
    constexpr Be16& operator=(std::uint16_t value) noexcept {
        storage = ToBigEndian(value);
        return *this;
    }
    [[nodiscard]] constexpr std::uint16_t get() const noexcept {
        return FromBigEndian(storage);
    }
};

struct Be32 {
    std::uint32_t storage{};
    constexpr Be32() = default;
    constexpr Be32(std::uint32_t value) : storage(ToBigEndian(value)) {}
    constexpr Be32& operator=(std::uint32_t value) noexcept {
        storage = ToBigEndian(value);
        return *this;
    }
    [[nodiscard]] constexpr std::uint32_t get() const noexcept {
        return FromBigEndian(storage);
    }
};

struct Be64 {
    Be32 high{};
    Be32 low{};
    constexpr Be64() = default;
    constexpr Be64(std::uint64_t value)
        : high(static_cast<std::uint32_t>(value >> 32U)),
          low(static_cast<std::uint32_t>(value)) {}
    constexpr Be64& operator=(std::uint64_t value) noexcept {
        high = static_cast<std::uint32_t>(value >> 32U);
        low = static_cast<std::uint32_t>(value);
        return *this;
    }
    [[nodiscard]] constexpr std::uint64_t get() const noexcept {
        return (static_cast<std::uint64_t>(high.get()) << 32U) | low.get();
    }
};

struct BeI32 {
    Be32 value{};
    constexpr BeI32() = default;
    constexpr BeI32(std::int32_t input) : value(std::bit_cast<std::uint32_t>(input)) {}
    constexpr BeI32& operator=(std::int32_t input) noexcept {
        value = std::bit_cast<std::uint32_t>(input);
        return *this;
    }
    [[nodiscard]] constexpr std::int32_t get() const noexcept {
        return std::bit_cast<std::int32_t>(value.get());
    }
};

template<typename T>
inline constexpr bool IsWireType = std::is_standard_layout_v<T> &&
    std::is_trivially_copyable_v<T>;

static_assert(sizeof(Be16) == 2);
static_assert(sizeof(Be32) == 4);
static_assert(sizeof(Be64) == 8);
static_assert(sizeof(BeI32) == 4);
static_assert(IsWireType<Be16> && IsWireType<Be32> && IsWireType<Be64> && IsWireType<BeI32>);

enum class Error : std::int32_t {
    Ok = 0,
    InvalidArgument = -1,
    Unavailable = -2,
    AbiMismatch = -3,
    PermissionDenied = -5,
    NotSupported = -6,
    Busy = -7,
    NotFound = -8,
    TooLarge = -9,
    ProtocolError = -10,
    Disconnected = -11,
    IoError = -12,
    TimedOut = -13,
};

enum class ServiceId : std::uint16_t {
    Core = 1,
    Input = 2,
    Logging = 3,
    Configuration = 4,
    File = 5,
    Clipboard = 6,
    Window = 7,
    Capture = 8,
    Diagnostics = 9,
};

} // namespace cemuextend::wire
