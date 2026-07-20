#include "cemuextend/guest.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cemuextend::guest {
namespace {

PlatformCallbacks gPlatform{};
std::mutex gPlatformMutex;
std::atomic_bool gPlatformConfigured{};

using QueryFunction = PlatformCallbacks::Query;
using OpenFunction = PlatformCallbacks::Open;
using SubmitFunction = PlatformCallbacks::Submit;
using PollFunction = PlatformCallbacks::Poll;
using CancelFunction = PlatformCallbacks::Cancel;
using CloseFunction = PlatformCallbacks::Close;
using TimeFunction = PlatformCallbacks::MonotonicTimeNs;

void InvokeResponse(const ResponseCallback& callback, wire::Status status,
                    std::span<const std::byte> payload) noexcept {
    if (!callback)
        return;
    try {
        callback(status, payload);
    } catch (...) {
        // A client callback must not prevent completion of the remaining work.
    }
}

void InvokeEvent(const EventCallback& callback, std::uint16_t operation,
                 std::span<const std::byte> payload) noexcept {
    if (!callback)
        return;
    try {
        callback(operation, payload);
    } catch (...) {
        // Event listeners are isolated from the transport state machine.
    }
}

wire::Status ErrorStatus(wire::Error error) {
    switch (error) {
    case wire::Error::InvalidArgument: return wire::Status::InvalidArgument;
    case wire::Error::PermissionDenied: return wire::Status::PermissionDenied;
    case wire::Error::NotSupported: return wire::Status::NotSupported;
    case wire::Error::Busy: return wire::Status::Busy;
    case wire::Error::NotFound: return wire::Status::NotFound;
    case wire::Error::TooLarge: return wire::Status::TooLarge;
    case wire::Error::IoError: return wire::Status::IoError;
    case wire::Error::TimedOut: return wire::Status::TimedOut;
    case wire::Error::Disconnected: return wire::Status::Disconnected;
    default: return wire::Status::ProtocolError;
    }
}

} // namespace

bool ConfigurePlatform(const PlatformCallbacks& callbacks) noexcept {
    if (!callbacks.query || !callbacks.open || !callbacks.submit || !callbacks.poll ||
        !callbacks.cancel || !callbacks.close || !callbacks.monotonicTimeNs)
        return false;
    {
        std::lock_guard lock(gPlatformMutex);
        gPlatform = callbacks;
    }
    gPlatformConfigured.store(true, std::memory_order_release);
    return true;
}

PlatformCallbacks GetPlatform() noexcept {
    std::lock_guard lock(gPlatformMutex);
    return gPlatform;
}

bool ConfigureTrustedCafePlatform(OSDynLoadAcquire acquire, OSDynLoadFindExport findExport,
                                  PlatformCallbacks::MonotonicTimeNs monotonicTimeNs) noexcept {
    if (!acquire || !findExport || !monotonicTimeNs)
        return false;
    std::uint32_t module{};
    if (acquire("cemuextend", &module) != 0 || module == 0)
        return false;
    PlatformCallbacks callbacks{};
    callbacks.monotonicTimeNs = monotonicTimeNs;
    auto resolve = [&](const char* name, auto& destination) {
        void* address{};
        if (findExport(module, false, name, &address) != 0 || !address)
            return false;
        static_assert(sizeof(destination) == sizeof(address));
        std::memcpy(&destination, &address, sizeof(address));
        return true;
    };
    if (!resolve("CEX2Query", callbacks.query) || !resolve("CEX2Open", callbacks.open) ||
        !resolve("CEX2Submit", callbacks.submit) || !resolve("CEX2Poll", callbacks.poll) ||
        !resolve("CEX2Cancel", callbacks.cancel) || !resolve("CEX2Close", callbacks.close))
        return false;
    return ConfigurePlatform(callbacks);
}

struct Client::Impl {
    struct Outbound { Request request; std::uint32_t correlation{}; };
    struct Pending {
        ResponseCallback callback;
        std::uint64_t deadline{};
        wire::ServiceId service{wire::ServiceId::Core};
        std::uint16_t operation{};
    };

