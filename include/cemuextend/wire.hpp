#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace cemuextend::wire {

constexpr std::uint32_t kMagic = 0x43455854U; // CEXT
constexpr std::uint32_t kBulkMagic = 0x4345424bU; // CEBK
constexpr std::uint16_t kAbiMajor = 1;
constexpr std::uint16_t kAbiMinor = 0;
constexpr std::uint32_t kDefaultRegionSize = 256U * 1024U;
constexpr std::uint32_t kMaximumRegionSize = 1024U * 1024U;
constexpr std::uint32_t kServiceDirectorySize = 2U * 1024U;
constexpr std::uint32_t kStatePageSize = 16U * 1024U;
constexpr std::uint32_t kControlRingCapacity = 16U * 1024U;
constexpr std::uint32_t kEventRingCapacity = 8U * 1024U;
constexpr std::uint32_t kBulkPayloadSize = 64U * 1024U;
constexpr std::uint32_t kBulkBlockCount = 2;
constexpr std::uint32_t kAlignment = 64;
constexpr std::uint32_t kRecordAlignment = 8;
constexpr std::uint32_t kHeartbeatTimeoutMs = 5000;

constexpr std::uint16_t ByteSwap16(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
}

constexpr std::uint32_t ByteSwap32(std::uint32_t value) noexcept {
    return ((value & 0x000000ffU) << 24U) | ((value & 0x0000ff00U) << 8U) |
           ((value & 0x00ff0000U) >> 8U) | ((value & 0xff000000U) >> 24U);
}

constexpr std::uint32_t ToBigEndian(std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return ByteSwap32(value);
    return value;
}

constexpr std::uint16_t ToBigEndian(std::uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return ByteSwap16(value);
    return value;
}

constexpr std::uint32_t FromBigEndian(std::uint32_t value) noexcept { return ToBigEndian(value); }
constexpr std::uint16_t FromBigEndian(std::uint16_t value) noexcept { return ToBigEndian(value); }

struct Be16 {
    std::uint16_t storage{};
    constexpr Be16() = default;
    constexpr Be16(std::uint16_t value) : storage(ToBigEndian(value)) {}
    constexpr Be16& operator=(std::uint16_t value) noexcept {
        storage = ToBigEndian(value);
        return *this;
    }
    [[nodiscard]] constexpr std::uint16_t get() const noexcept { return FromBigEndian(storage); }
};

struct Be32 {
    std::uint32_t storage{};
    constexpr Be32() = default;
    constexpr Be32(std::uint32_t value) : storage(ToBigEndian(value)) {}
    constexpr Be32& operator=(std::uint32_t value) noexcept {
        storage = ToBigEndian(value);
        return *this;
    }
    [[nodiscard]] constexpr std::uint32_t get() const noexcept { return FromBigEndian(storage); }
};

