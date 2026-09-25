#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <span>

namespace loctek {
/** A frame is the start byte, a length, a type, the payload, a CRC high byte
 *  first, and the end byte. The length counts every byte after itself, and the
 *  CRC covers the length, the type and the payload. */
inline constexpr std::uint8_t kStart = 0x9b;
inline constexpr std::uint8_t kEnd   = 0x9d;

inline constexpr std::size_t kLengthAt       = 1;
inline constexpr std::size_t kTypeAt         = 2;
inline constexpr std::size_t kPayloadAt      = 3;
inline constexpr std::size_t kUncountedBytes = kLengthAt + 1;
inline constexpr std::size_t kCrcBytes       = 2;
inline constexpr std::size_t kTrailerBytes   = kCrcBytes + 1;

inline constexpr std::size_t kMaxFrame = 16;
inline constexpr std::size_t kMinFrame = kPayloadAt + kTrailerBytes;

enum class FrameType : std::uint8_t {
    Key       = 0x02,
    Heartbeat = 0x11,
    Height    = 0x12,
};

/** Key bitmask carried by a Key frame. */
enum class Key : std::uint16_t {
    None    = 0x0000,  // also "wake screen" and "button released"
    Up      = 0x0001,
    Down    = 0x0002,
    Preset1 = 0x0004,
    Preset2 = 0x0008,
    Stand   = 0x0010,
    Memory  = 0x0020,
    Alarm   = 0x0040,
    Sit     = 0x0100,
};

/** In the order the handset labels them. */
enum class Preset : std::uint8_t { One, Two, Three, Four };

constexpr Key key_for(Preset preset)
{
    switch (preset) {
        case Preset::One:   return Key::Preset1;
        case Preset::Two:   return Key::Preset2;
        case Preset::Three: return Key::Stand;
        case Preset::Four:  return Key::Sit;
    }
    return Key::None;
}

/** A Key frame's payload is the key bitmask, low byte first. */
using KeyFrame = std::array<std::uint8_t, kMinFrame + sizeof(Key)>;

enum class Move : std::int8_t {
    Stop = 0,
    Up   = 1,
    Down = -1,
};

/** The key to send next while travelling to a height, given the height just
 *  reported and the key being sent. Stops `stop_early_mm` short, since the desk
 *  runs on for a few millimetres after the last frame, and stops rather than
 *  reverses once the target has been passed. */
constexpr Move steer(int target_mm, int height_mm, Move current, int stop_early_mm)
{
    const int away = target_mm - height_mm;
    if (away <= stop_early_mm && away >= -stop_early_mm) {
        return Move::Stop;
    }
    if ((current == Move::Up && away < 0) || (current == Move::Down && away > 0)) {
        return Move::Stop;
    }
    return away > 0 ? Move::Up : Move::Down;
}

constexpr std::uint8_t low_byte(std::uint16_t value)
{
    return static_cast<std::uint8_t>(value);
}

constexpr std::uint8_t high_byte(std::uint16_t value)
{
    return static_cast<std::uint8_t>(value >> CHAR_BIT);
}

/** CRC-16/MODBUS. */
inline constexpr std::uint16_t kCrcInitial    = 0xffff;
inline constexpr std::uint16_t kCrcPolynomial = 0xa001;  // 0x8005, bit-reversed

constexpr std::uint16_t crc16(std::span<const std::uint8_t> data)
{
    std::uint16_t crc = kCrcInitial;
    for (std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < CHAR_BIT; ++bit) {
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ kCrcPolynomial)
                            : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

constexpr std::size_t crc_offset(std::size_t frame_size)
{
    return frame_size - kTrailerBytes;
}

constexpr std::span<const std::uint8_t> crc_covered(std::span<const std::uint8_t> frame)
{
    return frame.subspan(kLengthAt, crc_offset(frame.size()) - kLengthAt);
}

constexpr KeyFrame build_key_frame(Key key)
{
    const auto mask = static_cast<std::uint16_t>(key);
    KeyFrame   frame{};
    frame.front()         = kStart;
    frame[kLengthAt]      = static_cast<std::uint8_t>(frame.size() - kUncountedBytes);
    frame[kTypeAt]        = static_cast<std::uint8_t>(FrameType::Key);
    frame[kPayloadAt]     = low_byte(mask);
    frame[kPayloadAt + 1] = high_byte(mask);
    frame.back()          = kEnd;

    const std::uint16_t crc = crc16(crc_covered(frame));
    const std::size_t   at  = crc_offset(frame.size());
    frame[at]               = high_byte(crc);
    frame[at + 1]           = low_byte(crc);
    return frame;
}

class Frame {
public:
    Frame(std::span<const std::uint8_t> bytes)
    {
        for (std::size_t i = 0; i < bytes.size() && i < kMaxFrame; ++i) {
            bytes_[i] = bytes[i];
        }
        len_ = static_cast<std::uint8_t>(bytes.size());
    }

    FrameType type() const { return static_cast<FrameType>(bytes_[kTypeAt]); }
    std::size_t size() const { return len_; }

    std::span<const std::uint8_t> payload() const
    {
        return std::span<const std::uint8_t>(bytes_).subspan(kPayloadAt, len_ - kMinFrame);
    }

private:
    std::array<std::uint8_t, kMaxFrame> bytes_{};
    std::uint8_t                        len_ = 0;
};

/** Frames arrive back to back in a single UART read, so this consumes one byte
 *  at a time rather than parsing a read buffer. */
class Parser {
public:
    std::optional<Frame> push(std::uint8_t byte);
    void reset() { len_ = 0; expected_ = 0; }

private:
    std::array<std::uint8_t, kMaxFrame> buf_{};
    std::uint8_t                        len_      = 0;
    std::uint8_t                        expected_ = 0;
};

/** The payload is three 7-segment patterns driving the panel display, with bit 7
 *  of the middle digit as the decimal point. Nothing for a blank display (box
 *  asleep), the "S-1" hyphen, or an error code such as "E01". */
std::optional<int> decode_height_mm(const Frame &frame);

}  // namespace loctek