    mutable std::mutex mutex;
    std::deque<Outbound> queue;
    std::unordered_map<std::uint32_t, Pending> pending;
    std::unordered_set<std::uint32_t> activeCorrelations;
    std::unordered_set<std::uint32_t> retiredCorrelations;
    std::unordered_map<std::uint16_t, EventCallback> subscriptions;
    std::size_t maximumQueuedRequests{128};
    std::size_t maximumPendingRequests{128};
    std::uint64_t requestTimeoutNs{5'000'000'000ULL};
    Statistics statistics{};
    std::uint64_t nextCorrelation{1};
    std::atomic_bool connected{};
    std::atomic_bool shuttingDown{};
    QueryFunction query{};
    OpenFunction open{};
    SubmitFunction submit{};
    PollFunction poll{};
    CancelFunction cancel{};
    CloseFunction close{};
    TimeFunction time{};
    std::uint32_t sessionId{};
    std::uint64_t hostBuildId{};

    void CompleteAll(wire::Status status) {
        std::vector<ResponseCallback> callbacks;
        {
            std::lock_guard lock(mutex);
            callbacks.reserve(queue.size() + pending.size());
            for (auto& outbound : queue)
                if (outbound.request.callback)
                    callbacks.push_back(std::move(outbound.request.callback));
            for (auto& [id, entry] : pending)
                if (entry.callback)
                    callbacks.push_back(std::move(entry.callback));
            queue.clear();
            pending.clear();
            activeCorrelations.clear();
            retiredCorrelations.clear();
        }
        for (auto& callback : callbacks)
            InvokeResponse(callback, status, {});
    }

    void Disconnect(wire::Status status) {
        connected.store(false, std::memory_order_release);
        const auto session = std::exchange(sessionId, 0U);
        if (close && session)
            close(session);
        CompleteAll(status);
        std::lock_guard lock(mutex);
        subscriptions.clear();
    }
};

Client::Client() : impl_(std::make_shared<Impl>()) {}
Client::~Client() { Shutdown(); }

wire::Error Client::Initialize(const InitializeOptions& options) {
    if (!gPlatformConfigured.load(std::memory_order_acquire))
        return wire::Error::Unavailable;
    if (options.maximumQueuedRequests == 0 || options.maximumPendingRequests == 0 ||
        options.maximumPendingRequests > transport::kMaximumResponseQueue || options.requestTimeoutNs == 0)
        return wire::Error::InvalidArgument;
    if (impl_->connected.load(std::memory_order_acquire))
        Shutdown();

    const auto platform = GetPlatform();
    impl_->query = platform.query;
    impl_->open = platform.open;
    impl_->submit = platform.submit;
    impl_->poll = platform.poll;
    impl_->cancel = platform.cancel;
    impl_->close = platform.close;
    impl_->time = platform.monotonicTimeNs;

    transport::Info info{};
    const auto queried = static_cast<wire::Error>(impl_->query(
        static_cast<std::uint32_t>(transport::Query::Info), &info, sizeof(info)));
    if (queried != wire::Error::Ok)
        return queried;
    if (info.abiMajor.get() != transport::kAbiMajor || info.abiMinor.get() < transport::kAbiMinor)
        return wire::Error::AbiMismatch;
    if (info.maximumMessageSize.get() < transport::kMaximumMessageSize)
        return wire::Error::AbiMismatch;
    constexpr auto requiredFeatures = static_cast<std::uint64_t>(transport::Feature::CopyTransport) |
        static_cast<std::uint64_t>(transport::Feature::Cancellation) |
        static_cast<std::uint64_t>(transport::Feature::Pagination) |
        static_cast<std::uint64_t>(transport::Feature::PermissionRevocation);
    if ((info.features.get() & requiredFeatures) != requiredFeatures)
        return wire::Error::AbiMismatch;

    transport::OpenOptions openOptions{};
    openOptions.abiMajor = transport::kAbiMajor;
    openOptions.abiMinor = transport::kAbiMinor;
    openOptions.maximumPendingRequests = static_cast<std::uint32_t>(options.maximumPendingRequests);
    std::uint32_t session{};
    const auto opened = static_cast<wire::Error>(impl_->open(&openOptions, sizeof(openOptions), &session));
    if (opened != wire::Error::Ok)
        return opened;
    if (session == 0) {
        return wire::Error::ProtocolError;
    }

    impl_->sessionId = session;
    impl_->hostBuildId = info.hostBuildId.get();
    impl_->maximumQueuedRequests = options.maximumQueuedRequests;
    impl_->maximumPendingRequests = options.maximumPendingRequests;
    impl_->requestTimeoutNs = options.requestTimeoutNs;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->nextCorrelation = 1;
    }
    impl_->shuttingDown.store(false, std::memory_order_release);
    impl_->connected.store(true, std::memory_order_release);
    return wire::Error::Ok;
}

