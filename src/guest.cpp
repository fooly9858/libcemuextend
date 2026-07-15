#include "cemuextend/guest.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace cemuextend::guest {
namespace {

PlatformCallbacks gPlatform{};
std::atomic_bool gPlatformConfigured{};

using QueryFunction = std::int32_t (*)(std::uint32_t query, void* output, std::uint32_t outputSize);
using RegisterFunction = std::int32_t (*)(std::uint32_t abiVersion, void* region,
                                         std::uint32_t regionSize, std::uint32_t* sessionId);
using NotifyFunction = std::int32_t (*)(std::uint32_t sessionId, std::uint32_t flags);
using UnregisterFunction = std::int32_t (*)(std::uint32_t sessionId);

template <typename Function>
bool Resolve(const PlatformCallbacks& platform, std::uint32_t module, const char* name, Function& output) {
    void* address{};
    if (platform.dynLoadFindExport(module, false, name, &address) != 0 || !address)
        return false;
    output = reinterpret_cast<Function>(address);
    return true;
}

wire::Status DecodeStatus(const wire::MessageHeader& header) {
    const auto status = header.status.get();
    if (status > static_cast<std::uint16_t>(wire::Status::TimedOut))
        return wire::Status::ProtocolError;
    return static_cast<wire::Status>(status);
}

} // namespace

bool ConfigurePlatform(const PlatformCallbacks& callbacks) noexcept {
    if (!callbacks.dynLoadAcquire || !callbacks.dynLoadFindExport || !callbacks.alignedAllocate ||
        !callbacks.free || !callbacks.monotonicTimeNs)
        return false;
    gPlatform = callbacks;
    gPlatformConfigured.store(true, std::memory_order_release);
    return true;
}

const PlatformCallbacks& GetPlatform() noexcept { return gPlatform; }

struct Client::Impl {
    struct Outbound {
        Request request;
        std::uint32_t correlation{};
        wire::MessageKind kind{wire::MessageKind::Request};
    };

    mutable std::mutex mutex;
    std::deque<Outbound> queue;
    std::unordered_map<std::uint32_t, ResponseCallback> pending;
    std::unordered_map<std::uint16_t, EventCallback> subscriptions;
    std::map<std::pair<std::uint16_t, std::uint16_t>, wire::StateValue> stateValues;
    std::size_t maximumQueuedRequests{128};
    Statistics statistics{};
    std::atomic<std::uint32_t> nextCorrelation{1};
    std::atomic_bool connected{};

    void* allocation{};
    std::span<std::byte> region{};
    wire::BridgeHeader* header{};
    wire::RingView guestControl;
    wire::RingView hostControl;
    wire::RingView guestEvents;
    wire::RingView hostEvents;
    wire::StatePageView guestState;
    wire::StatePageView hostState;
    wire::BulkAreaView bulk;

    QueryFunction query{};
    RegisterFunction registerBridge{};
    NotifyFunction notify{};
    UnregisterFunction unregisterBridge{};
    std::uint32_t sessionId{};
    std::uint64_t hostBuildId{};
    std::uint64_t lastHostHeartbeatTime{};
    std::uint32_t lastHostHeartbeat{};
    bool stateDirty{};
    std::uint32_t nextSequence{1};

    wire::Error Queue(Outbound outbound) {
        std::lock_guard lock(mutex);
        if (!connected.load(std::memory_order_acquire))
            return wire::Error::Disconnected;
        if (queue.size() >= maximumQueuedRequests) {
            ++statistics.queueOverflows;
            return wire::Error::Busy;
        }
        ++statistics.requestsQueued;
        queue.push_back(std::move(outbound));
        return wire::Error::Ok;
    }

    void MarkProtocolError() {
        {
            std::lock_guard lock(mutex);
            ++statistics.protocolErrors;
        }
        if (header) {
            header->lastError = static_cast<std::int32_t>(wire::Error::ProtocolError);
            wire::AtomicStore(header->connectionState,
                              static_cast<std::uint32_t>(wire::ConnectionState::Failed));
        }
        connected.store(false, std::memory_order_release);
    }

