#pragma once

#include "cemuextend/wire.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cemuextend::wire {

enum class Status : std::uint16_t {
    Ok = 0,
    InvalidArgument = 1,
    PermissionDenied = 2,
    NotSupported = 3,
    Busy = 4,
    NotFound = 5,
    TooLarge = 6,
    ProtocolError = 7,
    IoError = 8,
    TimedOut = 9,
};

enum class CoreOperation : std::uint16_t {
    GetServices = 1,
    Ping = 2,
    GetVersion = 3,
    Subscribe = 4,
    Unsubscribe = 5,
    GetStatistics = 6,
};

enum class CoreEvent : std::uint16_t {
    ServicesChanged = 1,
    Closing = 2,
};

enum class InputOperation : std::uint16_t {
    InjectGuest = 1,
    InjectMapped = 2,
};

enum class InputEvent : std::uint16_t {
    Keyboard = 1,
    Mouse = 2,
    Controller = 3,
};

enum class InputState : std::uint16_t {
    RawSnapshot = 1,
    WindowSnapshot = 2,
    ObservedVpad0 = 3,
    ObservedVpad1 = 4,
};

enum class LoggingOperation : std::uint16_t { Write = 1 };
enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Critical };

enum class ConfigurationOperation : std::uint16_t {
    Get = 1,
    Set = 2,
    Delete = 3,
    List = 4,
};

enum class ValueType : std::uint8_t {
    Boolean = 1,
    SignedInteger = 2,
    UnsignedInteger = 3,
    Float = 4,
    String = 5,
    Bytes = 6,
};

enum class FileOperation : std::uint16_t {
    Stat = 1,
    List = 2,
    Read = 3,
    Write = 4,
    Mkdir = 5,
    Remove = 6,
    Rename = 7,
};

enum class ClipboardOperation : std::uint16_t { Get = 1, Set = 2 };
enum class WindowOperation : std::uint16_t { Get = 1 };
enum class WindowEvent : std::uint16_t { Changed = 1 };
enum class WindowState : std::uint16_t { Snapshot = 1 };
enum class CaptureOperation : std::uint16_t { Open = 1, Read = 2, Close = 3 };
enum class DiagnosticsOperation : std::uint16_t { Get = 1 };
enum class DiagnosticsState : std::uint16_t { Snapshot = 1 };

enum class InputOrigin : std::uint8_t {
    Physical = 1,
    CemuMapped = 2,
    ClientInjected = 3,
    ObservedVpad = 4,
};

enum class InputChannel : std::uint8_t {
    Keyboard = 1,
    Mouse = 2,
    Controller = 3,
    Vpad0 = 4,
    Vpad1 = 5,
};

struct BeFloat {
    Be32 bits{};
    BeFloat() = default;
    explicit BeFloat(float value) : bits(std::bit_cast<std::uint32_t>(value)) {}
    BeFloat& operator=(float value) noexcept {
        bits = std::bit_cast<std::uint32_t>(value);
        return *this;
    }
    [[nodiscard]] float get() const noexcept { return std::bit_cast<float>(bits.get()); }
};

struct EventIdentity {
    Be64 eventId;
    Be64 parentEventId;
    std::uint8_t origin{};
    std::uint8_t channel{};
    Be16 deviceId;
    Be32 frameNumber;
};

struct KeyboardEventPayload {
    EventIdentity identity;
    Be16 usbHidUsage;
    std::uint8_t pressed{};
    std::uint8_t modifiers{};
};

struct MouseEventPayload {
    EventIdentity identity;
    BeI32 deltaX;
    BeI32 deltaY;
    BeI32 wheelX;
    BeI32 wheelY;
    Be32 buttons;
};

struct ControllerEventPayload {
    EventIdentity identity;
    Be32 buttons;
    BeFloat leftX;
    BeFloat leftY;
    BeFloat rightX;
    BeFloat rightY;
    BeFloat leftTrigger;
    BeFloat rightTrigger;
};

struct ObservedVpadState {
    Be32 frameNumber;
    Be32 sampleError;
    Be32 hold;
    Be32 trigger;
    Be32 release;
    BeFloat leftX;
    BeFloat leftY;
    BeFloat rightX;
    BeFloat rightY;
    BeFloat gyroX;
    BeFloat gyroY;
    BeFloat gyroZ;
    BeFloat touchX;
    BeFloat touchY;
    std::uint8_t touched{};
    std::array<std::byte, 3> reserved{};
};

