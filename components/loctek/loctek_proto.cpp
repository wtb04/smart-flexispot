#include "loctek_proto.h"

#include "units.h"

#include <algorithm>

namespace loctek {
namespace {
constexpr std::uint8_t SEG_BLANK = 0x00;
constexpr std::uint8_t SEG_POINT = 0x80;

constexpr std::array<std::uint8_t, 10> DIGIT_SEGMENTS{
    0x3f,
    0x06,
    0x5b,
    0x4f,
    0x66,
    0x6d,
    0x7d,
    0x07,
    0x7f,
    0x6f,
};

constexpr std::size_t DISPLAY_DIGITS = 3;
constexpr std::size_t POINT_DIGIT    = 1;
constexpr int         DECIMAL_BASE   = 10;

constexpr int SANE_MIN_MM = 400;
constexpr int SANE_MAX_MM = 2000;

std::optional<int> decode_digit(std::uint8_t segments)
{
    const auto pattern = static_cast<std::uint8_t>(segments & ~SEG_POINT);
    for (std::size_t digit = 0; digit < DIGIT_SEGMENTS.size(); ++digit) {
        if (DIGIT_SEGMENTS[digit] == pattern) {
            return static_cast<int>(digit);
        }
    }
    return std::nullopt;
}

std::optional<int> decode_number(std::span<const std::uint8_t> digits)
{
    int value = 0;
    for (std::uint8_t segments : digits) {
        const std::optional<int> digit = decode_digit(segments);
        if (!digit) {
            return std::nullopt;
        }
        value = value * DECIMAL_BASE + *digit;
    }
    return value;
}

}  // namespace

std::optional<Frame> Parser::push(std::uint8_t byte)
{
    if (len_ == 0) {
        if (byte == kStart) {
            buf_[len_++] = byte;
        }
        return std::nullopt;
    }

    buf_[len_++] = byte;

    if (len_ == kLengthAt + 1) {
        const std::size_t total = static_cast<std::size_t>(byte) + kUncountedBytes;
        if (total < kMinFrame || total > kMaxFrame) {
            reset();
        } else {
            expected_ = static_cast<std::uint8_t>(total);
        }
        return std::nullopt;
    }

    if (len_ < expected_) {
        return std::nullopt;
    }

    const std::span<const std::uint8_t> frame  = std::span<const std::uint8_t>(buf_).first(len_);
    const std::size_t                   crc_at = crc_offset(frame.size());

    const auto want  = static_cast<std::uint16_t>((frame[crc_at] << CHAR_BIT) | frame[crc_at + 1]);
    const bool valid = frame.back() == kEnd && crc16(crc_covered(frame)) == want;

    reset();
    if (!valid) {
        return std::nullopt;
    }
    return Frame(frame);
}

std::optional<int> decode_height_mm(const Frame &frame)
{
    if (frame.type() != FrameType::Height || frame.payload().size() < DISPLAY_DIGITS) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> digits = frame.payload().first(DISPLAY_DIGITS);

    if (std::all_of(digits.begin(), digits.end(), [](std::uint8_t d) { return d == SEG_BLANK; })) {
        return std::nullopt;
    }

    const std::optional<int> value = decode_number(digits);
    if (!value) {
        return std::nullopt;
    }

    const bool shows_tenths = (digits[POINT_DIGIT] & SEG_POINT) != 0;
    const int  mm           = shows_tenths ? *value : *value * units::kMmPerCm;
    if (mm < SANE_MIN_MM || mm > SANE_MAX_MM) {
        return std::nullopt;
    }
    return mm;
}

}  // namespace loctek