struct Be64 {
    Be32 high{};
    Be32 low{};
    constexpr Be64() = default;
    constexpr Be64(std::uint64_t value)
        : high(static_cast<std::uint32_t>(value >> 32U)), low(static_cast<std::uint32_t>(value)) {}
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

static_assert(sizeof(Be16) == 2);
static_assert(sizeof(Be32) == 4);
static_assert(sizeof(Be64) == 8);
static_assert(sizeof(BeI32) == 4);

[[nodiscard]] inline std::uint32_t AtomicLoad(const Be32& value,
                                              std::memory_order order = std::memory_order_acquire) noexcept {
    auto& mutableStorage = const_cast<std::uint32_t&>(value.storage);
    return FromBigEndian(std::atomic_ref<std::uint32_t>(mutableStorage).load(order));
}

inline void AtomicStore(Be32& value, std::uint32_t input,
                        std::memory_order order = std::memory_order_release) noexcept {
    std::atomic_ref<std::uint32_t>(value.storage).store(ToBigEndian(input), order);
}

inline std::uint32_t AtomicFetchAdd(Be32& value, std::uint32_t amount,
                                    std::memory_order order = std::memory_order_acq_rel) noexcept {
    auto reference = std::atomic_ref<std::uint32_t>(value.storage);
    auto currentRaw = reference.load(std::memory_order_relaxed);
    for (;;) {
        const auto current = FromBigEndian(currentRaw);
        const auto desiredRaw = ToBigEndian(current + amount);
        if (reference.compare_exchange_weak(currentRaw, desiredRaw, order, std::memory_order_relaxed))
            return current;
    }
}

enum class ConnectionState : std::uint32_t {
    Empty = 0,
    Registering = 1,
    Connected = 2,
    Closing = 3,
    Failed = 4,
};

enum class Error : std::int32_t {
    Ok = 0,
    InvalidArgument = -1,
    Unavailable = -2,
    AbiMismatch = -3,
    InvalidLayout = -4,
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

enum class NotifyFlag : std::uint32_t {
    Control = 1U << 0U,
    Event = 1U << 1U,
    State = 1U << 2U,
    Bulk = 1U << 3U,
    Closing = 1U << 4U,
};

enum class Query : std::uint32_t {
    BridgeInfo = 1,
};

struct BridgeInfo {
    Be32 available;
    Be16 minimumAbiMajor;
    Be16 minimumAbiMinor;
    Be16 maximumAbiMajor;
    Be16 maximumAbiMinor;
    Be32 maximumRegionSize;
    Be64 hostBuildId;
    Be64 features;
    Be32 maximumServices;
    Be32 reservedValue;
    std::array<std::byte, 24> reserved{};
};

static_assert(sizeof(BridgeInfo) == 64);

constexpr NotifyFlag operator|(NotifyFlag left, NotifyFlag right) noexcept {
    return static_cast<NotifyFlag>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

enum class Feature : std::uint64_t {
    SharedMemory = 1ULL << 0U,
    ServiceDirectory = 1ULL << 1U,
    StatePages = 1ULL << 2U,
    Bulk = 1ULL << 3U,
    Permissions = 1ULL << 4U,
    RawInput = 1ULL << 5U,
    ObservedVpad = 1ULL << 6U,
};

constexpr std::uint64_t operator|(Feature left, Feature right) noexcept {
    return static_cast<std::uint64_t>(left) | static_cast<std::uint64_t>(right);
}

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
    GameState = 0x100,
    CustomBase = 0x8000,
};

enum class ServiceDirection : std::uint8_t {
    HostProvides = 1,
    GuestProvides = 2,
};

enum class ServiceFeature : std::uint8_t {
    Requests = 1U << 0U,
    Events = 1U << 1U,
    State = 1U << 2U,
};

enum class Permission : std::uint32_t {
    None = 0,
    Read = 1U << 0U,
    Write = 1U << 1U,
    Inject = 1U << 2U,
};

constexpr Permission operator|(Permission left, Permission right) noexcept {
    return static_cast<Permission>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

enum class MessageKind : std::uint8_t {
    Padding = 0,
    Request = 1,
    Response = 2,
    Event = 3,
};

enum class MessageFlag : std::uint8_t {
    None = 0,
    HasBulk = 1U << 0U,
    More = 1U << 1U,
};

struct alignas(64) BridgeHeader {
    Be32 magic;
    Be16 abiMajor;
    Be16 abiMinor;
    Be32 headerSize;
    Be32 regionSize;
    Be64 hostBuildId;
    Be32 sessionId;
    Be32 generation;
    Be32 serviceDirectoryOffset;
    Be32 serviceDirectorySize;
    Be32 hostStateOffset;
    Be32 hostStateSize;
    Be32 guestStateOffset;
    Be32 guestStateSize;
    Be32 guestToHostControlOffset;
    Be32 guestToHostControlSize;
    Be32 hostToGuestControlOffset;
    Be32 hostToGuestControlSize;
    Be32 guestToHostEventOffset;
    Be32 guestToHostEventSize;
    Be32 hostToGuestEventOffset;
    Be32 hostToGuestEventSize;
    Be32 bulkOffset;
    Be32 bulkSize;
    Be32 hostHeartbeat;
    Be32 guestHeartbeat;
    Be32 connectionState;
    BeI32 lastError;
    std::array<std::byte, 144> reserved{};
};

struct MessageHeader {
    Be32 recordSize;
    Be32 payloadSize;
    Be16 serviceId;
    Be16 operation;
    std::uint8_t kind{};
    std::uint8_t flags{};
    Be16 status;
    Be32 correlationId;
    Be64 timestampNs;
    Be32 sequence;
};

struct alignas(64) RingHeader {
    Be32 writePosition;
    Be32 readPosition;
    Be32 droppedRecords;
    Be32 protocolErrors;
    Be32 capacity;
    Be32 generation;
    std::array<std::byte, 40> reserved{};
};

struct ServiceDirectoryHeader {
    Be32 generation;
    Be16 hostServiceCount;
    Be16 guestServiceCount;
    Be16 capacity;
    Be16 descriptorSize;
    Be32 descriptorsOffset;
    std::array<std::byte, 16> reserved{};
};

struct ServiceDescriptor {
    Be16 serviceId;
    Be16 major;
    Be16 minor;
    std::uint8_t direction{};
    std::uint8_t features{};
    Be32 requiredPermissions;
    Be32 grantedPermissions;
    Be32 nameHash;
    Be32 reserved;
};

struct StatePageHeader {
    Be32 sequence;
    Be32 generation;
    Be32 pageSize;
    Be16 entryCount;
    Be16 entryCapacity;
    Be32 entriesOffset;
    Be32 dataOffset;
    std::array<std::byte, 8> reserved{};
};

struct StateEntry {
    Be16 serviceId;
    Be16 stateId;
    Be32 version;
    Be32 offset;
    Be32 size;
};

enum class BulkOwner : std::uint32_t {
    Free = 0,
    Guest = 1,
    Host = 2,
};

enum class BulkState : std::uint32_t {
    Free = 0,
    Writing = 1,
    Ready = 2,
    Reading = 3,
};

struct alignas(64) BulkAreaHeader {
    Be32 magic;
    Be16 major;
    Be16 minor;
    Be32 blockCount;
    Be32 payloadSize;
    Be32 generation;
    Be32 blockStride;
    std::array<std::byte, 40> reserved{};
};

struct alignas(64) BulkBlockHeader {
    Be32 owner;
    Be32 state;
    Be32 generation;
    Be32 size;
    Be32 payloadOffset;
    Be32 reservedValue;
    std::array<std::byte, 40> reserved{};
};

struct BulkHandle {
    Be32 blockIndex;
    Be32 generation;
    Be32 offset;
    Be32 size;
};

static_assert(sizeof(BridgeHeader) == 256);
static_assert(offsetof(BridgeHeader, serviceDirectoryOffset) == 32);
static_assert(offsetof(BridgeHeader, bulkOffset) == 88);
static_assert(offsetof(BridgeHeader, hostHeartbeat) == 96);
static_assert(offsetof(BridgeHeader, reserved) == 112);
static_assert(sizeof(MessageHeader) == 32);
static_assert(offsetof(MessageHeader, correlationId) == 16);
static_assert(offsetof(MessageHeader, timestampNs) == 20);
static_assert(sizeof(RingHeader) == 64);
static_assert(sizeof(ServiceDirectoryHeader) == 32);
static_assert(sizeof(ServiceDescriptor) == 24);
static_assert(sizeof(StatePageHeader) == 32);
static_assert(sizeof(StateEntry) == 16);
static_assert(sizeof(BulkAreaHeader) == 64);
static_assert(sizeof(BulkBlockHeader) == 64);
static_assert(sizeof(BulkHandle) == 16);

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

struct RegionSlice {
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct DefaultLayout {
    RegionSlice header;
    RegionSlice serviceDirectory;
    RegionSlice hostState;
    RegionSlice guestState;
    RegionSlice guestToHostControl;
    RegionSlice hostToGuestControl;
    RegionSlice guestToHostEvent;
    RegionSlice hostToGuestEvent;
    RegionSlice bulk;
    std::uint32_t regionSize{};
};

[[nodiscard]] constexpr DefaultLayout BuildDefaultLayout() noexcept {
    DefaultLayout result{};
    std::uint32_t cursor = 0;
    auto take = [&cursor](std::uint32_t size) constexpr {
        cursor = AlignUp(cursor, kAlignment);
        const RegionSlice slice{cursor, size};
        cursor += size;
        return slice;
    };
    result.header = take(sizeof(BridgeHeader));
    result.serviceDirectory = take(kServiceDirectorySize);
    result.hostState = take(kStatePageSize);
    result.guestState = take(kStatePageSize);
    result.guestToHostControl = take(sizeof(RingHeader) + kControlRingCapacity);
    result.hostToGuestControl = take(sizeof(RingHeader) + kControlRingCapacity);
    result.guestToHostEvent = take(sizeof(RingHeader) + kEventRingCapacity);
    result.hostToGuestEvent = take(sizeof(RingHeader) + kEventRingCapacity);
    result.bulk = take(sizeof(BulkAreaHeader) +
                       kBulkBlockCount * (sizeof(BulkBlockHeader) + kBulkPayloadSize));
    result.regionSize = kDefaultRegionSize;
    return result;
}

static_assert(BuildDefaultLayout().bulk.offset + BuildDefaultLayout().bulk.size <= kDefaultRegionSize);

enum class LayoutError {
    None,
    TooSmall,
    TooLarge,
    BadMagic,
    AbiMismatch,
    BadHeaderSize,
    BadRegionSize,
    Unaligned,
    OutOfRange,
    Overlap,
    BadRing,
    BadStatePage,
    BadServiceDirectory,
    BadBulk,
};

struct LayoutValidation {
    LayoutError error{LayoutError::None};
    std::string_view field{};
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return error == LayoutError::None; }
};

inline void InitializeRing(std::span<std::byte> region, RegionSlice slice, std::uint32_t capacity) {
    auto* ring = reinterpret_cast<RingHeader*>(region.data() + slice.offset);
    std::memset(ring, 0, sizeof(*ring));
    ring->capacity = capacity;
    ring->generation = 1;
}

inline void InitializeStatePage(std::span<std::byte> region, RegionSlice slice) {
    auto* page = reinterpret_cast<StatePageHeader*>(region.data() + slice.offset);
    std::memset(page, 0, slice.size);
    page->generation = 1;
    page->pageSize = slice.size;
    page->entryCapacity = static_cast<std::uint16_t>(
        (slice.size - sizeof(StatePageHeader)) / (sizeof(StateEntry) + 16U));
    page->entriesOffset = sizeof(StatePageHeader);
    page->dataOffset = AlignUp(sizeof(StatePageHeader) +
                                   page->entryCapacity.get() * sizeof(StateEntry),
                               kRecordAlignment);
}

inline Error InitializeDefaultRegion(std::span<std::byte> region,
                                     std::span<const ServiceDescriptor> guestServices = {}) {
    const auto layout = BuildDefaultLayout();
    if (region.size() < layout.regionSize || region.size() > kMaximumRegionSize)
        return Error::InvalidArgument;
    std::memset(region.data(), 0, region.size());
    auto* header = reinterpret_cast<BridgeHeader*>(region.data());
    header->magic = kMagic;
    header->abiMajor = kAbiMajor;
    header->abiMinor = kAbiMinor;
    header->headerSize = sizeof(BridgeHeader);
    header->regionSize = static_cast<std::uint32_t>(region.size());
    header->generation = 1;
    header->serviceDirectoryOffset = layout.serviceDirectory.offset;
    header->serviceDirectorySize = layout.serviceDirectory.size;
    header->hostStateOffset = layout.hostState.offset;
    header->hostStateSize = layout.hostState.size;
    header->guestStateOffset = layout.guestState.offset;
    header->guestStateSize = layout.guestState.size;
    header->guestToHostControlOffset = layout.guestToHostControl.offset;
    header->guestToHostControlSize = layout.guestToHostControl.size;
    header->hostToGuestControlOffset = layout.hostToGuestControl.offset;
    header->hostToGuestControlSize = layout.hostToGuestControl.size;
    header->guestToHostEventOffset = layout.guestToHostEvent.offset;
    header->guestToHostEventSize = layout.guestToHostEvent.size;
    header->hostToGuestEventOffset = layout.hostToGuestEvent.offset;
    header->hostToGuestEventSize = layout.hostToGuestEvent.size;
    header->bulkOffset = layout.bulk.offset;
    header->bulkSize = layout.bulk.size;
    header->connectionState = static_cast<std::uint32_t>(ConnectionState::Registering);

    auto* directory = reinterpret_cast<ServiceDirectoryHeader*>(region.data() + layout.serviceDirectory.offset);
    directory->generation = 1;
    directory->guestServiceCount = static_cast<std::uint16_t>(guestServices.size());
    directory->capacity = static_cast<std::uint16_t>(
        (layout.serviceDirectory.size - sizeof(ServiceDirectoryHeader)) / sizeof(ServiceDescriptor));
    directory->descriptorSize = sizeof(ServiceDescriptor);
    directory->descriptorsOffset = sizeof(ServiceDirectoryHeader);
    if (guestServices.size() > directory->capacity.get())
        return Error::TooLarge;
    std::memcpy(reinterpret_cast<std::byte*>(directory) + sizeof(ServiceDirectoryHeader),
                guestServices.data(), guestServices.size_bytes());

    InitializeStatePage(region, layout.hostState);
    InitializeStatePage(region, layout.guestState);
    InitializeRing(region, layout.guestToHostControl, kControlRingCapacity);
    InitializeRing(region, layout.hostToGuestControl, kControlRingCapacity);
    InitializeRing(region, layout.guestToHostEvent, kEventRingCapacity);
    InitializeRing(region, layout.hostToGuestEvent, kEventRingCapacity);

    auto* bulk = reinterpret_cast<BulkAreaHeader*>(region.data() + layout.bulk.offset);
    bulk->magic = kBulkMagic;
    bulk->major = kAbiMajor;
    bulk->minor = kAbiMinor;
    bulk->blockCount = kBulkBlockCount;
    bulk->payloadSize = kBulkPayloadSize;
    bulk->generation = 1;
    bulk->blockStride = sizeof(BulkBlockHeader) + kBulkPayloadSize;
    for (std::uint32_t index = 0; index < kBulkBlockCount; ++index) {
        const auto blockOffset = sizeof(BulkAreaHeader) + index * bulk->blockStride.get();
        auto* block = reinterpret_cast<BulkBlockHeader*>(reinterpret_cast<std::byte*>(bulk) + blockOffset);
        block->owner = static_cast<std::uint32_t>(BulkOwner::Free);
        block->state = static_cast<std::uint32_t>(BulkState::Free);
        block->generation = 1;
        block->payloadOffset = blockOffset + sizeof(BulkBlockHeader);
    }
    return Error::Ok;
}

[[nodiscard]] inline LayoutValidation ValidateLayout(std::span<const std::byte> region) {
    if (region.size() < sizeof(BridgeHeader))
        return {LayoutError::TooSmall, "region"};
    if (region.size() > kMaximumRegionSize)
        return {LayoutError::TooLarge, "region"};
    const auto& header = *reinterpret_cast<const BridgeHeader*>(region.data());
    if (header.magic.get() != kMagic)
        return {LayoutError::BadMagic, "magic"};
    if (header.abiMajor.get() != kAbiMajor)
        return {LayoutError::AbiMismatch, "abiMajor"};
    if (header.headerSize.get() != sizeof(BridgeHeader))
        return {LayoutError::BadHeaderSize, "headerSize"};
    if (header.regionSize.get() != region.size() || header.regionSize.get() > kMaximumRegionSize)
        return {LayoutError::BadRegionSize, "regionSize"};

    struct NamedSlice { RegionSlice slice; std::string_view name; };
    const std::array slices{
        NamedSlice{{0, header.headerSize.get()}, "header"},
        NamedSlice{{header.serviceDirectoryOffset.get(), header.serviceDirectorySize.get()}, "serviceDirectory"},
        NamedSlice{{header.hostStateOffset.get(), header.hostStateSize.get()}, "hostState"},
        NamedSlice{{header.guestStateOffset.get(), header.guestStateSize.get()}, "guestState"},
        NamedSlice{{header.guestToHostControlOffset.get(), header.guestToHostControlSize.get()}, "guestToHostControl"},
        NamedSlice{{header.hostToGuestControlOffset.get(), header.hostToGuestControlSize.get()}, "hostToGuestControl"},
        NamedSlice{{header.guestToHostEventOffset.get(), header.guestToHostEventSize.get()}, "guestToHostEvent"},
        NamedSlice{{header.hostToGuestEventOffset.get(), header.hostToGuestEventSize.get()}, "hostToGuestEvent"},
        NamedSlice{{header.bulkOffset.get(), header.bulkSize.get()}, "bulk"},
    };
    for (const auto& named : slices) {
        if (named.slice.offset % kAlignment != 0)
            return {LayoutError::Unaligned, named.name};
        if (named.slice.size == 0 || named.slice.offset > region.size() ||
            named.slice.size > region.size() - named.slice.offset)
            return {LayoutError::OutOfRange, named.name};
    }
    for (std::size_t left = 0; left < slices.size(); ++left) {
        for (std::size_t right = left + 1; right < slices.size(); ++right) {
            const auto leftEnd = static_cast<std::uint64_t>(slices[left].slice.offset) + slices[left].slice.size;
            const auto rightEnd = static_cast<std::uint64_t>(slices[right].slice.offset) + slices[right].slice.size;
            if (slices[left].slice.offset < rightEnd && slices[right].slice.offset < leftEnd)
                return {LayoutError::Overlap, slices[right].name};
        }
    }

    const auto validateRing = [&](RegionSlice slice, std::string_view name) -> LayoutValidation {
        if (slice.size < sizeof(RingHeader) + kRecordAlignment)
            return {LayoutError::BadRing, name};
        const auto& ring = *reinterpret_cast<const RingHeader*>(region.data() + slice.offset);
        const auto capacity = ring.capacity.get();
        if (capacity != slice.size - sizeof(RingHeader) || capacity % kRecordAlignment != 0 ||
            capacity > std::numeric_limits<std::int32_t>::max())
            return {LayoutError::BadRing, name};
        const auto used = AtomicLoad(ring.writePosition) - AtomicLoad(ring.readPosition);
        if (used > capacity)
            return {LayoutError::BadRing, name};
        return {};
    };
    for (const auto [offset, size, name] : std::array{
             std::tuple{header.guestToHostControlOffset.get(), header.guestToHostControlSize.get(), std::string_view{"guestToHostControl"}},
             std::tuple{header.hostToGuestControlOffset.get(), header.hostToGuestControlSize.get(), std::string_view{"hostToGuestControl"}},
             std::tuple{header.guestToHostEventOffset.get(), header.guestToHostEventSize.get(), std::string_view{"guestToHostEvent"}},
             std::tuple{header.hostToGuestEventOffset.get(), header.hostToGuestEventSize.get(), std::string_view{"hostToGuestEvent"}},
         }) {
        if (const auto validation = validateRing({offset, size}, name); !validation)
            return validation;
    }

    const auto validateState = [&](std::uint32_t offset, std::uint32_t size,
                                   std::string_view name) -> LayoutValidation {
        if (size < sizeof(StatePageHeader))
            return {LayoutError::BadStatePage, name};
        const auto& page = *reinterpret_cast<const StatePageHeader*>(region.data() + offset);
        if (page.pageSize.get() != size || page.entriesOffset.get() < sizeof(StatePageHeader) ||
            page.dataOffset.get() > size || page.entryCount.get() > page.entryCapacity.get())
            return {LayoutError::BadStatePage, name};
        const auto directoryBytes = static_cast<std::uint64_t>(page.entryCapacity.get()) * sizeof(StateEntry);
        if (page.entriesOffset.get() + directoryBytes > page.dataOffset.get())
            return {LayoutError::BadStatePage, name};
        return {};
    };
    if (auto validation = validateState(header.hostStateOffset.get(), header.hostStateSize.get(), "hostState"); !validation)
        return validation;
    if (auto validation = validateState(header.guestStateOffset.get(), header.guestStateSize.get(), "guestState"); !validation)
        return validation;

    const auto& directory = *reinterpret_cast<const ServiceDirectoryHeader*>(
        region.data() + header.serviceDirectoryOffset.get());
    if (directory.descriptorSize.get() != sizeof(ServiceDescriptor) ||
        directory.descriptorsOffset.get() < sizeof(ServiceDirectoryHeader) ||
        static_cast<std::uint32_t>(directory.hostServiceCount.get() + directory.guestServiceCount.get()) >
            directory.capacity.get() ||
        directory.descriptorsOffset.get() +
                static_cast<std::uint64_t>(directory.capacity.get()) * sizeof(ServiceDescriptor) >
            header.serviceDirectorySize.get())
        return {LayoutError::BadServiceDirectory, "serviceDirectory"};

    const auto& bulk = *reinterpret_cast<const BulkAreaHeader*>(region.data() + header.bulkOffset.get());
    if (bulk.magic.get() != kBulkMagic || bulk.major.get() != kAbiMajor ||
        bulk.blockCount.get() != kBulkBlockCount || bulk.payloadSize.get() != kBulkPayloadSize ||
        bulk.blockStride.get() != sizeof(BulkBlockHeader) + kBulkPayloadSize ||
        sizeof(BulkAreaHeader) + static_cast<std::uint64_t>(bulk.blockCount.get()) * bulk.blockStride.get() >
            header.bulkSize.get())
        return {LayoutError::BadBulk, "bulk"};
    for (std::uint32_t index = 0; index < bulk.blockCount.get(); ++index) {
        const auto blockOffset = sizeof(BulkAreaHeader) + index * bulk.blockStride.get();
        const auto& block = *reinterpret_cast<const BulkBlockHeader*>(
            reinterpret_cast<const std::byte*>(&bulk) + blockOffset);
        if (block.payloadOffset.get() != blockOffset + sizeof(BulkBlockHeader) ||
            block.size.get() > bulk.payloadSize.get())
            return {LayoutError::BadBulk, "bulkBlock"};
    }
    return {};
}

struct MessageView {
    MessageHeader header{};
    std::span<const std::byte> payload{};
};

enum class RingReadResult {
    Empty,
    Message,
    ProtocolError,
};

class RingView {
public:
    RingView() = default;
    RingView(std::byte* base, std::size_t areaSize)
        : header_(reinterpret_cast<RingHeader*>(base)), data_(base + sizeof(RingHeader)), areaSize_(areaSize) {}

    [[nodiscard]] bool valid() const noexcept {
        return header_ && areaSize_ >= sizeof(RingHeader) &&
               header_->capacity.get() == areaSize_ - sizeof(RingHeader);
    }

    [[nodiscard]] std::uint32_t used() const noexcept {
        return AtomicLoad(header_->writePosition) - AtomicLoad(header_->readPosition);
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return header_->capacity.get(); }

    bool Push(MessageHeader message, std::span<const std::byte> payload, bool dropIfFull = false) {
        if (!valid() || payload.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        const auto recordSize = AlignUp(static_cast<std::uint32_t>(sizeof(MessageHeader) + payload.size()),
                                        kRecordAlignment);
        const auto capacityValue = capacity();
        if (recordSize > capacityValue)
            return false;
        const auto write = AtomicLoad(header_->writePosition, std::memory_order_relaxed);
        const auto read = AtomicLoad(header_->readPosition);
        const auto occupied = write - read;
        if (occupied > capacityValue) {
            AtomicFetchAdd(header_->protocolErrors, 1);
            return false;
        }
        const auto position = write % capacityValue;
        const auto remaining = capacityValue - position;
        const auto wrapBytes = remaining < recordSize ? remaining : 0U;
        if (recordSize + wrapBytes > capacityValue - occupied) {
            if (dropIfFull)
                AtomicFetchAdd(header_->droppedRecords, 1);
            return false;
        }
        auto committedWrite = write;
        if (wrapBytes != 0) {
            if (remaining >= sizeof(MessageHeader)) {
                MessageHeader padding{};
                padding.recordSize = remaining;
                padding.kind = static_cast<std::uint8_t>(MessageKind::Padding);
                std::memcpy(data_ + position, &padding, sizeof(padding));
                if (remaining > sizeof(padding))
                    std::memset(data_ + position + sizeof(padding), 0, remaining - sizeof(padding));
            } else {
                std::memset(data_ + position, 0, remaining);
            }
            committedWrite += remaining;
        }
        const auto destination = committedWrite % capacityValue;
        message.recordSize = recordSize;
        message.payloadSize = static_cast<std::uint32_t>(payload.size());
        std::memcpy(data_ + destination, &message, sizeof(message));
        if (!payload.empty())
            std::memcpy(data_ + destination + sizeof(message), payload.data(), payload.size());
        const auto paddingSize = recordSize - sizeof(message) - payload.size();
        if (paddingSize)
            std::memset(data_ + destination + sizeof(message) + payload.size(), 0, paddingSize);
        AtomicStore(header_->writePosition, committedWrite + recordSize);
        return true;
    }

    RingReadResult Pop(MessageView& output) {
        output = {};
        if (!valid())
            return RingReadResult::ProtocolError;
        const auto capacityValue = capacity();
        for (;;) {
            auto read = AtomicLoad(header_->readPosition, std::memory_order_relaxed);
            const auto write = AtomicLoad(header_->writePosition);
            if (read == write)
                return RingReadResult::Empty;
            const auto occupied = write - read;
            if (occupied > capacityValue) {
                AtomicFetchAdd(header_->protocolErrors, 1);
                return RingReadResult::ProtocolError;
            }
            const auto position = read % capacityValue;
            const auto remaining = capacityValue - position;
            if (remaining < sizeof(MessageHeader)) {
                if (occupied < remaining)
                    return FailRead();
                read += remaining;
                AtomicStore(header_->readPosition, read);
                continue;
            }
            MessageHeader message{};
            std::memcpy(&message, data_ + position, sizeof(message));
            const auto recordSize = message.recordSize.get();
            const auto payloadSize = message.payloadSize.get();
            if (recordSize < sizeof(MessageHeader) || recordSize % kRecordAlignment != 0 ||
                recordSize > remaining || recordSize > occupied || payloadSize > recordSize - sizeof(MessageHeader))
                return FailRead();
            AtomicStore(header_->readPosition, read + recordSize);
            if (message.kind == static_cast<std::uint8_t>(MessageKind::Padding))
                continue;
            output.header = message;
            output.payload = {data_ + position + sizeof(MessageHeader), payloadSize};
            return RingReadResult::Message;
        }
    }

private:
    RingReadResult FailRead() {
        AtomicFetchAdd(header_->protocolErrors, 1);
        return RingReadResult::ProtocolError;
    }

    RingHeader* header_{};
    std::byte* data_{};
    std::size_t areaSize_{};
};

struct StateValue {
    std::uint16_t serviceId{};
    std::uint16_t stateId{};
    std::uint32_t version{};
    std::vector<std::byte> payload;
};

// Allocation-free State Page read target. SnapshotSelected() copies matching
// payloads into scratch while the seqlock is being validated and commits them
// to output only after a stable sequence has been observed. output and scratch
// must have the same size and must not overlap shared bridge memory.
struct StateReadTarget {
    std::uint16_t serviceId{};
    std::uint16_t stateId{};
    std::span<std::byte> output;
    std::span<std::byte> scratch;
    std::uint32_t version{};
    std::size_t size{};
    bool found{};
};

class StatePageView {
public:
    StatePageView() = default;
    StatePageView(std::byte* base, std::size_t size)
        : header_(reinterpret_cast<StatePageHeader*>(base)), base_(base), size_(size) {}

    bool Publish(std::span<const StateValue> values) {
        if (!header_ || size_ < sizeof(StatePageHeader) || values.size() > header_->entryCapacity.get())
            return false;
        std::uint64_t cursor = header_->dataOffset.get();
        for (const auto& value : values) {
            cursor = AlignUp(static_cast<std::uint32_t>(cursor), kRecordAlignment);
            if (cursor + value.payload.size() > size_)
                return false;
            cursor += value.payload.size();
        }
        const auto sequence = AtomicLoad(header_->sequence, std::memory_order_relaxed);
        AtomicStore(header_->sequence, sequence | 1U);
        auto* entries = reinterpret_cast<StateEntry*>(base_ + header_->entriesOffset.get());
        cursor = header_->dataOffset.get();
        for (std::size_t index = 0; index < values.size(); ++index) {
            cursor = AlignUp(static_cast<std::uint32_t>(cursor), kRecordAlignment);
            entries[index].serviceId = values[index].serviceId;
            entries[index].stateId = values[index].stateId;
            entries[index].version = values[index].version;
            entries[index].offset = static_cast<std::uint32_t>(cursor);
            entries[index].size = static_cast<std::uint32_t>(values[index].payload.size());
            if (!values[index].payload.empty())
                std::memcpy(base_ + cursor, values[index].payload.data(), values[index].payload.size());
            cursor += values[index].payload.size();
        }
        header_->entryCount = static_cast<std::uint16_t>(values.size());
        AtomicFetchAdd(header_->generation, 1);
        AtomicStore(header_->sequence, (sequence | 1U) + 1U);
        return true;
    }

    [[nodiscard]] bool Snapshot(std::vector<StateValue>& output, std::uint32_t retries = 8) const {
        if (!header_ || header_->pageSize.get() != size_)
            return false;
        for (std::uint32_t attempt = 0; attempt < retries; ++attempt) {
            const auto before = AtomicLoad(header_->sequence);
            if (before & 1U)
                continue;
            const auto count = header_->entryCount.get();
            if (count > header_->entryCapacity.get())
                return false;
            std::vector<StateValue> snapshot;
            snapshot.reserve(count);
            const auto* entries = reinterpret_cast<const StateEntry*>(base_ + header_->entriesOffset.get());
            bool valid = true;
            for (std::uint16_t index = 0; index < count; ++index) {
                const auto offset = entries[index].offset.get();
                const auto size = entries[index].size.get();
                if (offset > size_ || size > size_ - offset) {
                    valid = false;
                    break;
                }
                StateValue value{entries[index].serviceId.get(), entries[index].stateId.get(),
                                 entries[index].version.get(), {}};
                value.payload.assign(base_ + offset, base_ + offset + size);
                snapshot.push_back(std::move(value));
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = AtomicLoad(header_->sequence);
            if (valid && before == after && !(after & 1U)) {
                output = std::move(snapshot);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool SnapshotSelected(std::span<StateReadTarget> targets,
                                        std::uint32_t retries = 8) const {
        if (!header_ || header_->pageSize.get() != size_)
            return false;
        for (auto& target : targets) {
            if (target.output.size() != target.scratch.size())
                return false;
        }

        for (std::uint32_t attempt = 0; attempt < retries; ++attempt) {
            for (auto& target : targets) {
                target.version = 0;
                target.size = 0;
                target.found = false;
            }

            const auto before = AtomicLoad(header_->sequence);
            if (before & 1U)
                continue;
            const auto count = header_->entryCount.get();
            const auto capacity = header_->entryCapacity.get();
            const auto entriesOffset = header_->entriesOffset.get();
            if (count > capacity || entriesOffset > size_ ||
                static_cast<std::uint64_t>(count) * sizeof(StateEntry) > size_ - entriesOffset)
                return false;

            const auto* entries = reinterpret_cast<const StateEntry*>(base_ + entriesOffset);
            bool valid = true;
            for (std::uint16_t index = 0; index < count && valid; ++index) {
                const auto serviceId = entries[index].serviceId.get();
                const auto stateId = entries[index].stateId.get();
                const auto offset = entries[index].offset.get();
                const auto payloadSize = entries[index].size.get();
                if (offset > size_ || payloadSize > size_ - offset) {
                    valid = false;
                    break;
                }
                for (auto& target : targets) {
                    if (target.found || target.serviceId != serviceId || target.stateId != stateId)
                        continue;
                    if (payloadSize > target.scratch.size()) {
                        valid = false;
                        break;
                    }
                    if (payloadSize != 0)
                        std::memcpy(target.scratch.data(), base_ + offset, payloadSize);
                    target.version = entries[index].version.get();
                    target.size = payloadSize;
                    target.found = true;
                    break;
                }
            }

            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = AtomicLoad(header_->sequence);
            if (!valid || before != after || (after & 1U))
                continue;

            for (auto& target : targets) {
                if (target.found && target.size != 0)
                    std::memcpy(target.output.data(), target.scratch.data(), target.size);
            }
            return true;
        }
        return false;
    }

private:
    StatePageHeader* header_{};
    std::byte* base_{};
    std::size_t size_{};
};

class BulkAreaView {
public:
    BulkAreaView() = default;
    BulkAreaView(std::byte* base, std::size_t size)
        : header_(reinterpret_cast<BulkAreaHeader*>(base)), base_(base), size_(size) {}

    bool TryWrite(BulkOwner owner, std::span<const std::byte> payload, BulkHandle& handle) {
        if (!Valid() || owner == BulkOwner::Free || payload.size() > header_->payloadSize.get())
            return false;
        for (std::uint32_t index = 0; index < header_->blockCount.get(); ++index) {
            auto* block = Block(index);
            auto& stateStorage = block->state.storage;
            auto state = std::atomic_ref<std::uint32_t>(stateStorage);
            auto expected = ToBigEndian(static_cast<std::uint32_t>(BulkState::Free));
            if (!state.compare_exchange_strong(expected,
                                               ToBigEndian(static_cast<std::uint32_t>(BulkState::Writing)),
                                               std::memory_order_acq_rel))
                continue;
            block->owner = static_cast<std::uint32_t>(owner);
            const auto generation = block->generation.get() + 1U;
            block->generation = generation;
            block->size = static_cast<std::uint32_t>(payload.size());
            if (!payload.empty())
                std::memcpy(base_ + block->payloadOffset.get(), payload.data(), payload.size());
            handle.blockIndex = index;
            handle.generation = generation;
            handle.offset = 0;
            handle.size = static_cast<std::uint32_t>(payload.size());
            AtomicStore(block->state, static_cast<std::uint32_t>(BulkState::Ready));
            return true;
        }
        return false;
    }

    bool ReadAndRelease(BulkOwner expectedOwner, const BulkHandle& handle,
                        std::vector<std::byte>& output) {
        if (!Valid() || handle.blockIndex.get() >= header_->blockCount.get())
            return false;
        auto* block = Block(handle.blockIndex.get());
        auto& stateStorage = block->state.storage;
        auto state = std::atomic_ref<std::uint32_t>(stateStorage);
        auto expected = ToBigEndian(static_cast<std::uint32_t>(BulkState::Ready));
        if (!state.compare_exchange_strong(expected,
                                           ToBigEndian(static_cast<std::uint32_t>(BulkState::Reading)),
                                           std::memory_order_acq_rel))
            return false;
        const auto valid = block->owner.get() == static_cast<std::uint32_t>(expectedOwner) &&
                           block->generation.get() == handle.generation.get() &&
                           handle.offset.get() <= block->size.get() &&
                           handle.size.get() <= block->size.get() - handle.offset.get();
        if (valid) {
            const auto start = block->payloadOffset.get() + handle.offset.get();
            output.assign(base_ + start, base_ + start + handle.size.get());
        }
        block->size = 0;
        block->owner = static_cast<std::uint32_t>(BulkOwner::Free);
        AtomicStore(block->state, static_cast<std::uint32_t>(BulkState::Free));
        return valid;
    }

private:
    [[nodiscard]] bool Valid() const noexcept {
        return header_ && size_ >= sizeof(BulkAreaHeader) && header_->magic.get() == kBulkMagic &&
               sizeof(BulkAreaHeader) +
                       static_cast<std::uint64_t>(header_->blockCount.get()) * header_->blockStride.get() <=
                   size_;
    }

    [[nodiscard]] BulkBlockHeader* Block(std::uint32_t index) const noexcept {
        return reinterpret_cast<BulkBlockHeader*>(base_ + sizeof(BulkAreaHeader) +
                                                  index * header_->blockStride.get());
    }

    BulkAreaHeader* header_{};
    std::byte* base_{};
    std::size_t size_{};
};

} // namespace cemuextend::wire
