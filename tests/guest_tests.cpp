#include "cemuextend/guest.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

using namespace cemuextend;

namespace {

struct MockHost {
    std::span<std::byte> region{};
    wire::BridgeHeader* header{};
    std::uint64_t nowNs{1'000'000'000ULL};
    std::uint32_t session{0x1234};
    std::uint32_t requests{};
    std::uint32_t guestEvents{};
    bool emitWindowEvent{};
} gHost;

std::int32_t Query(std::uint32_t query, void* output, std::uint32_t size) {
    if (query != static_cast<std::uint32_t>(wire::Query::BridgeInfo) || size != sizeof(wire::BridgeInfo))
        return static_cast<std::int32_t>(wire::Error::InvalidArgument);
    auto& info = *static_cast<wire::BridgeInfo*>(output);
    info.available = 1;
    info.minimumAbiMajor = wire::kAbiMajor;
    info.minimumAbiMinor = 0;
    info.maximumAbiMajor = wire::kAbiMajor;
    info.maximumAbiMinor = wire::kAbiMinor;
    info.maximumRegionSize = wire::kMaximumRegionSize;
    info.hostBuildId = 0x1122334455667788ULL;
    info.features = static_cast<std::uint64_t>(wire::Feature::SharedMemory) |
                    static_cast<std::uint64_t>(wire::Feature::Bulk);
    info.maximumServices = 64;
    return 0;
}

std::int32_t Register(std::uint32_t abi, void* region, std::uint32_t size, std::uint32_t* session) {
    if (abi != ((static_cast<std::uint32_t>(wire::kAbiMajor) << 16U) | wire::kAbiMinor))
        return static_cast<std::int32_t>(wire::Error::AbiMismatch);
    gHost.region = {static_cast<std::byte*>(region), size};
    gHost.header = reinterpret_cast<wire::BridgeHeader*>(region);
    gHost.header->hostBuildId = 0x1122334455667788ULL;
    gHost.header->sessionId = gHost.session;
    wire::AtomicStore(gHost.header->connectionState,
                      static_cast<std::uint32_t>(wire::ConnectionState::Connected));
    *session = gHost.session;
    return 0;
}

void PushResponse(const wire::MessageView& request) {
    wire::RingView responses(gHost.region.data() + gHost.header->hostToGuestControlOffset.get(),
                             gHost.header->hostToGuestControlSize.get());
    wire::MessageHeader response{};
    response.serviceId = request.header.serviceId.get();
    response.operation = request.header.operation.get();
    response.kind = static_cast<std::uint8_t>(wire::MessageKind::Response);
    response.status = static_cast<std::uint16_t>(wire::Status::Ok);
    response.correlationId = request.header.correlationId.get();
    response.timestampNs = gHost.nowNs;
    assert(responses.Push(response, request.payload));
}

std::int32_t Notify(std::uint32_t session, std::uint32_t) {
    assert(session == gHost.session);
    wire::AtomicFetchAdd(gHost.header->hostHeartbeat, 1);
    wire::RingView requests(gHost.region.data() + gHost.header->guestToHostControlOffset.get(),
                            gHost.header->guestToHostControlSize.get());
    for (;;) {
        wire::MessageView request;
        const auto result = requests.Pop(request);
        if (result == wire::RingReadResult::Empty)
            break;
        assert(result == wire::RingReadResult::Message);
        ++gHost.requests;
        if (request.header.flags & static_cast<std::uint8_t>(wire::MessageFlag::HasBulk)) {
            wire::BulkHandle handle{};
            assert(request.payload.size() >= sizeof(handle));
            std::memcpy(&handle, request.payload.data(), sizeof(handle));
            wire::BulkAreaView bulk(gHost.region.data() + gHost.header->bulkOffset.get(),
                                    gHost.header->bulkSize.get());
            std::vector<std::byte> bytes;
            assert(bulk.ReadAndRelease(wire::BulkOwner::Guest, handle, bytes));
        }
        PushResponse(request);
    }
    wire::RingView guestEvents(gHost.region.data() + gHost.header->guestToHostEventOffset.get(),
                               gHost.header->guestToHostEventSize.get());
    for (;;) {
        wire::MessageView event;
        const auto result = guestEvents.Pop(event);
        if (result == wire::RingReadResult::Empty)
            break;
        assert(result == wire::RingReadResult::Message);
        ++gHost.guestEvents;
    }
    if (gHost.emitWindowEvent) {
        wire::RingView events(gHost.region.data() + gHost.header->hostToGuestEventOffset.get(),
                              gHost.header->hostToGuestEventSize.get());
        wire::MessageHeader event{};
        event.serviceId = static_cast<std::uint16_t>(wire::ServiceId::Window);
        event.operation = static_cast<std::uint16_t>(wire::WindowEvent::Changed);
        event.kind = static_cast<std::uint8_t>(wire::MessageKind::Event);
        assert(events.Push(event, {}));
        gHost.emitWindowEvent = false;
    }
    return 0;
}

std::int32_t Unregister(std::uint32_t session) {
    assert(session == gHost.session);
    gHost.region = {};
    gHost.header = nullptr;
    return 0;
}

std::int32_t Acquire(const char*, std::uint32_t* module) {
    *module = 1;
    return 0;
}

std::int32_t FindExport(std::uint32_t, bool, const char* name, void** address) {
    if (std::string_view(name) == "CEXQuery")
        *address = reinterpret_cast<void*>(&Query);
    else if (std::string_view(name) == "CEXRegister")
        *address = reinterpret_cast<void*>(&Register);
    else if (std::string_view(name) == "CEXNotify")
        *address = reinterpret_cast<void*>(&Notify);
    else if (std::string_view(name) == "CEXUnregister")
        *address = reinterpret_cast<void*>(&Unregister);
    else
        return -1;
    return 0;
}

void* Allocate(std::size_t alignment, std::size_t size) {
    void* result{};
    return posix_memalign(&result, alignment, size) == 0 ? result : nullptr;
}

std::uint64_t Time() { return gHost.nowNs; }

void Configure() {
    const guest::PlatformCallbacks callbacks{Acquire, FindExport, Allocate, std::free, Time};
    assert(guest::ConfigurePlatform(callbacks));
}

void TestClient() {
    Configure();
    guest::Client client;
    assert(client.Initialize() == wire::Error::Ok);
    assert(client.IsConnected());
    assert(client.HostVersion().buildId == 0x1122334455667788ULL);

    const std::array hostStateBytes{std::byte{0x31}, std::byte{0x32}};
    wire::StatePageView hostStatePage(
        gHost.region.data() + gHost.header->hostStateOffset.get(), gHost.header->hostStateSize.get());
    const wire::StateValue hostState{static_cast<std::uint16_t>(wire::ServiceId::Input),
                                     static_cast<std::uint16_t>(wire::InputState::RawSnapshot), 3,
                                     {hostStateBytes.begin(), hostStateBytes.end()}};
    assert(hostStatePage.Publish(std::span<const wire::StateValue>(&hostState, 1)));
    wire::StateValue readState;
    assert(client.ReadHostState(hostState.serviceId, hostState.stateId, readState) == wire::Error::Ok);
    assert(readState.version == 3 && readState.payload == hostState.payload);

    std::array<std::byte, 8> selectedOutput{};
    std::array<std::byte, 8> selectedScratch{};
    std::array selected{wire::StateReadTarget{hostState.serviceId, hostState.stateId,
                                               selectedOutput, selectedScratch}};
    assert(client.ReadHostStates(selected) == wire::Error::Ok);
    assert(selected[0].found && selected[0].version == 3 && selected[0].size == 2);
    assert(selectedOutput[0] == std::byte{0x31} && selectedOutput[1] == std::byte{0x32});

    bool pinged{};
    assert(client.Ping(0xfeedfaceULL, [&](wire::Status status, std::span<const std::byte> payload) {
        assert(status == wire::Status::Ok);
        wire::Decoder decoder(payload);
        std::uint64_t cookie{};
        assert(decoder.U64(cookie) && cookie == 0xfeedfaceULL);
        pinged = true;
    }) == wire::Error::Ok);
    client.Pump();
    assert(pinged);

    bool eventReceived{};
    assert(client.Subscribe(wire::ServiceId::Window,
                            [&](std::uint16_t operation, std::span<const std::byte>) {
                                assert(operation == static_cast<std::uint16_t>(wire::WindowEvent::Changed));
                                eventReceived = true;
                            }) == wire::Error::Ok);
    gHost.emitWindowEvent = true;
    client.Pump();
    assert(eventReceived);

    const std::array state{std::byte{1}, std::byte{2}};
    assert(client.PublishState(static_cast<std::uint16_t>(wire::ServiceId::GameState), 1, 1, state) ==
           wire::Error::Ok);
    assert(client.PublishEvent(static_cast<std::uint16_t>(wire::ServiceId::GameState), 2, state) ==
           wire::Error::Ok);
    client.Pump();
    assert(gHost.guestEvents == 1);
    auto& header = *gHost.header;
    wire::StatePageView page(gHost.region.data() + header.guestStateOffset.get(),
                             header.guestStateSize.get());
    std::vector<wire::StateValue> states;
    assert(page.Snapshot(states) && states.size() == 1 && states[0].payload == std::vector(state.begin(), state.end()));

    std::uint32_t callbacks{};
    const auto count = [&](wire::Status status, std::span<const std::byte>) {
        assert(status == wire::Status::Ok);
        ++callbacks;
    };
    const std::vector<std::byte> fileBytes(4096, std::byte{0x44});
    assert(client.Log(wire::LogLevel::Info, "hello", count) == wire::Error::Ok);
    assert(client.GetServices(count) == wire::Error::Ok);
    assert(client.GetVersion(count) == wire::Error::Ok);
    assert(client.GetHostStatistics(count) == wire::Error::Ok);
    assert(client.InputInjectGuest(state, count) == wire::Error::Ok);
    wire::ObservedVpadState mapped{};
    assert(client.InputInjectMapped(0, mapped, count) == wire::Error::Ok);
    assert(client.ConfigurationGet("key", count) == wire::Error::Ok);
    assert(client.ConfigurationSet("key", wire::ValueType::Bytes, state, count) == wire::Error::Ok);
    assert(client.ConfigurationDelete("key", count) == wire::Error::Ok);
    assert(client.ConfigurationList("", count) == wire::Error::Ok);
    assert(client.FileStat("save.dat", count) == wire::Error::Ok);
    assert(client.FileList(".", count) == wire::Error::Ok);
    assert(client.FileRead("save.dat", 0, 128, count) == wire::Error::Ok);
    assert(client.FileWrite("save.dat", 0, fileBytes, count) == wire::Error::Ok);
    assert(client.FileMkdir("folder", count) == wire::Error::Ok);
    assert(client.FileRemove("folder", count) == wire::Error::Ok);
    assert(client.FileRename("a", "b", count) == wire::Error::Ok);
    assert(client.ClipboardGet(count) == wire::Error::Ok);
    assert(client.ClipboardSet("clipboard", count) == wire::Error::Ok);
    assert(client.WindowGet(count) == wire::Error::Ok);
    assert(client.CaptureOpen(false, count) == wire::Error::Ok);
    assert(client.CaptureRead(1, 0, count) == wire::Error::Ok);
    assert(client.CaptureClose(1, count) == wire::Error::Ok);
    assert(client.DiagnosticsGet(count) == wire::Error::Ok);
    client.Pump();
    assert(callbacks == 24);

    auto statistics = client.GetStatistics();
    assert(statistics.responsesReceived >= 21);
    assert(statistics.statePublishes == 1);
    assert(statistics.bulkBytes >= fileBytes.size());

    gHost.nowNs += 6'000'000'000ULL;
    client.Pump();
    gHost.nowNs += 6'000'000'000ULL;
    client.Pump();
    assert(!client.IsConnected());
    client.Shutdown();
    assert(client.Initialize() == wire::Error::Ok);
    client.Shutdown();
}

void TestQueueBound() {
    guest::Client client;
    guest::InitializeOptions options;
    options.maximumQueuedRequests = 2;
    assert(client.Initialize(options) == wire::Error::Ok);
    assert(client.Ping(1) == wire::Error::Ok);
    assert(client.Ping(2) == wire::Error::Ok);
    assert(client.Ping(3) == wire::Error::Busy);
    assert(client.GetStatistics().queueOverflows == 1);
    client.Shutdown();
}

} // namespace

int main() {
    TestClient();
    TestQueueBound();
    std::cout << "guest tests passed\n";
}
