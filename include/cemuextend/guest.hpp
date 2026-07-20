#pragma once

#include "cemuextend/services.hpp"
#include "cemuextend/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace cemuextend::guest {

struct PlatformCallbacks {
    using Query = std::int32_t (*)(std::uint32_t, void*, std::uint32_t);
    using Open = std::int32_t (*)(const void*, std::uint32_t, std::uint32_t*);
    using Submit = std::int32_t (*)(std::uint32_t, const void*, std::uint32_t);
    using Poll = std::int32_t (*)(std::uint32_t, void*, std::uint32_t, std::uint32_t*);
    using Cancel = std::int32_t (*)(std::uint32_t, std::uint32_t);
    using Close = std::int32_t (*)(std::uint32_t);
    using MonotonicTimeNs = std::uint64_t (*)();

    Query query{};
    Open open{};
    Submit submit{};
    Poll poll{};
    Cancel cancel{};
    Close close{};
    MonotonicTimeNs monotonicTimeNs{};
};

[[nodiscard]] bool ConfigurePlatform(const PlatformCallbacks& callbacks) noexcept;
[[nodiscard]] PlatformCallbacks GetPlatform() noexcept;

using OSDynLoadAcquire = std::int32_t (*)(const char* moduleName, std::uint32_t* moduleHandle);
using OSDynLoadFindExport = std::int32_t (*)(std::uint32_t moduleHandle, bool isData,
	const char* exportName, void** address);

// Resolves CEX2 from the title's cemuextend HLE module. This is the transport
// setup used by trusted_native payloads; isolated payloads use the direct HLE
// stubs below.
[[nodiscard]] bool ConfigureTrustedCafePlatform(OSDynLoadAcquire acquire,
	OSDynLoadFindExport findExport,
	PlatformCallbacks::MonotonicTimeNs monotonicTimeNs) noexcept;

#if defined(__powerpc__) || defined(__PPC__)
extern "C" std::int32_t CEX2Query(std::uint32_t, void*, std::uint32_t);
extern "C" std::int32_t CEX2Open(const void*, std::uint32_t, std::uint32_t*);
extern "C" std::int32_t CEX2Submit(std::uint32_t, const void*, std::uint32_t);
extern "C" std::int32_t CEX2Poll(std::uint32_t, void*, std::uint32_t, std::uint32_t*);
extern "C" std::int32_t CEX2Cancel(std::uint32_t, std::uint32_t);
extern "C" std::int32_t CEX2Close(std::uint32_t);

[[nodiscard]] inline bool ConfigureCemodPlatform(PlatformCallbacks::MonotonicTimeNs monotonicTimeNs) noexcept {
    return ConfigurePlatform({CEX2Query, CEX2Open, CEX2Submit, CEX2Poll, CEX2Cancel,
                              CEX2Close, monotonicTimeNs});
}
#endif

struct Version {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint64_t buildId{};
};

struct Statistics {
    std::uint64_t requestsQueued{};
    std::uint64_t requestsSent{};
    std::uint64_t responsesReceived{};
    std::uint64_t eventsReceived{};
    std::uint64_t queueOverflows{};
    std::uint64_t transportBusy{};
    std::uint64_t protocolErrors{};
    std::uint64_t bytesCopied{};
};

using ResponseCallback = std::function<void(wire::Status, std::span<const std::byte>)>;
using EventCallback = std::function<void(std::uint16_t operation, std::span<const std::byte>)>;

struct Request {
    wire::ServiceId service{wire::ServiceId::Core};
    std::uint16_t operation{};
    std::vector<std::byte> payload;
    ResponseCallback callback;
    std::uint16_t operationVersion{transport::kOperationVersion};
    std::uint16_t flags{};
};

struct PageRequest {
    std::uint16_t maximumEntries{transport::kMaximumPageEntries};
    std::vector<std::byte> continuationToken;
};

struct PageInfo {
    bool truncated{};
    std::vector<std::byte> continuationToken;
};