    void ProcessRing(wire::RingView& ring, bool events) {
        for (;;) {
            wire::MessageView message;
            const auto result = ring.Pop(message);
            if (result == wire::RingReadResult::Empty)
                return;
            if (result == wire::RingReadResult::ProtocolError) {
                MarkProtocolError();
                return;
            }
            if (events || message.header.kind == static_cast<std::uint8_t>(wire::MessageKind::Event)) {
                EventCallback callback;
                {
                    std::lock_guard lock(mutex);
                    ++statistics.eventsReceived;
                    if (const auto found = subscriptions.find(message.header.serviceId.get());
                        found != subscriptions.end())
                        callback = found->second;
                }
                if (callback)
                    callback(message.header.operation.get(), message.payload);
                continue;
            }
            if (message.header.kind != static_cast<std::uint8_t>(wire::MessageKind::Response)) {
                MarkProtocolError();
                return;
            }
            ResponseCallback callback;
            {
                std::lock_guard lock(mutex);
                ++statistics.responsesReceived;
                if (const auto found = pending.find(message.header.correlationId.get()); found != pending.end()) {
                    callback = std::move(found->second);
                    pending.erase(found);
                }
            }
            if (!callback)
                continue;
            if (message.header.flags & static_cast<std::uint8_t>(wire::MessageFlag::HasBulk)) {
                if (message.payload.size() < sizeof(wire::BulkHandle)) {
                    callback(wire::Status::ProtocolError, {});
                    MarkProtocolError();
                    return;
                }
                wire::BulkHandle handle{};
                std::memcpy(&handle, message.payload.data(), sizeof(handle));
                std::vector<std::byte> copied;
                if (!bulk.ReadAndRelease(wire::BulkOwner::Host, handle, copied)) {
                    callback(wire::Status::ProtocolError, {});
                    MarkProtocolError();
                    return;
                }
                {
                    std::lock_guard lock(mutex);
                    statistics.bulkBytes += copied.size();
                }
                callback(DecodeStatus(message.header), copied);
            } else {
                callback(DecodeStatus(message.header), message.payload);
            }
        }
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { Shutdown(); }

wire::Error Client::Initialize(const InitializeOptions& options) {
    if (!gPlatformConfigured.load(std::memory_order_acquire))
        return wire::Error::Unavailable;
    if (options.regionSize < wire::kDefaultRegionSize || options.regionSize > wire::kMaximumRegionSize ||
        options.maximumQueuedRequests == 0)
        return wire::Error::InvalidArgument;
    if (impl_->allocation)
        Shutdown();

    const auto& platform = GetPlatform();
    std::uint32_t module{};
    if (platform.dynLoadAcquire("cemuextend.rpl", &module) != 0 &&
        platform.dynLoadAcquire("cemuextend", &module) != 0)
        return wire::Error::Unavailable;
    if (!Resolve(platform, module, "CEXQuery", impl_->query) ||
        !Resolve(platform, module, "CEXRegister", impl_->registerBridge) ||
        !Resolve(platform, module, "CEXNotify", impl_->notify) ||
        !Resolve(platform, module, "CEXUnregister", impl_->unregisterBridge))
        return wire::Error::Unavailable;

    wire::BridgeInfo info{};
    if (impl_->query(static_cast<std::uint32_t>(wire::Query::BridgeInfo), &info, sizeof(info)) != 0 ||
        info.available.get() == 0)
        return wire::Error::Unavailable;
    if (info.minimumAbiMajor.get() > wire::kAbiMajor || info.maximumAbiMajor.get() < wire::kAbiMajor)
        return wire::Error::AbiMismatch;
    if (options.regionSize > info.maximumRegionSize.get())
        return wire::Error::TooLarge;

    impl_->allocation = platform.alignedAllocate(wire::kAlignment, options.regionSize);
    if (!impl_->allocation)
        return wire::Error::Unavailable;
    impl_->region = {reinterpret_cast<std::byte*>(impl_->allocation), options.regionSize};
    if (const auto initialized = wire::InitializeDefaultRegion(impl_->region, options.guestServices);
        initialized != wire::Error::Ok) {
        platform.free(impl_->allocation);
        impl_->allocation = nullptr;
        impl_->region = {};
        return initialized;
    }

    std::uint32_t session{};
    const auto abi = (static_cast<std::uint32_t>(wire::kAbiMajor) << 16U) | wire::kAbiMinor;
    const auto registered = impl_->registerBridge(abi, impl_->allocation, options.regionSize, &session);
    if (registered != 0) {
        platform.free(impl_->allocation);
        impl_->allocation = nullptr;
        impl_->region = {};
        return static_cast<wire::Error>(registered);
    }
    if (const auto validation = wire::ValidateLayout(impl_->region); !validation) {
        impl_->unregisterBridge(session);
        platform.free(impl_->allocation);
        impl_->allocation = nullptr;
        impl_->region = {};
        return wire::Error::InvalidLayout;
    }

    impl_->header = reinterpret_cast<wire::BridgeHeader*>(impl_->region.data());
    impl_->sessionId = session;
    impl_->header->sessionId = session;
    impl_->hostBuildId = info.hostBuildId.get();
    impl_->maximumQueuedRequests = options.maximumQueuedRequests;
    impl_->guestControl = {impl_->region.data() + impl_->header->guestToHostControlOffset.get(),
                           impl_->header->guestToHostControlSize.get()};
    impl_->hostControl = {impl_->region.data() + impl_->header->hostToGuestControlOffset.get(),
                          impl_->header->hostToGuestControlSize.get()};
    impl_->guestEvents = {impl_->region.data() + impl_->header->guestToHostEventOffset.get(),
                          impl_->header->guestToHostEventSize.get()};
    impl_->hostEvents = {impl_->region.data() + impl_->header->hostToGuestEventOffset.get(),
                         impl_->header->hostToGuestEventSize.get()};
    impl_->guestState = {impl_->region.data() + impl_->header->guestStateOffset.get(),
                         impl_->header->guestStateSize.get()};
    impl_->hostState = {impl_->region.data() + impl_->header->hostStateOffset.get(),
                        impl_->header->hostStateSize.get()};
    impl_->bulk = {impl_->region.data() + impl_->header->bulkOffset.get(), impl_->header->bulkSize.get()};
    impl_->lastHostHeartbeat = wire::AtomicLoad(impl_->header->hostHeartbeat);
    impl_->lastHostHeartbeatTime = platform.monotonicTimeNs();
    wire::AtomicStore(impl_->header->connectionState,
                      static_cast<std::uint32_t>(wire::ConnectionState::Connected));
    impl_->connected.store(true, std::memory_order_release);
    return wire::Error::Ok;
}

void Client::Shutdown() {
    if (!impl_->allocation)
        return;
    impl_->connected.store(false, std::memory_order_release);
    if (impl_->header) {
        wire::AtomicStore(impl_->header->connectionState,
                          static_cast<std::uint32_t>(wire::ConnectionState::Closing));
        if (impl_->notify)
            impl_->notify(impl_->sessionId, static_cast<std::uint32_t>(wire::NotifyFlag::Closing));
    }
    if (impl_->unregisterBridge && impl_->sessionId)
        impl_->unregisterBridge(impl_->sessionId);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.clear();
        impl_->pending.clear();
        impl_->subscriptions.clear();
        impl_->stateValues.clear();
    }
    GetPlatform().free(impl_->allocation);
    impl_->allocation = nullptr;
    impl_->region = {};
    impl_->header = nullptr;
    impl_->sessionId = 0;
}

void Client::Pump() {
    if (!impl_->connected.load(std::memory_order_acquire) || !impl_->header)
        return;
    const auto& platform = GetPlatform();
    const auto now = platform.monotonicTimeNs();
    wire::AtomicFetchAdd(impl_->header->guestHeartbeat, 1);
    const auto hostHeartbeat = wire::AtomicLoad(impl_->header->hostHeartbeat);
    if (hostHeartbeat != impl_->lastHostHeartbeat) {
        impl_->lastHostHeartbeat = hostHeartbeat;
        impl_->lastHostHeartbeatTime = now;
    } else if (now - impl_->lastHostHeartbeatTime >
               static_cast<std::uint64_t>(wire::kHeartbeatTimeoutMs) * 1'000'000ULL) {
        impl_->header->lastError = static_cast<std::int32_t>(wire::Error::TimedOut);
        impl_->connected.store(false, std::memory_order_release);
        return;
    }
    const auto state = static_cast<wire::ConnectionState>(wire::AtomicLoad(impl_->header->connectionState));
    if (state == wire::ConnectionState::Closing || state == wire::ConnectionState::Failed) {
        impl_->connected.store(false, std::memory_order_release);
        return;
    }

    bool sentControl = false;
    bool sentEvent = false;
    for (;;) {
        Impl::Outbound outbound;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->queue.empty())
                break;
            outbound = impl_->queue.front();
        }
        wire::MessageHeader message{};
        message.serviceId = static_cast<std::uint16_t>(outbound.request.service);
        message.operation = outbound.request.operation;
        message.kind = static_cast<std::uint8_t>(outbound.kind);
        message.flags = outbound.request.flags;
        message.correlationId = outbound.correlation;
        message.timestampNs = now;
        message.sequence = impl_->nextSequence++;