void Client::Shutdown() {
    auto impl = impl_;
    if (!impl->connected.load(std::memory_order_acquire) && impl->sessionId == 0)
        return;
    bool expected = false;
    if (!impl->shuttingDown.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    impl->connected.store(false, std::memory_order_release);
    const auto session = std::exchange(impl->sessionId, 0U);
    if (impl->close && session)
        impl->close(session);
    impl->CompleteAll(wire::Status::Disconnected);
    {
        std::lock_guard lock(impl->mutex);
        impl->subscriptions.clear();
    }
}

void Client::Pump() {
    auto impl = impl_;
    if (!impl->connected.load(std::memory_order_acquire))
        return;
    const auto now = impl->time();
    const auto dispatchBudgetExhausted = [&] {
        return impl->time() - now >= 2'000'000ULL;
    };

    std::vector<std::pair<std::uint32_t, ResponseCallback>> timedOut;
    {
        std::lock_guard lock(impl->mutex);
        for (auto iterator = impl->pending.begin(); iterator != impl->pending.end();) {
            if (now < iterator->second.deadline) {
                ++iterator;
                continue;
            }
            if (iterator->second.callback)
                timedOut.emplace_back(iterator->first, std::move(iterator->second.callback));
            iterator = impl->pending.erase(iterator);
        }
    }
    for (auto& [correlation, callback] : timedOut) {
        const auto cancelled = static_cast<wire::Error>(impl->cancel(impl->sessionId, correlation));
        {
            std::lock_guard lock(impl->mutex);
            if (cancelled == wire::Error::Ok)
                impl->retiredCorrelations.insert(correlation);
            else
                impl->activeCorrelations.erase(correlation);
        }
        InvokeResponse(callback, wire::Status::TimedOut, {});
        if (!impl->connected.load(std::memory_order_acquire))
            return;
    }

    for (std::uint32_t count = 0; count < 64; ++count) {
        Impl::Outbound outbound;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->queue.empty() || impl->pending.size() >= impl->maximumPendingRequests)
                break;
            outbound = impl->queue.front();
        }
        const auto payloadSize = outbound.request.payload.size();
        std::vector<std::byte> message(sizeof(transport::RequestHeader) + payloadSize);
		transport::RequestHeader header{};
        header.totalSize = static_cast<std::uint32_t>(message.size());
        header.correlationId = outbound.correlation;
        header.serviceId = static_cast<std::uint16_t>(outbound.request.service);
        header.operation = outbound.request.operation;
        header.operationVersion = outbound.request.operationVersion;
        header.flags = outbound.request.flags;
		std::memcpy(message.data(), &header, sizeof(header));
        if (!outbound.request.payload.empty())
            std::memcpy(message.data() + sizeof(header), outbound.request.payload.data(),
                        outbound.request.payload.size());
        const auto result = static_cast<wire::Error>(
            impl->submit(impl->sessionId, message.data(), static_cast<std::uint32_t>(message.size())));
        if (result == wire::Error::Busy) {
            std::lock_guard lock(impl->mutex);
            ++impl->statistics.transportBusy;
            break;
        }
        ResponseCallback rejected;
        bool queueMismatch{};
        {
            std::lock_guard lock(impl->mutex);
            if (impl->queue.empty() || impl->queue.front().correlation != outbound.correlation) {
                queueMismatch = true;
            } else {
                rejected = std::move(impl->queue.front().request.callback);
                impl->queue.pop_front();
                if (result == wire::Error::Ok) {
                    impl->pending.emplace(outbound.correlation,
                        Impl::Pending{std::move(rejected), now + impl->requestTimeoutNs,
                                      outbound.request.service, outbound.request.operation});
                    ++impl->statistics.requestsSent;
                    impl->statistics.bytesCopied += message.size();
                } else {
                    impl->activeCorrelations.erase(outbound.correlation);
                    ++impl->statistics.protocolErrors;
                }
            }
        }
        if (queueMismatch) {
            impl->Disconnect(wire::Status::ProtocolError);
            return;
        }
        if (result == wire::Error::Ok)
        {
            if (dispatchBudgetExhausted())
                break;
            continue;
        }
        if (rejected)
            InvokeResponse(rejected, ErrorStatus(result), {});
        if (result == wire::Error::Disconnected || result == wire::Error::ProtocolError) {
            impl->Disconnect(ErrorStatus(result));
            return;
        }
        if (dispatchBudgetExhausted())
            break;
    }

    if (dispatchBudgetExhausted())
        return;
    std::array<std::byte, transport::kMaximumMessageSize> response{};
    for (std::uint32_t count = 0; count < 64; ++count) {
        std::uint32_t responseSize{};
        const auto result = static_cast<wire::Error>(impl->poll(
            impl->sessionId, response.data(), static_cast<std::uint32_t>(response.size()), &responseSize));
        if (result == wire::Error::NotFound || result == wire::Error::Busy)
            break;
        if (result != wire::Error::Ok || responseSize < sizeof(transport::ResponseHeader) ||
            responseSize > response.size()) {
            impl->Disconnect(result == wire::Error::Disconnected ? wire::Status::Disconnected
                                                                 : wire::Status::ProtocolError);
            return;
        }
		transport::ResponseHeader header{};
		std::memcpy(&header, response.data(), sizeof(header));
        const auto flags = header.flags.get();
        const auto statusValue = header.status.get();
        if (header.totalSize.get() != responseSize || flags > static_cast<std::uint16_t>(transport::ResponseFlag::Event) ||
            statusValue > static_cast<std::uint16_t>(wire::Status::Cancelled)) {
            impl->Disconnect(wire::Status::ProtocolError);
            return;
        }
		const auto payload = std::span<const std::byte>(response).subspan(
			sizeof(transport::ResponseHeader), responseSize - sizeof(transport::ResponseHeader));
        if (flags == static_cast<std::uint16_t>(transport::ResponseFlag::Event)) {
            if (header.correlationId.get() != 0) {
                impl->Disconnect(wire::Status::ProtocolError);
                return;
            }
            EventCallback callback;
            {
                std::lock_guard lock(impl->mutex);
                ++impl->statistics.eventsReceived;
                if (const auto found = impl->subscriptions.find(header.serviceId.get());
                    found != impl->subscriptions.end())
                    callback = found->second;
            }
            InvokeEvent(callback, header.operation.get(), payload);
        } else {
            ResponseCallback callback;
            bool retired{};
            bool protocolError{};
            {
                std::lock_guard lock(impl->mutex);
                const auto correlation = header.correlationId.get();
                if (const auto found = impl->pending.find(correlation); found != impl->pending.end()) {
                    if (header.serviceId.get() != static_cast<std::uint16_t>(found->second.service) ||
                        header.operation.get() != found->second.operation) {
                        protocolError = true;
                    } else {
                        callback = std::move(found->second.callback);
                        impl->pending.erase(found);
                        impl->activeCorrelations.erase(correlation);
                    }
                } else if (impl->retiredCorrelations.erase(correlation)) {
                    impl->activeCorrelations.erase(correlation);
                    retired = true;
                } else {
                    protocolError = true;
                }
                if (!protocolError) {
                    ++impl->statistics.responsesReceived;
                    impl->statistics.bytesCopied += responseSize;
                }
            }
            if (protocolError) {
                impl->Disconnect(wire::Status::ProtocolError);
                return;
            }
            if (retired)
                continue;
            InvokeResponse(callback, static_cast<wire::Status>(statusValue), payload);
        }
        if (!impl->connected.load(std::memory_order_acquire))
            return;
        if (dispatchBudgetExhausted())
            break;
    }
}

