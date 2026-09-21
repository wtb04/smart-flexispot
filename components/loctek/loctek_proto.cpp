#include "loctek_proto.h"

namespace loctek {
namespace {

constexpr std::uint8_t kSegBlank = 0x00;
constexpr std::uint8_t kSegPoint = 0x80;

// Common-cathode 7-segment patterns, bit 0 = segment a .. bit 6 = segment g.
constexpr std::array<std::uint8_t, 10> kDigitSegments{
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

// A reading outside this is an error code, not a height.
constexpr int kSaneMinMm = 400;
constexpr int kSaneMaxMm = 2000;

std::optional<int> decode_digit(std::uint8_t segments)
{
    const auto pattern = static_cast<std::uint8_t>(segments & ~kSegPoint);
    for (std::size_t digit = 0; digit < kDigitSegments.size(); ++digit) {
        if (kDigitSegments[digit] == pattern) {
            return static_cast<int>(digit);
        }
    }
    return std::nullopt;
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

    if (len_ == 2) {
        const std::size_t total = static_cast<std::size_t>(byte) + 2;
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

    const std::size_t   total = len_;
    const std::uint16_t want =
        static_cast<std::uint16_t>((buf_[total - 3] << 8) | buf_[total - 2]);
    const bool valid = buf_[total - 1] == kEnd &&
                       crc16(std::span<const std::uint8_t>(buf_).subspan(1, total - 4)) == want;

    reset();
    if (!valid) {
        return std::nullopt;
    }
    return Frame(std::span<const std::uint8_t>(buf_).first(total));
}

std::optional<int> decode_height_mm(const Frame &frame)
{
    if (frame.type() != FrameType::Height || frame.payload().size() < 3) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> digits = frame.payload();

    // Blank display: the box is asleep and has nothing to report.
    if (digits[0] == kSegBlank && digits[1] == kSegBlank && digits[2] == kSegBlank) {
        return std::nullopt;
    }

    int value = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::optional<int> digit = decode_digit(digits[i]);
        if (!digit) {
            // 0x40 is a hyphen ("S-1", shown while programming presets) and
            // anything else unmatched is an error code such as "E01".
            return std::nullopt;
        }
        value = value * 10 + *digit;
    }

    // The decimal point sits on the middle digit: 7 4. 1 is 74.1 cm.
    const int mm = (digits[1] & kSegPoint) ? value : value * 10;
    if (mm < kSaneMinMm || mm > kSaneMaxMm) {
        return std::nullopt;
    }
    return mm;
}

}  // namespace loctek