        std::vector<std::byte> payload = outbound.request.payload;
        wire::BulkHandle bulkHandle{};
        bool hasBulk = !outbound.request.bulkPayload.empty();
        if (hasBulk) {
            if (!impl_->bulk.TryWrite(wire::BulkOwner::Guest, outbound.request.bulkPayload, bulkHandle)) {
                std::lock_guard lock(impl_->mutex);
                ++impl_->statistics.ringBusy;
                break;
            }
            std::vector<std::byte> withHandle(sizeof(bulkHandle) + payload.size());
            std::memcpy(withHandle.data(), &bulkHandle, sizeof(bulkHandle));
            if (!payload.empty())
                std::memcpy(withHandle.data() + sizeof(bulkHandle), payload.data(), payload.size());
            payload = std::move(withHandle);
            message.flags |= static_cast<std::uint8_t>(wire::MessageFlag::HasBulk);
        }
        auto& ring = outbound.kind == wire::MessageKind::Event ? impl_->guestEvents : impl_->guestControl;
        if (!ring.Push(message, payload, outbound.kind == wire::MessageKind::Event)) {
            if (hasBulk) {
                std::vector<std::byte> discard;
                impl_->bulk.ReadAndRelease(wire::BulkOwner::Guest, bulkHandle, discard);
            }
            std::lock_guard lock(impl_->mutex);
            ++impl_->statistics.ringBusy;
            break;
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->queue.pop_front();
            if (outbound.kind == wire::MessageKind::Request && outbound.request.callback)
                impl_->pending.emplace(outbound.correlation, std::move(outbound.request.callback));
            ++impl_->statistics.requestsSent;
            if (hasBulk)
                impl_->statistics.bulkBytes += outbound.request.bulkPayload.size();
        }
        if (outbound.kind == wire::MessageKind::Event)
            sentEvent = true;
        else
            sentControl = true;
    }

    bool publishState = false;
    std::vector<wire::StateValue> stateValues;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stateDirty) {
            for (const auto& [key, value] : impl_->stateValues)
                stateValues.push_back(value);
            impl_->stateDirty = false;
            publishState = true;
        }
    }
    if (publishState) {
        if (impl_->guestState.Publish(stateValues)) {
            std::lock_guard lock(impl_->mutex);
            ++impl_->statistics.statePublishes;
        } else {
            impl_->MarkProtocolError();
            return;
        }
    }

    std::uint32_t notifyFlags{};
    if (sentControl)
        notifyFlags |= static_cast<std::uint32_t>(wire::NotifyFlag::Control);
    if (sentEvent)
        notifyFlags |= static_cast<std::uint32_t>(wire::NotifyFlag::Event);
    if (publishState)
        notifyFlags |= static_cast<std::uint32_t>(wire::NotifyFlag::State);
    if (notifyFlags)
        impl_->notify(impl_->sessionId, notifyFlags);

    impl_->ProcessRing(impl_->hostControl, false);
    if (impl_->connected.load(std::memory_order_acquire))
        impl_->ProcessRing(impl_->hostEvents, true);
}