bool Client::IsConnected() const noexcept { return impl_->connected.load(std::memory_order_acquire); }
Version Client::HostVersion() const noexcept {
    return {transport::kAbiMajor, transport::kAbiMinor, impl_->hostBuildId};
}
Statistics Client::GetStatistics() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->statistics;
}

wire::Error Client::Send(Request request, std::uint32_t* correlationId) {
    const auto payloadSize = request.payload.size();
    if (request.flags != 0 || request.operationVersion == 0)
        return wire::Error::InvalidArgument;
    if (payloadSize > transport::kMaximumMessageSize - sizeof(transport::RequestHeader))
        return wire::Error::TooLarge;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->connected.load(std::memory_order_acquire))
        return wire::Error::Disconnected;
    if (impl_->queue.size() >= impl_->maximumQueuedRequests) {
        ++impl_->statistics.queueOverflows;
        return wire::Error::Busy;
    }
    if (impl_->nextCorrelation > std::numeric_limits<std::uint32_t>::max())
        return wire::Error::Busy;
    const auto correlation = static_cast<std::uint32_t>(impl_->nextCorrelation++);
    if (impl_->activeCorrelations.contains(correlation))
        return wire::Error::ProtocolError;
    impl_->activeCorrelations.insert(correlation);
    impl_->queue.push_back({std::move(request), correlation});
    ++impl_->statistics.requestsQueued;
    if (correlationId)
        *correlationId = correlation;
    return wire::Error::Ok;
}