struct InitializeOptions {
    std::size_t maximumQueuedRequests{128};
    std::size_t maximumPendingRequests{128};
    std::uint64_t requestTimeoutNs{5'000'000'000ULL};
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] wire::Error Initialize(const InitializeOptions& options = {});
    void Shutdown();
    void Pump();

    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] Version HostVersion() const noexcept;
    [[nodiscard]] Statistics GetStatistics() const noexcept;

    [[nodiscard]] wire::Error Send(Request request, std::uint32_t* correlationId = nullptr);
    [[nodiscard]] wire::Error Cancel(std::uint32_t correlationId);
    [[nodiscard]] wire::Error Subscribe(wire::ServiceId service, EventCallback callback);
    [[nodiscard]] wire::Error Unsubscribe(wire::ServiceId service);
    [[nodiscard]] wire::Error GetServices(ResponseCallback callback);
    [[nodiscard]] wire::Error Ping(std::uint64_t cookie, ResponseCallback callback = {});
    [[nodiscard]] wire::Error GetVersion(ResponseCallback callback);
    [[nodiscard]] wire::Error GetHostStatistics(ResponseCallback callback);
    [[nodiscard]] wire::Error InputInjectGuest(std::span<const std::byte> payload,
                                               ResponseCallback callback = {});
    [[nodiscard]] wire::Error InputInjectMapped(std::uint8_t channel,
                                                const wire::ObservedVpadState& state,
                                                ResponseCallback callback = {});
    [[nodiscard]] wire::Error InputGetObserved(std::uint8_t channel,
                                               ResponseCallback callback);
    [[nodiscard]] wire::Error Log(wire::LogLevel level, std::string_view message,
                                  ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationGet(std::string_view key, ResponseCallback callback);
    [[nodiscard]] wire::Error ConfigurationSet(std::string_view key, wire::ValueType type,
                                                std::span<const std::byte> value,
                                                ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationDelete(std::string_view key,
                                                   ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationList(std::string_view prefix, ResponseCallback callback);
    [[nodiscard]] wire::Error ConfigurationList(std::string_view prefix, const PageRequest& page,
                                                ResponseCallback callback);
    [[nodiscard]] wire::Error FileStat(std::string_view path, ResponseCallback callback);
    [[nodiscard]] wire::Error FileList(std::string_view path, ResponseCallback callback);
    [[nodiscard]] wire::Error FileList(std::string_view path, const PageRequest& page,
                                      ResponseCallback callback);
    [[nodiscard]] wire::Error FileRead(std::string_view path, std::uint64_t offset,
                                       std::uint32_t size, ResponseCallback callback);
    [[nodiscard]] wire::Error FileWrite(std::string_view path, std::uint64_t offset,
                                        std::span<const std::byte> data,
                                        ResponseCallback callback = {});
    [[nodiscard]] wire::Error FileMkdir(std::string_view path, ResponseCallback callback = {});
    [[nodiscard]] wire::Error FileRemove(std::string_view path, ResponseCallback callback = {});
    [[nodiscard]] wire::Error FileRename(std::string_view from, std::string_view to,
                                         ResponseCallback callback = {});
    [[nodiscard]] wire::Error ClipboardGet(ResponseCallback callback);
    [[nodiscard]] wire::Error ClipboardSet(std::string_view text, ResponseCallback callback = {});
    [[nodiscard]] wire::Error WindowGet(ResponseCallback callback);
    [[nodiscard]] wire::Error CaptureOpen(bool drc, ResponseCallback callback);
    [[nodiscard]] wire::Error CaptureRead(std::uint32_t handle, std::uint32_t offset,
                                          ResponseCallback callback);
    [[nodiscard]] wire::Error CaptureClose(std::uint32_t handle, ResponseCallback callback = {});
    [[nodiscard]] wire::Error DiagnosticsGet(ResponseCallback callback);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cemuextend::guest