bool Client::IsConnected() const noexcept { return impl_->connected.load(std::memory_order_acquire); }

Version Client::HostVersion() const noexcept { return {wire::kAbiMajor, wire::kAbiMinor, impl_->hostBuildId}; }

Statistics Client::GetStatistics() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->statistics;
}

wire::Error Client::Send(Request request, std::uint32_t* correlationId) {
    auto correlation = impl_->nextCorrelation.fetch_add(1, std::memory_order_relaxed);
    if (correlation == 0)
        correlation = impl_->nextCorrelation.fetch_add(1, std::memory_order_relaxed);
    if (correlationId)
        *correlationId = correlation;
    return impl_->Queue({std::move(request), correlation, wire::MessageKind::Request});
}

wire::Error Client::Subscribe(wire::ServiceId service, EventCallback callback) {
    if (!callback)
        return wire::Error::InvalidArgument;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->subscriptions[static_cast<std::uint16_t>(service)] = std::move(callback);
    }
    wire::Encoder encoder;
    encoder.U16(static_cast<std::uint16_t>(service));
    return Send({wire::ServiceId::Core, static_cast<std::uint16_t>(wire::CoreOperation::Subscribe),
                 encoder.Take()});
}

wire::Error Client::Unsubscribe(wire::ServiceId service) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->subscriptions.erase(static_cast<std::uint16_t>(service));
    }
    wire::Encoder encoder;
    encoder.U16(static_cast<std::uint16_t>(service));
    return Send({wire::ServiceId::Core, static_cast<std::uint16_t>(wire::CoreOperation::Unsubscribe),
                 encoder.Take()});
}

