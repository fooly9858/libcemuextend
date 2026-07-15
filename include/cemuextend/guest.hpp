#pragma once

#include "cemuextend/services.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace cemuextend::guest {

struct PlatformCallbacks {
    using DynLoadAcquire = std::int32_t (*)(const char* moduleName, std::uint32_t* moduleHandle);
    using DynLoadFindExport = std::int32_t (*)(std::uint32_t moduleHandle, bool isData,
                                              const char* exportName, void** address);
    using AlignedAllocate = void* (*)(std::size_t alignment, std::size_t size);
    using Free = void (*)(void* address);
    using MonotonicTimeNs = std::uint64_t (*)();

    DynLoadAcquire dynLoadAcquire{};
    DynLoadFindExport dynLoadFindExport{};
    AlignedAllocate alignedAllocate{};
    Free free{};
    MonotonicTimeNs monotonicTimeNs{};
};

[[nodiscard]] bool ConfigurePlatform(const PlatformCallbacks& callbacks) noexcept;
[[nodiscard]] const PlatformCallbacks& GetPlatform() noexcept;

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
    std::uint64_t ringBusy{};
    std::uint64_t protocolErrors{};
    std::uint64_t statePublishes{};
    std::uint64_t bulkBytes{};
};

using ResponseCallback = std::function<void(wire::Status, std::span<const std::byte>)>;
using EventCallback = std::function<void(std::uint16_t operation, std::span<const std::byte>)>;

struct Request {
    wire::ServiceId service{wire::ServiceId::Core};
    std::uint16_t operation{};
    std::vector<std::byte> payload;
    std::vector<std::byte> bulkPayload;
    ResponseCallback callback;
    std::uint8_t flags{};
};

struct InitializeOptions {
    std::uint32_t regionSize{wire::kDefaultRegionSize};
    std::size_t maximumQueuedRequests{128};
    std::span<const wire::ServiceDescriptor> guestServices{};
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
    [[nodiscard]] wire::Error Subscribe(wire::ServiceId service, EventCallback callback);
    [[nodiscard]] wire::Error Unsubscribe(wire::ServiceId service);
    [[nodiscard]] wire::Error PublishState(std::uint16_t serviceId, std::uint16_t stateId,
                                           std::uint32_t version,
                                           std::span<const std::byte> payload);
    [[nodiscard]] wire::Error PublishEvent(std::uint16_t serviceId, std::uint16_t operation,
                                           std::span<const std::byte> payload);
    [[nodiscard]] wire::Error ReadHostState(std::uint16_t serviceId, std::uint16_t stateId,
                                           wire::StateValue& output) const;
    [[nodiscard]] wire::Error ReadHostStates(std::span<wire::StateReadTarget> targets) const;

    [[nodiscard]] wire::Error GetServices(ResponseCallback callback);
    [[nodiscard]] wire::Error Ping(std::uint64_t cookie, ResponseCallback callback = {});
    [[nodiscard]] wire::Error GetVersion(ResponseCallback callback);
    [[nodiscard]] wire::Error GetHostStatistics(ResponseCallback callback);
    [[nodiscard]] wire::Error InputInjectGuest(std::span<const std::byte> payload,
                                               ResponseCallback callback = {});
    [[nodiscard]] wire::Error InputInjectMapped(std::uint8_t channel,
                                                const wire::ObservedVpadState& state,
                                                ResponseCallback callback = {});
    [[nodiscard]] wire::Error Log(wire::LogLevel level, std::string_view message,
                                  ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationGet(std::string_view key, ResponseCallback callback);
    [[nodiscard]] wire::Error ConfigurationSet(std::string_view key, wire::ValueType type,
                                                std::span<const std::byte> value,
                                                ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationDelete(std::string_view key,
                                                   ResponseCallback callback = {});
    [[nodiscard]] wire::Error ConfigurationList(std::string_view prefix, ResponseCallback callback);
    [[nodiscard]] wire::Error FileStat(std::string_view path, ResponseCallback callback);
    [[nodiscard]] wire::Error FileList(std::string_view path, ResponseCallback callback);
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
    std::unique_ptr<Impl> impl_;
};

} // namespace cemuextend::guest
