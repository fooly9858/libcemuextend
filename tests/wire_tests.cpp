#include "cemuextend/services.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace cemuextend;

namespace {

std::vector<std::byte> NewRegion() {
    std::vector<std::byte> region(wire::kDefaultRegionSize);
    assert(wire::InitializeDefaultRegion(region) == wire::Error::Ok);
    assert(wire::ValidateLayout(region));
    return region;
}

void TestEndianAndLayout() {
    wire::Be16 value16{0x1234};
    wire::Be32 value32{0x12345678};
    wire::Be64 value64{0x0123456789abcdefULL};
    const auto* bytes16 = reinterpret_cast<const std::uint8_t*>(&value16);
    const auto* bytes32 = reinterpret_cast<const std::uint8_t*>(&value32);
    const auto* bytes64 = reinterpret_cast<const std::uint8_t*>(&value64);
    assert(bytes16[0] == 0x12 && bytes16[1] == 0x34 && value16.get() == 0x1234);
    assert(bytes32[0] == 0x12 && bytes32[3] == 0x78 && value32.get() == 0x12345678);
    assert(bytes64[0] == 0x01 && bytes64[7] == 0xef && value64.get() == 0x0123456789abcdefULL);

    constexpr auto layout = wire::BuildDefaultLayout();
    static_assert(layout.header.offset == 0);
    static_assert(layout.serviceDirectory.offset == 256);
    static_assert(layout.bulk.offset % wire::kAlignment == 0);
    static_assert(layout.bulk.offset + layout.bulk.size <= layout.regionSize);
    auto region = NewRegion();
    const auto& header = *reinterpret_cast<const wire::BridgeHeader*>(region.data());
    assert(header.magic.get() == wire::kMagic);
    assert(header.serviceDirectorySize.get() == wire::kServiceDirectorySize);
}

void TestExtendedRegion() {
    std::vector<std::byte> region(wire::kMaximumRegionSize);
    assert(wire::InitializeDefaultRegion(region) == wire::Error::Ok);
    assert(wire::ValidateLayout(region));
    const auto& header = *reinterpret_cast<const wire::BridgeHeader*>(region.data());
    assert(header.regionSize.get() == wire::kMaximumRegionSize);
}

void TestInvalidLayouts() {
    {
        auto region = NewRegion();
        auto& header = *reinterpret_cast<wire::BridgeHeader*>(region.data());
        header.magic = 0;
        assert(wire::ValidateLayout(region).error == wire::LayoutError::BadMagic);
    }
    {
        auto region = NewRegion();
        auto& header = *reinterpret_cast<wire::BridgeHeader*>(region.data());
        header.guestStateOffset = header.hostStateOffset.get();
        assert(wire::ValidateLayout(region).error == wire::LayoutError::Overlap);
    }
    {
        auto region = NewRegion();
        auto& header = *reinterpret_cast<wire::BridgeHeader*>(region.data());
        header.bulkOffset = 0xffff'ffc0U;
        header.bulkSize = 0x1000U;
        assert(wire::ValidateLayout(region).error == wire::LayoutError::OutOfRange);
    }
    {
        auto region = NewRegion();
        auto& header = *reinterpret_cast<wire::BridgeHeader*>(region.data());
        auto& ring = *reinterpret_cast<wire::RingHeader*>(
            region.data() + header.guestToHostControlOffset.get());
        ring.capacity = ring.capacity.get() - 8;
        assert(wire::ValidateLayout(region).error == wire::LayoutError::BadRing);
    }
}

void TestRing() {
    auto region = NewRegion();
    auto& bridge = *reinterpret_cast<wire::BridgeHeader*>(region.data());
    auto* base = region.data() + bridge.guestToHostControlOffset.get();
    auto& header = *reinterpret_cast<wire::RingHeader*>(base);
    wire::RingView ring(base, bridge.guestToHostControlSize.get());
    wire::MessageHeader message{};
    message.serviceId = static_cast<std::uint16_t>(wire::ServiceId::Core);
    message.operation = static_cast<std::uint16_t>(wire::CoreOperation::Ping);
    message.kind = static_cast<std::uint8_t>(wire::MessageKind::Request);
    message.correlationId = 42;
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
    assert(ring.Push(message, payload));
    wire::MessageView result;
    assert(ring.Pop(result) == wire::RingReadResult::Message);
    assert(result.header.recordSize.get() == 40);
    assert(result.header.payloadSize.get() == 3);
    assert(result.header.correlationId.get() == 42);
    assert(result.payload[2] == std::byte{3});
    std::memset(base + sizeof(wire::RingHeader), 0xcc, 40);
    assert(result.payload[0] == std::byte{1} && result.payload[2] == std::byte{3});
    assert(ring.Pop(result) == wire::RingReadResult::Empty);

    wire::AtomicStore(header.readPosition, 0xffff'fff0U);
    wire::AtomicStore(header.writePosition, 0xffff'fff0U);
    assert(ring.Push(message, payload));
    assert(ring.Pop(result) == wire::RingReadResult::Message);
    assert(result.header.correlationId.get() == 42);

    std::vector<std::byte> large(ring.capacity() - sizeof(wire::MessageHeader));
    wire::AtomicStore(header.readPosition, 0);
    wire::AtomicStore(header.writePosition, 0);
    assert(ring.Push(message, large));
    assert(!ring.Push(message, payload, true));
    assert(wire::AtomicLoad(header.droppedRecords) == 1);

    wire::AtomicStore(header.readPosition, 0);
    wire::AtomicStore(header.writePosition, 40);
    auto* record = reinterpret_cast<wire::MessageHeader*>(base + sizeof(wire::RingHeader));
    std::memset(record, 0, sizeof(*record));
    record->recordSize = 39;
    assert(ring.Pop(result) == wire::RingReadResult::ProtocolError);
    assert(wire::AtomicLoad(header.protocolErrors) == 1);
}

void TestStatePage() {
    auto region = NewRegion();
    auto& bridge = *reinterpret_cast<wire::BridgeHeader*>(region.data());
    auto* base = region.data() + bridge.guestStateOffset.get();
    auto& header = *reinterpret_cast<wire::StatePageHeader*>(base);
    wire::StatePageView page(base, bridge.guestStateSize.get());
    wire::StateValue first{static_cast<std::uint16_t>(wire::ServiceId::GameState), 1, 7,
                           {std::byte{0xaa}, std::byte{0xbb}}};
    wire::StateValue second{static_cast<std::uint16_t>(wire::ServiceId::Input), 3, 1,
                            {std::byte{0x11}}};
    const std::array values{first, second};
    assert(page.Publish(values));
    std::vector<wire::StateValue> snapshot;
    assert(page.Snapshot(snapshot));
    assert(snapshot.size() == 2);
    assert(snapshot[0].version == 7 && snapshot[0].payload[1] == std::byte{0xbb});

    std::array<std::byte, 4> firstOutput{std::byte{0x55}, std::byte{0x55},
                                         std::byte{0x55}, std::byte{0x55}};
    std::array<std::byte, 4> firstScratch{};
    std::array<std::byte, 4> missingOutput{};
    std::array<std::byte, 4> missingScratch{};
    std::array selected{
        wire::StateReadTarget{first.serviceId, first.stateId, firstOutput, firstScratch},
        wire::StateReadTarget{99, 99, missingOutput, missingScratch},
    };
    assert(page.SnapshotSelected(selected));
    assert(selected[0].found && selected[0].version == 7 && selected[0].size == 2);
    assert(firstOutput[0] == std::byte{0xaa} && firstOutput[1] == std::byte{0xbb});
    assert(!selected[1].found);

    wire::AtomicStore(header.sequence, 3);
    assert(!page.Snapshot(snapshot, 2));
    firstOutput.fill(std::byte{0x77});
    assert(!page.SnapshotSelected(selected, 2));
    assert(firstOutput[0] == std::byte{0x77});
    wire::AtomicStore(header.sequence, 4);
    assert(page.Snapshot(snapshot));

    const auto entriesOffset = header.entriesOffset;
    header.entriesOffset = 0xffff'fff0U;
    assert(!page.Publish(values));
    header.entriesOffset = entriesOffset;
}

void TestBulk() {
    auto region = NewRegion();
    auto& bridge = *reinterpret_cast<wire::BridgeHeader*>(region.data());
    wire::BulkAreaView bulk(region.data() + bridge.bulkOffset.get(), bridge.bulkSize.get());
    std::vector<std::byte> input(1024, std::byte{0x5a});
    wire::BulkHandle first{};
    assert(bulk.TryWrite(wire::BulkOwner::Guest, input, first));
    std::vector<std::byte> output;
    assert(!bulk.ReadAndRelease(wire::BulkOwner::Host, first, output));
    assert(output.empty());

    wire::BulkHandle second{};
    assert(bulk.TryWrite(wire::BulkOwner::Guest, input, second));
    auto stale = second;
    stale.generation = second.generation.get() + 1;
    assert(!bulk.ReadAndRelease(wire::BulkOwner::Guest, stale, output));

    wire::BulkHandle third{};
    assert(bulk.TryWrite(wire::BulkOwner::Guest, input, third));
    assert(bulk.ReadAndRelease(wire::BulkOwner::Guest, third, output));
    assert(output == input);

    auto* bulkBase = region.data() + bridge.bulkOffset.get();
    auto& bulkHeader = *reinterpret_cast<wire::BulkAreaHeader*>(bulkBase);
    auto& firstBlock = *reinterpret_cast<wire::BulkBlockHeader*>(bulkBase + sizeof(wire::BulkAreaHeader));
    const auto payloadOffset = firstBlock.payloadOffset;
    firstBlock.payloadOffset = bridge.bulkSize.get() + 64;
    wire::BulkHandle invalid{};
    assert(!bulk.TryWrite(wire::BulkOwner::Host, input, invalid));
    assert(wire::AtomicLoad(firstBlock.state) == static_cast<std::uint32_t>(wire::BulkState::Free));
    firstBlock.payloadOffset = payloadOffset;

    const auto stride = bulkHeader.blockStride;
    bulkHeader.blockStride = 1;
    assert(!bulk.TryWrite(wire::BulkOwner::Host, input, invalid));
    bulkHeader.blockStride = stride;
}

void TestCodec() {
    wire::Encoder encoder;
    encoder.U8(9);
    encoder.U16(0x1234);
    encoder.U32(0x12345678);
    encoder.U64(0x0123456789abcdefULL);
    assert(encoder.String("CemuExtend"));
    wire::Decoder decoder(encoder.data());
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};
    std::string text;
    assert(decoder.U8(u8) && u8 == 9);
    assert(decoder.U16(u16) && u16 == 0x1234);
    assert(decoder.U32(u32) && u32 == 0x12345678);
    assert(decoder.U64(u64) && u64 == 0x0123456789abcdefULL);
    assert(decoder.String(text) && text == "CemuExtend");
    assert(decoder.remaining() == 0);
}

} // namespace

int main() {
    TestEndianAndLayout();
    TestExtendedRegion();
    TestInvalidLayouts();
    TestRing();
    TestStatePage();
    TestBulk();
    TestCodec();
    std::cout << "wire tests passed\n";
}
