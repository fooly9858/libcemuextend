#pragma once

#include "cemuextend/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cemuextend::transport {

constexpr std::uint16_t kAbiMajor = 2;
// ABI 2.1 adds Java-style host input capabilities without changing any
// existing wire struct size: RawRelative uses MouseV2::flags and Text is a new
// event operation.
constexpr std::uint16_t kAbiMinor = 1;
constexpr std::uint32_t kMaximumMessageSize = 64U * 1024U;
constexpr std::uint32_t kMaximumResponseQueue = 256;
constexpr std::uint32_t kMaximumPageEntries = 128;
constexpr std::uint16_t kOperationVersion = 1;

enum class Query : std::uint32_t { Info = 1 };

enum class Feature : std::uint64_t {
    CopyTransport = 1ULL << 0U,
    Cancellation = 1ULL << 1U,
    Pagination = 1ULL << 2U,
    PermissionRevocation = 1ULL << 3U,
};

constexpr std::uint64_t operator|(Feature left, Feature right) noexcept {
    return static_cast<std::uint64_t>(left) | static_cast<std::uint64_t>(right);
}

struct Info {
    wire::Be16 abiMajor;
    wire::Be16 abiMinor;
    wire::Be32 maximumMessageSize;
    wire::Be32 maximumResponseQueue;
    wire::Be32 maximumPageEntries;
    wire::Be64 hostBuildId;
    wire::Be64 features;
    wire::Be16 coreServiceVersion;
    wire::Be16 reserved16;
    wire::Be32 reserved32;
    std::array<std::byte, 24> reserved{};
};
static_assert(sizeof(Info) == 64);
static_assert(wire::IsWireType<Info>);

struct OpenOptions {
    wire::Be16 abiMajor;
    wire::Be16 abiMinor;
    wire::Be32 flags;
    wire::Be32 maximumPendingRequests;
    wire::Be32 reserved;
};
static_assert(sizeof(OpenOptions) == 16);
static_assert(wire::IsWireType<OpenOptions>);

struct RequestHeader {
    wire::Be32 totalSize;
    wire::Be32 correlationId;
    wire::Be16 serviceId;
    wire::Be16 operation;
    wire::Be16 operationVersion;
    wire::Be16 flags;
};
static_assert(sizeof(RequestHeader) == 16);
static_assert(wire::IsWireType<RequestHeader>);

enum class ResponseFlag : std::uint16_t { Event = 1U << 0U };

struct ResponseHeader {
    wire::Be32 totalSize;
    wire::Be32 correlationId;
    wire::Be16 serviceId;
    wire::Be16 operation;
    wire::Be16 status;
    wire::Be16 flags;
};
static_assert(sizeof(ResponseHeader) == 16);
static_assert(wire::IsWireType<ResponseHeader>);

} // namespace cemuextend::transport