wire::Error Client::PublishState(std::uint16_t serviceId, std::uint16_t stateId,
                                 std::uint32_t version, std::span<const std::byte> payload) {
    if (!IsConnected())
        return wire::Error::Disconnected;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stateValues.size() >= 64 && !impl_->stateValues.contains({serviceId, stateId}))
        return wire::Error::Busy;
    wire::StateValue value{serviceId, stateId, version, {}};
    value.payload.assign(payload.begin(), payload.end());
    impl_->stateValues[{serviceId, stateId}] = std::move(value);
    impl_->stateDirty = true;
    return wire::Error::Ok;
}

wire::Error Client::PublishEvent(std::uint16_t serviceId, std::uint16_t operation,
                                 std::span<const std::byte> payload) {
    Request request;
    request.service = static_cast<wire::ServiceId>(serviceId);
    request.operation = operation;
    request.payload.assign(payload.begin(), payload.end());
    return impl_->Queue({std::move(request), 0, wire::MessageKind::Event});
}

wire::Error Client::ReadHostState(std::uint16_t serviceId, std::uint16_t stateId,
                                 wire::StateValue& output) const {
    if (!IsConnected())
        return wire::Error::Disconnected;
    std::vector<wire::StateValue> values;
    if (!impl_->hostState.Snapshot(values))
        return wire::Error::Busy;
    for (auto& value : values) {
        if (value.serviceId == serviceId && value.stateId == stateId) {
            output = std::move(value);
            return wire::Error::Ok;
        }
    }
    return wire::Error::NotFound;
}

wire::Error Client::ReadHostStates(std::span<wire::StateReadTarget> targets) const {
    if (!IsConnected())
        return wire::Error::Disconnected;
    return impl_->hostState.SnapshotSelected(targets) ? wire::Error::Ok : wire::Error::Busy;
}

wire::Error Client::GetServices(ResponseCallback callback) {
    return Send({wire::ServiceId::Core,
                 static_cast<std::uint16_t>(wire::CoreOperation::GetServices), {}, {},
                 std::move(callback)});
}

wire::Error Client::Ping(std::uint64_t cookie, ResponseCallback callback) {
    wire::Encoder encoder;
    encoder.U64(cookie);
    return Send({wire::ServiceId::Core, static_cast<std::uint16_t>(wire::CoreOperation::Ping),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::GetVersion(ResponseCallback callback) {
    return Send({wire::ServiceId::Core,
                 static_cast<std::uint16_t>(wire::CoreOperation::GetVersion), {}, {},
                 std::move(callback)});
}

wire::Error Client::GetHostStatistics(ResponseCallback callback) {
    return Send({wire::ServiceId::Core,
                 static_cast<std::uint16_t>(wire::CoreOperation::GetStatistics), {}, {},
                 std::move(callback)});
}

wire::Error Client::InputInjectGuest(std::span<const std::byte> payload, ResponseCallback callback) {
    Request request{wire::ServiceId::Input,
                    static_cast<std::uint16_t>(wire::InputOperation::InjectGuest)};
    request.payload.assign(payload.begin(), payload.end());
    request.callback = std::move(callback);
    return Send(std::move(request));
}

wire::Error Client::InputInjectMapped(std::uint8_t channel,
                                      const wire::ObservedVpadState& state,
                                      ResponseCallback callback) {
    if (channel >= 2)
        return wire::Error::InvalidArgument;
    Request request{wire::ServiceId::Input,
                    static_cast<std::uint16_t>(wire::InputOperation::InjectMapped)};
    request.payload.resize(1 + sizeof(state));
    request.payload[0] = static_cast<std::byte>(channel);
    std::memcpy(request.payload.data() + 1, &state, sizeof(state));
    request.callback = std::move(callback);
    return Send(std::move(request));
}

wire::Error Client::Log(wire::LogLevel level, std::string_view message, ResponseCallback callback) {
    wire::Encoder encoder;
    encoder.U8(static_cast<std::uint8_t>(level));
    if (!encoder.String(message))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::Logging, static_cast<std::uint16_t>(wire::LoggingOperation::Write),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::ConfigurationGet(std::string_view key, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(key))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::Configuration,
                 static_cast<std::uint16_t>(wire::ConfigurationOperation::Get), encoder.Take(), {},
                 std::move(callback)});
}

wire::Error Client::ConfigurationSet(std::string_view key, wire::ValueType type,
                                     std::span<const std::byte> value, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(key))
        return wire::Error::TooLarge;
    encoder.U8(static_cast<std::uint8_t>(type));
    encoder.U32(static_cast<std::uint32_t>(value.size()));
    encoder.Bytes(value);
    return Send({wire::ServiceId::Configuration,
                 static_cast<std::uint16_t>(wire::ConfigurationOperation::Set), encoder.Take(), {},
                 std::move(callback)});
}