wire::Error Client::Cancel(std::uint32_t correlationId) {
    if (!correlationId)
        return wire::Error::InvalidArgument;
    ResponseCallback callback;
    bool removedQueued{};
    bool pending{};
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->connected.load(std::memory_order_acquire))
            return wire::Error::Disconnected;
        const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
            [correlationId](const Impl::Outbound& item) { return item.correlation == correlationId; });
        if (queued != impl_->queue.end()) {
            callback = std::move(queued->request.callback);
            impl_->queue.erase(queued);
            impl_->activeCorrelations.erase(correlationId);
            removedQueued = true;
        } else {
            pending = impl_->pending.contains(correlationId);
        }
    }
    if (removedQueued) {
        InvokeResponse(callback, wire::Status::Cancelled, {});
        return wire::Error::Ok;
    }
    if (!pending)
        return wire::Error::NotFound;
    return static_cast<wire::Error>(impl_->cancel(impl_->sessionId, correlationId));
}

wire::Error Client::Subscribe(wire::ServiceId service, EventCallback callback) {
    if (!callback)
        return wire::Error::InvalidArgument;
    wire::Encoder encoder;
    encoder.U16(static_cast<std::uint16_t>(service));
    auto impl = impl_;
    return Send({wire::ServiceId::Core, static_cast<std::uint16_t>(wire::CoreOperation::Subscribe),
        encoder.Take(), [impl, service, callback = std::move(callback)](
            wire::Status status, std::span<const std::byte>) mutable {
            if (status == wire::Status::Ok) {
                std::lock_guard lock(impl->mutex);
                impl->subscriptions[static_cast<std::uint16_t>(service)] = std::move(callback);
            }
        }});
}