struct WindowStatePayload {
    Be32 frameNumber;
    Be32 tvWidth;
    Be32 tvHeight;
    Be32 drcWidth;
    Be32 drcHeight;
    BeFloat dpiScale;
    std::uint8_t focused{};
    std::uint8_t fullscreen{};
    std::array<std::byte, 2> reserved{};
};

struct DiagnosticsPayload {
    Be32 hostHeartbeat;
    Be32 guestHeartbeat;
    Be32 guestToHostControlUsed;
    Be32 hostToGuestControlUsed;
    Be32 guestToHostEventUsed;
    Be32 hostToGuestEventUsed;
    Be64 droppedEvents;
    Be64 protocolErrors;
    Be64 requests;
    Be64 responses;
    Be64 bulkBytes;
    BeI32 lastError;
    Be32 reserved{};
};

struct CaptureOpenResponse {
    Be32 handle;
    Be32 width;
    Be32 height;
    Be32 totalBytes;
    Be32 format;
    Be32 chunkSize;
};

struct FileStatPayload {
    Be64 size;
    Be64 modifiedTimeNs;
    Be32 mode;
    std::uint8_t type{};
    std::array<std::byte, 3> reserved{};
};

static_assert(sizeof(BeFloat) == 4);
static_assert(sizeof(EventIdentity) == 24);
static_assert(sizeof(KeyboardEventPayload) == 28);
static_assert(sizeof(MouseEventPayload) == 44);
static_assert(sizeof(ControllerEventPayload) == 52);
static_assert(sizeof(ObservedVpadState) == 60);
static_assert(sizeof(WindowStatePayload) == 28);
static_assert(sizeof(DiagnosticsPayload) == 72);
static_assert(sizeof(CaptureOpenResponse) == 24);
static_assert(sizeof(FileStatPayload) == 24);

class Encoder {
public:
    void U8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void U16(std::uint16_t value) {
        const Be16 encoded{value};
        Append(encoded);
    }
    void U32(std::uint32_t value) {
        const Be32 encoded{value};
        Append(encoded);
    }
    void U64(std::uint64_t value) {
        const Be64 encoded{value};
        Append(encoded);
    }
    void Bytes(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    bool String(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        U32(static_cast<std::uint32_t>(value.size()));
        Bytes({reinterpret_cast<const std::byte*>(value.data()), value.size()});
        return true;
    }
    template <typename T> void Struct(const T& value) { Append(value); }
    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::byte> Take() noexcept { return std::move(bytes_); }

private:
    template <typename T> void Append(const T& value) {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        bytes_.insert(bytes_.end(), begin, begin + sizeof(T));
    }
    std::vector<std::byte> bytes_;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> bytes) : bytes_(bytes) {}

    bool U8(std::uint8_t& value) {
        if (remaining() < 1)
            return false;
        value = std::to_integer<std::uint8_t>(bytes_[cursor_++]);
        return true;
    }
    bool U16(std::uint16_t& value) {
        Be16 encoded{};
        if (!Read(encoded))
            return false;
        value = encoded.get();
        return true;
    }
    bool U32(std::uint32_t& value) {
        Be32 encoded{};
        if (!Read(encoded))
            return false;
        value = encoded.get();
        return true;
    }
    bool U64(std::uint64_t& value) {
        Be64 encoded{};
        if (!Read(encoded))
            return false;
        value = encoded.get();
        return true;
    }
    bool String(std::string& value) {
        std::uint32_t size{};
        if (!U32(size) || size > remaining())
            return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return true;
    }
    template <typename T> bool Struct(T& value) { return Read(value); }
    bool Bytes(std::size_t size, std::span<const std::byte>& value) {
        if (size > remaining())
            return false;
        value = bytes_.subspan(cursor_, size);
        cursor_ += size;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - cursor_; }

private:
    template <typename T> bool Read(T& value) {
        if (remaining() < sizeof(T))
            return false;
        std::memcpy(&value, bytes_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return true;
    }
    std::span<const std::byte> bytes_;
    std::size_t cursor_{};
};

} // namespace cemuextend::wire