wire::Error Client::ConfigurationDelete(std::string_view key, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(key))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::Configuration,
                 static_cast<std::uint16_t>(wire::ConfigurationOperation::Delete), encoder.Take(), {},
                 std::move(callback)});
}

wire::Error Client::ConfigurationList(std::string_view prefix, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(prefix))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::Configuration,
                 static_cast<std::uint16_t>(wire::ConfigurationOperation::List), encoder.Take(), {},
                 std::move(callback)});
}

wire::Error Client::FileStat(std::string_view path, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Stat),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::FileList(std::string_view path, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::List),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::FileRead(std::string_view path, std::uint64_t offset, std::uint32_t size,
                            ResponseCallback callback) {
    if (size > wire::kBulkPayloadSize)
        return wire::Error::TooLarge;
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    encoder.U64(offset);
    encoder.U32(size);
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Read),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::FileWrite(std::string_view path, std::uint64_t offset,
                             std::span<const std::byte> data, ResponseCallback callback) {
    if (data.size() > wire::kBulkPayloadSize)
        return wire::Error::TooLarge;
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    encoder.U64(offset);
    Request request{wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Write),
                    encoder.Take(), {}, std::move(callback)};
    request.bulkPayload.assign(data.begin(), data.end());
    return Send(std::move(request));
}

wire::Error Client::FileMkdir(std::string_view path, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Mkdir),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::FileRemove(std::string_view path, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(path))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Remove),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::FileRename(std::string_view from, std::string_view to, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(from) || !encoder.String(to))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::File, static_cast<std::uint16_t>(wire::FileOperation::Rename),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::ClipboardGet(ResponseCallback callback) {
    return Send({wire::ServiceId::Clipboard,
                 static_cast<std::uint16_t>(wire::ClipboardOperation::Get), {}, {},
                 std::move(callback)});
}

wire::Error Client::ClipboardSet(std::string_view text, ResponseCallback callback) {
    wire::Encoder encoder;
    if (!encoder.String(text))
        return wire::Error::TooLarge;
    return Send({wire::ServiceId::Clipboard,
                 static_cast<std::uint16_t>(wire::ClipboardOperation::Set), encoder.Take(), {},
                 std::move(callback)});
}

wire::Error Client::WindowGet(ResponseCallback callback) {
    return Send({wire::ServiceId::Window, static_cast<std::uint16_t>(wire::WindowOperation::Get),
                 {}, {}, std::move(callback)});
}

wire::Error Client::CaptureOpen(bool drc, ResponseCallback callback) {
    wire::Encoder encoder;
    encoder.U8(drc ? 1 : 0);
    return Send({wire::ServiceId::Capture, static_cast<std::uint16_t>(wire::CaptureOperation::Open),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::CaptureRead(std::uint32_t handle, std::uint32_t offset,
                               ResponseCallback callback) {
    wire::Encoder encoder;
    encoder.U32(handle);
    encoder.U32(offset);
    return Send({wire::ServiceId::Capture, static_cast<std::uint16_t>(wire::CaptureOperation::Read),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::CaptureClose(std::uint32_t handle, ResponseCallback callback) {
    wire::Encoder encoder;
    encoder.U32(handle);
    return Send({wire::ServiceId::Capture, static_cast<std::uint16_t>(wire::CaptureOperation::Close),
                 encoder.Take(), {}, std::move(callback)});
}

wire::Error Client::DiagnosticsGet(ResponseCallback callback) {
    return Send({wire::ServiceId::Diagnostics,
                 static_cast<std::uint16_t>(wire::DiagnosticsOperation::Get), {}, {},
                 std::move(callback)});
}

} // namespace cemuextend::guest