wire::Error Client::Unsubscribe(wire::ServiceId service) {
    wire::Encoder encoder;
    encoder.U16(static_cast<std::uint16_t>(service));
    auto impl = impl_;
    return Send({wire::ServiceId::Core, static_cast<std::uint16_t>(wire::CoreOperation::Unsubscribe),
        encoder.Take(), [impl, service](wire::Status status, std::span<const std::byte>) {
            if (status == wire::Status::Ok) {
                std::lock_guard lock(impl->mutex);
                impl->subscriptions.erase(static_cast<std::uint16_t>(service));
            }
        }});
}

wire::Error Client::GetServices(ResponseCallback cb) { return Send({wire::ServiceId::Core, 1, {}, std::move(cb)}); }
wire::Error Client::Ping(std::uint64_t cookie, ResponseCallback cb) { wire::Encoder e; e.U64(cookie); return Send({wire::ServiceId::Core, 2, e.Take(), std::move(cb)}); }
wire::Error Client::GetVersion(ResponseCallback cb) { return Send({wire::ServiceId::Core, 3, {}, std::move(cb)}); }
wire::Error Client::GetHostStatistics(ResponseCallback cb) { return Send({wire::ServiceId::Core, 6, {}, std::move(cb)}); }
wire::Error Client::InputInjectGuest(std::span<const std::byte> p, ResponseCallback cb) { Request r{wire::ServiceId::Input, 1, {}, std::move(cb)}; r.payload.assign(p.begin(), p.end()); return Send(std::move(r)); }
wire::Error Client::InputInjectMapped(std::uint8_t c, const wire::ObservedVpadState& s, ResponseCallback cb) { if(c>=2)return wire::Error::InvalidArgument; Request r{wire::ServiceId::Input,2,{},std::move(cb)}; r.payload.resize(1+sizeof(s)); r.payload[0]=static_cast<std::byte>(c); std::memcpy(r.payload.data()+1,&s,sizeof(s)); return Send(std::move(r)); }
wire::Error Client::InputGetObserved(std::uint8_t c,ResponseCallback cb){if(c>=2)return wire::Error::InvalidArgument;wire::Encoder e;e.U8(c);return Send({wire::ServiceId::Input,3,e.Take(),std::move(cb)});}
wire::Error Client::Log(wire::LogLevel l, std::string_view m, ResponseCallback cb) { wire::Encoder e; e.U8(static_cast<std::uint8_t>(l)); if(!e.String(m))return wire::Error::TooLarge; return Send({wire::ServiceId::Logging,1,e.Take(),std::move(cb)}); }
wire::Error Client::ConfigurationGet(std::string_view k, ResponseCallback cb) { wire::Encoder e; if(!e.String(k))return wire::Error::TooLarge; return Send({wire::ServiceId::Configuration,1,e.Take(),std::move(cb)}); }
wire::Error Client::ConfigurationSet(std::string_view k, wire::ValueType t, std::span<const std::byte> v, ResponseCallback cb) { wire::Encoder e; if(!e.String(k))return wire::Error::TooLarge; e.U8(static_cast<std::uint8_t>(t)); e.U32(static_cast<std::uint32_t>(v.size())); e.Bytes(v); return Send({wire::ServiceId::Configuration,2,e.Take(),std::move(cb)}); }
wire::Error Client::ConfigurationDelete(std::string_view k, ResponseCallback cb) { wire::Encoder e; if(!e.String(k))return wire::Error::TooLarge; return Send({wire::ServiceId::Configuration,3,e.Take(),std::move(cb)}); }
wire::Error Client::ConfigurationList(std::string_view p, ResponseCallback cb) { return ConfigurationList(p, {}, std::move(cb)); }
wire::Error Client::ConfigurationList(std::string_view p, const PageRequest& page, ResponseCallback cb) { if(page.maximumEntries==0||page.maximumEntries>transport::kMaximumPageEntries)return wire::Error::InvalidArgument; wire::Encoder e; if(!e.String(p))return wire::Error::TooLarge;e.U16(page.maximumEntries);e.U32(static_cast<std::uint32_t>(page.continuationToken.size()));e.Bytes(page.continuationToken);return Send({wire::ServiceId::Configuration,4,e.Take(),std::move(cb)}); }
wire::Error Client::FileStat(std::string_view p, ResponseCallback cb) { wire::Encoder e; if(!e.String(p))return wire::Error::TooLarge; return Send({wire::ServiceId::File,1,e.Take(),std::move(cb)}); }
wire::Error Client::FileList(std::string_view p, ResponseCallback cb) { return FileList(p, {}, std::move(cb)); }
wire::Error Client::FileList(std::string_view p, const PageRequest& page, ResponseCallback cb) { if(page.maximumEntries==0||page.maximumEntries>transport::kMaximumPageEntries)return wire::Error::InvalidArgument;wire::Encoder e;if(!e.String(p))return wire::Error::TooLarge;e.U16(page.maximumEntries);e.U32(static_cast<std::uint32_t>(page.continuationToken.size()));e.Bytes(page.continuationToken);return Send({wire::ServiceId::File,2,e.Take(),std::move(cb)}); }
wire::Error Client::FileRead(std::string_view p,std::uint64_t o,std::uint32_t s,ResponseCallback cb){wire::Encoder e;if(!e.String(p))return wire::Error::TooLarge;e.U64(o);e.U32(s);return Send({wire::ServiceId::File,3,e.Take(),std::move(cb)});}
wire::Error Client::FileWrite(std::string_view p,std::uint64_t o,std::span<const std::byte>d,ResponseCallback cb){wire::Encoder e;if(!e.String(p))return wire::Error::TooLarge;e.U64(o);e.Bytes(d);return Send({wire::ServiceId::File,4,e.Take(),std::move(cb)});}
wire::Error Client::FileMkdir(std::string_view p,ResponseCallback cb){wire::Encoder e;if(!e.String(p))return wire::Error::TooLarge;return Send({wire::ServiceId::File,5,e.Take(),std::move(cb)});}
wire::Error Client::FileRemove(std::string_view p,ResponseCallback cb){wire::Encoder e;if(!e.String(p))return wire::Error::TooLarge;return Send({wire::ServiceId::File,6,e.Take(),std::move(cb)});}
wire::Error Client::FileRename(std::string_view a,std::string_view b,ResponseCallback cb){wire::Encoder e;if(!e.String(a)||!e.String(b))return wire::Error::TooLarge;return Send({wire::ServiceId::File,7,e.Take(),std::move(cb)});}
wire::Error Client::ClipboardGet(ResponseCallback cb){return Send({wire::ServiceId::Clipboard,1,{},std::move(cb)});}
wire::Error Client::ClipboardSet(std::string_view t,ResponseCallback cb){wire::Encoder e;if(!e.String(t))return wire::Error::TooLarge;return Send({wire::ServiceId::Clipboard,2,e.Take(),std::move(cb)});}
wire::Error Client::WindowGet(ResponseCallback cb){return Send({wire::ServiceId::Window,1,{},std::move(cb)});}
wire::Error Client::CaptureOpen(bool d,ResponseCallback cb){wire::Encoder e;e.U8(d?1:0);return Send({wire::ServiceId::Capture,1,e.Take(),std::move(cb)});}
wire::Error Client::CaptureRead(std::uint32_t h,std::uint32_t o,ResponseCallback cb){wire::Encoder e;e.U32(h);e.U32(o);return Send({wire::ServiceId::Capture,2,e.Take(),std::move(cb)});}
wire::Error Client::CaptureClose(std::uint32_t h,ResponseCallback cb){wire::Encoder e;e.U32(h);return Send({wire::ServiceId::Capture,3,e.Take(),std::move(cb)});}
wire::Error Client::DiagnosticsGet(ResponseCallback cb){return Send({wire::ServiceId::Diagnostics,1,{},std::move(cb)});}

} // namespace cemuextend::guest
