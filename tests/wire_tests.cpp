#include "cemuextend/services.hpp"
#include "cemuextend/transport.hpp"

#include <cstdlib>
#include <iostream>

using namespace cemuextend;

namespace {
[[noreturn]] void Fail(const char* expression, int line) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::abort();
}
#define CHECK(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
}

int main() {
    wire::Be16 value16{0x1234};
    wire::Be32 value32{0x12345678};
    wire::Be64 value64{0x0123456789abcdefULL};
    const auto* bytes16 = reinterpret_cast<const std::uint8_t*>(&value16);
    const auto* bytes32 = reinterpret_cast<const std::uint8_t*>(&value32);
    const auto* bytes64 = reinterpret_cast<const std::uint8_t*>(&value64);
    CHECK(bytes16[0] == 0x12 && bytes16[1] == 0x34 && value16.get() == 0x1234);
    CHECK(bytes32[0] == 0x12 && bytes32[3] == 0x78 && value32.get() == 0x12345678);
    CHECK(bytes64[0] == 0x01 && bytes64[7] == 0xef &&
          value64.get() == 0x0123456789abcdefULL);

    wire::Encoder encoder;
    encoder.U8(9);
    encoder.U16(0x1234);
    encoder.U32(0x12345678);
    encoder.U64(0x0123456789abcdefULL);
    CHECK(encoder.String("CemuExtend ABI 2"));
    wire::Decoder decoder(encoder.data());
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};
    std::string text;
    CHECK(decoder.U8(u8) && u8 == 9);
    CHECK(decoder.U16(u16) && u16 == 0x1234);
    CHECK(decoder.U32(u32) && u32 == 0x12345678);
    CHECK(decoder.U64(u64) && u64 == 0x0123456789abcdefULL);
    CHECK(decoder.String(text) && text == "CemuExtend ABI 2");
    CHECK(decoder.remaining() == 0);

    static_assert(transport::kAbiMajor == 2);
    static_assert(transport::kMaximumMessageSize == 64U * 1024U);
    static_assert(sizeof(transport::RequestHeader) == 16);
    static_assert(sizeof(transport::ResponseHeader) == 16);
    return 0;
}
