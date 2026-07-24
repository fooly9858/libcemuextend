#include "cemuextend/guest.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace cemuextend;

namespace {

[[noreturn]] void CheckFailed(const char* expression, int line) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::abort();
}
#define CHECK(condition) do { if (!(condition)) CheckFailed(#condition, __LINE__); } while (false)

struct MockHost {
    std::uint64_t now{1'000'000'000ULL};
    std::uint64_t timeStep{};
    std::uint32_t session{0x12345678};
    std::uint32_t submissions{};
    bool respond{true};
    bool open{};
    std::deque<std::vector<std::byte>> responses;
    std::optional<transport::RequestHeader> deferred;
} gHost;

std::int32_t Query(std::uint32_t query, void* output, std::uint32_t size) {
    if (query != static_cast<std::uint32_t>(transport::Query::Info) || size < sizeof(transport::Info))
        return static_cast<std::int32_t>(wire::Error::InvalidArgument);
    transport::Info info{};
    info.abiMajor = transport::kAbiMajor;
    info.abiMinor = transport::kAbiMinor;
    info.maximumMessageSize = transport::kMaximumMessageSize;
    info.maximumResponseQueue = transport::kMaximumResponseQueue;
    info.maximumPageEntries = transport::kMaximumPageEntries;
    info.hostBuildId = 0x1122334455667788ULL;
    info.features = static_cast<std::uint64_t>(transport::Feature::CopyTransport) |
        static_cast<std::uint64_t>(transport::Feature::Cancellation) |
        static_cast<std::uint64_t>(transport::Feature::Pagination) |
        static_cast<std::uint64_t>(transport::Feature::PermissionRevocation);
    info.coreServiceVersion = 1;
    std::memcpy(output, &info, sizeof(info));
    return 0;
}

std::int32_t Open(const void* bytes, std::uint32_t size, std::uint32_t* session) {
    if (size != sizeof(transport::OpenOptions) || gHost.open)
        return static_cast<std::int32_t>(wire::Error::Busy);
    const auto& options = *static_cast<const transport::OpenOptions*>(bytes);
    if (options.abiMajor.get() != 2)
        return static_cast<std::int32_t>(wire::Error::AbiMismatch);
    gHost.open = true;
    *session = gHost.session;
    return 0;
}

std::int32_t Submit(std::uint32_t session, const void* bytes, std::uint32_t size) {
    CHECK(gHost.open && session == gHost.session);
    if (gHost.responses.size() >= transport::kMaximumResponseQueue)
        return static_cast<std::int32_t>(wire::Error::Busy);
    const auto& request = *static_cast<const transport::RequestHeader*>(bytes);
    CHECK(size == request.totalSize.get());
    ++gHost.submissions;
    if (!gHost.respond) {
        gHost.deferred = request;
        return 0;
    }
    std::vector<std::byte> response(sizeof(transport::ResponseHeader) + size - sizeof(request));
    auto& header = *reinterpret_cast<transport::ResponseHeader*>(response.data());
    header.totalSize = static_cast<std::uint32_t>(response.size());
    header.correlationId = request.correlationId.get();
    header.serviceId = request.serviceId.get();
    header.operation = request.operation.get();
    header.status = static_cast<std::uint16_t>(wire::Status::Ok);
    if (size > sizeof(request))
        std::memcpy(response.data() + sizeof(header), static_cast<const std::byte*>(bytes) + sizeof(request),
                    size - sizeof(request));
    gHost.responses.push_back(std::move(response));
    return 0;
}

std::int32_t Poll(std::uint32_t session, void* output, std::uint32_t capacity, std::uint32_t* size) {
    CHECK(gHost.open && session == gHost.session);
    *size = 0;
    if (gHost.responses.empty())
        return static_cast<std::int32_t>(wire::Error::NotFound);
    if (capacity < gHost.responses.front().size())
        return static_cast<std::int32_t>(wire::Error::TooLarge);
    *size = static_cast<std::uint32_t>(gHost.responses.front().size());
    std::memcpy(output, gHost.responses.front().data(), *size);
    gHost.responses.pop_front();
    return 0;
}

std::int32_t Cancel(std::uint32_t session, std::uint32_t correlation) {
    CHECK(gHost.open && session == gHost.session);
    for (auto& response : gHost.responses) {
        auto& header = *reinterpret_cast<transport::ResponseHeader*>(response.data());
        if (header.correlationId.get() == correlation) {
            header.status = static_cast<std::uint16_t>(wire::Status::Cancelled);
            header.totalSize = sizeof(header);
            response.resize(sizeof(header));
            return 0;
        }
    }
    if (gHost.deferred && gHost.deferred->correlationId.get() == correlation) {
        std::vector<std::byte> response(sizeof(transport::ResponseHeader));
        auto& header = *reinterpret_cast<transport::ResponseHeader*>(response.data());
        header.totalSize = sizeof(header);
        header.correlationId = correlation;
        header.serviceId = gHost.deferred->serviceId.get();
        header.operation = gHost.deferred->operation.get();
        header.status = static_cast<std::uint16_t>(wire::Status::Cancelled);
        gHost.responses.push_back(std::move(response));
        gHost.deferred.reset();
        return 0;
    }
    return static_cast<std::int32_t>(wire::Error::NotFound);
}

std::int32_t Close(std::uint32_t session) {
    CHECK(session == gHost.session);
    gHost.open = false;
    gHost.responses.clear();
    gHost.deferred.reset();
    return 0;
}

std::uint64_t Time() {
    const auto result = gHost.now;
    gHost.now += gHost.timeStep;
    return result;
}

void Configure() {
    CHECK(guest::ConfigurePlatform({Query, Open, Submit, Poll, Cancel, Close, Time}));
}

void TestCopyTransportAndBounds() {
    guest::Client client;
    guest::InitializeOptions options;
    options.maximumQueuedRequests = 2;
    CHECK(client.Initialize(options) == wire::Error::Ok);
    CHECK(client.HostVersion().major == 2);
    CHECK(client.HostVersion().buildId == 0x1122334455667788ULL);
    bool pinged{};
    CHECK(client.Ping(0xfeedfaceULL, [&](wire::Status status, std::span<const std::byte> payload) {
        CHECK(status == wire::Status::Ok);
        wire::Decoder decoder(payload);
        std::uint64_t cookie{};
        CHECK(decoder.U64(cookie) && cookie == 0xfeedfaceULL);
        pinged = true;
    }) == wire::Error::Ok);
    CHECK(client.Ping(2) == wire::Error::Ok);
    CHECK(client.Ping(3) == wire::Error::Busy);
    client.Pump();
    CHECK(pinged);
    CHECK(gHost.submissions == 2);
    CHECK(client.GetStatistics().bytesCopied > 0);

    const std::array fileData{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    bool wrote{};
    CHECK(client.FileWrite("file.bin", 0, fileData,
        [&](wire::Status writeStatus, std::span<const std::byte> payload) {
            CHECK(writeStatus == wire::Status::Ok);
            wire::Decoder decoder(payload);
            std::string path;
            std::uint64_t offset{};
            std::span<const std::byte> encodedData;
            CHECK(decoder.String(path) && path == "file.bin" && decoder.U64(offset) && offset == 0);
            CHECK(decoder.Bytes(decoder.remaining(), encodedData));
            CHECK(encodedData.size() == fileData.size());
            CHECK(std::memcmp(encodedData.data(), fileData.data(), fileData.size()) == 0);
            wrote = true;
        }) == wire::Error::Ok);
    client.Pump();
    CHECK(wrote);

    guest::Request oversized{wire::ServiceId::Core, 2};
    oversized.payload.resize(transport::kMaximumMessageSize);
    CHECK(client.Send(std::move(oversized)) == wire::Error::TooLarge);
    client.Shutdown();
}

void TestCancelTimeoutAndCallbackShutdown() {
    guest::Client client;
    guest::InitializeOptions options;
    options.requestTimeoutNs = 100;
    CHECK(client.Initialize(options) == wire::Error::Ok);
    wire::Status status{wire::Status::Ok};
    std::uint32_t correlation{};
    guest::Request queued{wire::ServiceId::Core, 2};
    queued.callback = [&](wire::Status value, std::span<const std::byte>) { status = value; };
    CHECK(client.Send(std::move(queued), &correlation) == wire::Error::Ok);
    CHECK(client.Cancel(correlation) == wire::Error::Ok);
    CHECK(status == wire::Status::Cancelled);

    correlation = 0;
    wire::Encoder cancelPayload;
    cancelPayload.U64(99);
    CHECK(client.Send({wire::ServiceId::Core,
        static_cast<std::uint16_t>(wire::CoreOperation::Ping), cancelPayload.Take()},
        &correlation) == wire::Error::Ok);
    CHECK(correlation != 0);
    CHECK(client.Cancel(correlation) == wire::Error::Ok);
    CHECK(client.Cancel(correlation) == wire::Error::NotFound);

    gHost.respond = false;
    CHECK(client.Ping(1, [&](wire::Status value, std::span<const std::byte>) { status = value; }) == wire::Error::Ok);
    client.Pump();
    const auto admitted = gHost.submissions;
    gHost.now += 101;
    client.Pump();
    CHECK(status == wire::Status::TimedOut);
    CHECK(gHost.submissions == admitted);
    CHECK(client.IsConnected());
    gHost.respond = true;

    CHECK(client.Ping(7, [&](wire::Status value, std::span<const std::byte>) {
        CHECK(value == wire::Status::Ok);
        client.Shutdown();
    }) == wire::Error::Ok);
    client.Pump();
    CHECK(!client.IsConnected());
}

void TestProtocolDisconnectCanReconnect() {
    guest::Client client;
    CHECK(client.Initialize() == wire::Error::Ok);
    gHost.respond = false;
    wire::Status completed{wire::Status::Ok};
    std::uint32_t correlation{};
    guest::Request request{wire::ServiceId::Core,
        static_cast<std::uint16_t>(wire::CoreOperation::Ping)};
    wire::Encoder payload;
    payload.U64(123);
    request.payload = payload.Take();
    request.callback = [&](wire::Status status, std::span<const std::byte>) {
        completed = status;
    };
    CHECK(client.Send(std::move(request), &correlation) == wire::Error::Ok);
    client.Pump();
    CHECK(gHost.deferred.has_value());
    CHECK(client.Cancel(correlation) == wire::Error::Ok);
    CHECK(!gHost.responses.empty());
    reinterpret_cast<transport::ResponseHeader*>(gHost.responses.front().data())->serviceId =
        static_cast<std::uint16_t>(wire::ServiceId::Logging);
    client.Pump();
    CHECK(!client.IsConnected());
    CHECK(!gHost.open);
    CHECK(completed == wire::Status::ProtocolError);

    gHost.respond = true;
    CHECK(client.Initialize() == wire::Error::Ok);
    client.Shutdown();
}

void TestDispatchBudget() {
    guest::Client client;
    CHECK(client.Initialize() == wire::Error::Ok);
    const auto before = gHost.submissions;
    for (std::uint64_t cookie = 0; cookie < 5; ++cookie)
        CHECK(client.Ping(cookie) == wire::Error::Ok);
    gHost.timeStep = 1'000'000ULL;
    client.Pump();
    CHECK(gHost.submissions - before == 2);
    gHost.timeStep = 0;
    client.Pump();
    CHECK(gHost.submissions - before == 5);
    client.Shutdown();
}

void TestThrowingCallbackDoesNotSkipShutdownCompletion() {
    guest::Client client;
    CHECK(client.Initialize() == wire::Error::Ok);
    std::uint32_t completed{};
    CHECK(client.Ping(1, [](wire::Status, std::span<const std::byte>) {
        throw std::runtime_error("client callback failure");
    }) == wire::Error::Ok);
    CHECK(client.Ping(2, [&](wire::Status status, std::span<const std::byte>) {
        CHECK(status == wire::Status::Disconnected);
        ++completed;
    }) == wire::Error::Ok);
    client.Shutdown();
    CHECK(completed == 1);
}

void TestPointerGuestMethods() {
    guest::Client client;
    CHECK(client.Initialize() == wire::Error::Ok);

    wire::PointerPolicyPayload policy{};
    policy.mode = static_cast<std::uint8_t>(wire::PointerMode::CapturedRelative);
    policy.cursor = static_cast<std::uint8_t>(wire::PointerCursor::Arrow);
    policy.surface = static_cast<std::uint8_t>(wire::PointerSurface::Tv);

    bool setPolicy{};
    CHECK(client.WindowSetPointerPolicy(policy,
        [&](wire::Status status, std::span<const std::byte> payload) {
            CHECK(status == wire::Status::Ok);
            CHECK(payload.size() == sizeof(policy));
            const auto* echoed = reinterpret_cast<const wire::PointerPolicyPayload*>(payload.data());
            CHECK(echoed->mode == policy.mode && echoed->surface == policy.surface);
            setPolicy = true;
        }) == wire::Error::Ok);
    bool gotMouse{};
    CHECK(client.InputGetHostMouse(
        [&](wire::Status status, std::span<const std::byte> payload) {
            CHECK(status == wire::Status::Ok);
            CHECK(payload.empty());
            gotMouse = true;
        }) == wire::Error::Ok);
    bool gotPolicy{};
    CHECK(client.WindowGetPointerPolicy(
        [&](wire::Status status, std::span<const std::byte> payload) {
            CHECK(status == wire::Status::Ok);
            CHECK(payload.empty());
            gotPolicy = true;
        }) == wire::Error::Ok);
    client.Pump();
    CHECK(setPolicy && gotMouse && gotPolicy);
    client.Shutdown();
}

} // namespace

int main() {
    Configure();
    TestCopyTransportAndBounds();
    TestCancelTimeoutAndCallbackShutdown();
    TestProtocolDisconnectCanReconnect();
    TestDispatchBudget();
    TestThrowingCallbackDoesNotSkipShutdownCompletion();
    TestPointerGuestMethods();
    return 0;
}
