#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <cstring>

namespace deskproto {
/** Six presets, of which the control box keeps the first four; the panel keeps
 *  5 and 6 and has the desk driven there. */
inline constexpr int kPresetCount = 6;
inline constexpr int kBoxPresets  = 4;

inline constexpr const char *kBetween = "Between";

/** What Home Assistant and anything else off the screen calls them. The screen
 *  has its own names. */
constexpr const char *preset_label(int index)
{
    constexpr const char *LABELS[kPresetCount] = {
        "Preset 1", "Preset 2", "Preset 3", "Preset 4", "Preset 5", "Preset 6",
    };
    return index >= 0 && index < kPresetCount ? LABELS[index] : kBetween;
}

/** How the companion is found and spoken to. Bytes in the order
 *  BLE_UUID128_INIT takes them. */
inline constexpr char kDeviceName[] = "desk-companion";
using Uuid128 = std::array<std::uint8_t, 16>;
inline constexpr Uuid128 kServiceUuid = {
    0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f, 0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x01, 0x00, 0xa5, 0xde};
inline constexpr Uuid128 kEchoUuid = {
    0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f, 0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x02, 0x00, 0xa5, 0xde};
inline constexpr Uuid128 kUpdateUuid = {
    0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f, 0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x03, 0x00, 0xa5, 0xde};

/** The panel repeats a hold this often; the companion lets go of a hold that has
 *  not been repeated for this long. Two repeats may go missing, not three. */
inline constexpr int kHoldPeriodMs  = 100;
inline constexpr int kHoldTimeoutMs = 300;
static_assert(kHoldTimeoutMs >= 2 * kHoldPeriodMs + kHoldPeriodMs / 2,
              "a single late repeat must not stop the desk");

inline constexpr std::uint8_t kMagic0  = 'D';
inline constexpr std::uint8_t kMagic1  = 'K';
inline constexpr std::uint8_t kVersion = 1;

inline constexpr std::size_t kCommandLen = 12;
inline constexpr std::size_t kStatusLen  = 16;

/** Both packets open with the two magic bytes and the version. Numbers are
 *  little-endian. */
inline constexpr std::size_t kMagicAt   = 0;
inline constexpr std::size_t kVersionAt = 2;

namespace command_at {
inline constexpr std::size_t kOp        = 3;
inline constexpr std::size_t kSeq       = 4;
inline constexpr std::size_t kDirection = 8;
inline constexpr std::size_t kPreset    = 9;
inline constexpr std::size_t kHeight    = 10;
}  // namespace command_at

namespace status_at {
inline constexpr std::size_t kFlags  = 3;
inline constexpr std::size_t kSeq    = 4;
inline constexpr std::size_t kHeight = 8;
inline constexpr std::size_t kMotion = 12;
}  // namespace status_at

static_assert(command_at::kHeight + sizeof(std::uint16_t) == kCommandLen);
static_assert(status_at::kMotion < kStatusLen);

inline constexpr std::uint8_t kLinkedFlag  = 1 << 0;
inline constexpr std::uint8_t kHoldingFlag = 1 << 1;
inline constexpr std::uint8_t kDrivingFlag = 1 << 2;

enum class Op : std::uint8_t {
    Hold   = 1,
    Stop   = 2,
    Preset = 3,
    Store  = 4,
    Wake   = 5,
    Ping   = 6,
    GoTo   = 7,
};

enum class Motion : std::uint8_t {
    Idle = 0,
    Up   = 1,
    Down = 2,
};

/** To and from a signed direction, +1 up and -1 down, as the drivers count. */
constexpr int direction_of(Motion motion)
{
    return motion == Motion::Up ? 1 : motion == Motion::Down ? -1 : 0;
}

constexpr Motion motion_of(int direction)
{
    return direction > 0 ? Motion::Up : direction < 0 ? Motion::Down : Motion::Idle;
}

struct Command {
    Op            op        = Op::Ping;
    Motion        direction = Motion::Idle;  // Hold only
    std::uint8_t  preset    = 0;             // Preset and Store only
    std::uint16_t height_mm = 0;             // GoTo only
    std::uint32_t seq       = 0;
};

struct Status {
    std::int32_t  height_mm = -1;     // negative when the desk has not said
    Motion        motion    = Motion::Idle;
    bool          linked    = false;  // the control box is answering
    bool          holding   = false;  // a hold is live, so the deadman is armed
    bool          driving   = false;  // travelling to a height on its own
    std::uint32_t seq       = 0;      // the last command seen, echoed back
};

template <typename Unsigned>
void put_le(std::uint8_t *out, Unsigned value)
{
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (i * CHAR_BIT));
    }
}

template <typename Unsigned>
Unsigned get_le(const std::uint8_t *in)
{
    Unsigned value = 0;
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        value = static_cast<Unsigned>(value | (static_cast<Unsigned>(in[i]) << (i * CHAR_BIT)));
    }
    return value;
}

inline void put_header(std::uint8_t *out)
{
    out[kMagicAt]     = kMagic0;
    out[kMagicAt + 1] = kMagic1;
    out[kVersionAt]   = kVersion;
}

inline bool has_header(const std::uint8_t *in, std::size_t length, std::size_t expected)
{
    return in != nullptr && length >= expected && in[kMagicAt] == kMagic0 &&
           in[kMagicAt + 1] == kMagic1 && in[kVersionAt] == kVersion;
}

inline void encode(const Command &command, std::uint8_t *out)
{
    std::memset(out, 0, kCommandLen);
    put_header(out);
    out[command_at::kOp] = static_cast<std::uint8_t>(command.op);
    put_le<std::uint32_t>(out + command_at::kSeq, command.seq);
    out[command_at::kDirection] = static_cast<std::uint8_t>(command.direction);
    out[command_at::kPreset]    = command.preset;
    put_le<std::uint16_t>(out + command_at::kHeight, command.height_mm);
}

inline bool decode(const std::uint8_t *in, std::size_t length, Command &out)
{
    if (!has_header(in, length, kCommandLen)) {
        return false;
    }
    const std::uint8_t op = in[command_at::kOp];
    if (op < static_cast<std::uint8_t>(Op::Hold) || op > static_cast<std::uint8_t>(Op::GoTo)) {
        return false;
    }
    const std::uint8_t direction = in[command_at::kDirection];
    if (direction > static_cast<std::uint8_t>(Motion::Down)) {
        return false;
    }

    const std::uint8_t preset = in[command_at::kPreset];
    const bool         keyed  = op == static_cast<std::uint8_t>(Op::Preset) ||
                       op == static_cast<std::uint8_t>(Op::Store);
    if (keyed && preset >= kBoxPresets) {
        return false;
    }

    out.op        = static_cast<Op>(op);
    out.seq       = get_le<std::uint32_t>(in + command_at::kSeq);
    out.direction = static_cast<Motion>(direction);
    out.preset    = preset;
    out.height_mm = get_le<std::uint16_t>(in + command_at::kHeight);
    return true;
}

inline void encode(const Status &status, std::uint8_t *out)
{
    std::memset(out, 0, kStatusLen);
    put_header(out);
    out[status_at::kFlags] = static_cast<std::uint8_t>((status.linked ? kLinkedFlag : 0) |
                                                       (status.holding ? kHoldingFlag : 0) |
                                                       (status.driving ? kDrivingFlag : 0));
    put_le<std::uint32_t>(out + status_at::kSeq, status.seq);
    put_le<std::uint32_t>(out + status_at::kHeight, static_cast<std::uint32_t>(status.height_mm));
    out[status_at::kMotion] = static_cast<std::uint8_t>(status.motion);
}

inline bool decode(const std::uint8_t *in, std::size_t length, Status &out)
{
    if (!has_header(in, length, kStatusLen)) {
        return false;
    }
    const std::uint8_t flags  = in[status_at::kFlags];
    const std::uint8_t motion = in[status_at::kMotion];
    out.linked    = (flags & kLinkedFlag) != 0;
    out.holding   = (flags & kHoldingFlag) != 0;
    out.driving   = (flags & kDrivingFlag) != 0;
    out.seq       = get_le<std::uint32_t>(in + status_at::kSeq);
    out.height_mm = static_cast<std::int32_t>(get_le<std::uint32_t>(in + status_at::kHeight));
    out.motion    = motion <= static_cast<std::uint8_t>(Motion::Down) ? static_cast<Motion>(motion)
                                                                      : Motion::Idle;
    return true;
}

// A new firmware for the companion, carried from the panel in pieces written
// to their own characteristic. Each write is answered before the next is sent,
// so a piece is never lost or taken out of order; the answer's ATT error, when
// there is one, says why the companion stopped.
enum class UpdateStep : std::uint8_t {
    Begin   = 1,  // the image's size and CRC-32 follow
    Piece   = 2,  // where it goes in the image, then the bytes
    Finish  = 3,  // check what arrived and boot it
    Abandon = 4,
};

namespace update_at {
inline constexpr std::size_t kStep   = 3;
inline constexpr std::size_t kSize   = 4;  // Begin
inline constexpr std::size_t kCrc    = 8;  // Begin
inline constexpr std::size_t kOffset = 4;  // Piece
inline constexpr std::size_t kData   = 8;  // Piece
}  // namespace update_at

inline constexpr std::size_t kUpdateStepLen  = update_at::kStep + 1;
inline constexpr std::size_t kUpdateBeginLen = update_at::kCrc + sizeof(std::uint32_t);

/** CRC-32 as zip and PNG compute it, carried on from `crc` so an image can be
 *  checked piece by piece; start from zero. Bitwise rather than by table: it
 *  runs once per update, and the companion has little room to spare. */
inline std::uint32_t crc32(std::uint32_t crc, const std::uint8_t *data, std::size_t length)
{
    constexpr std::uint32_t POLYNOMIAL = 0xEDB88320u;  // reflected
    constexpr int           BITS       = 8;
    crc = ~crc;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < BITS; ++bit) {
            crc = (crc >> 1) ^ (POLYNOMIAL & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/** What the companion answers a step with, as application ATT errors. */
inline constexpr std::uint8_t kUpdateBusy       = 0x80;  // the desk is moving
inline constexpr std::uint8_t kUpdateOutOfOrder = 0x81;
inline constexpr std::uint8_t kUpdateBadImage   = 0x82;
inline constexpr std::uint8_t kUpdateFlashFault = 0x83;

struct UpdateMessage {
    UpdateStep          step   = UpdateStep::Abandon;
    std::uint32_t       size   = 0;        // Begin
    std::uint32_t       crc    = 0;        // Begin
    std::uint32_t       offset = 0;        // Piece
    const std::uint8_t *data   = nullptr;  // Piece, pointing into the message
    std::size_t         length = 0;        // Piece
};

/** The step's bytes into out, which holds the most a write may carry; returns
 *  how many, or zero when out is too small. */
inline std::size_t encode(const UpdateMessage &message, std::uint8_t *out, std::size_t capacity)
{
    const std::size_t length = message.step == UpdateStep::Begin   ? kUpdateBeginLen
                               : message.step == UpdateStep::Piece ? update_at::kData + message.length
                                                                   : kUpdateStepLen;
    if (capacity < length) {
        return 0;
    }
    put_header(out);
    out[update_at::kStep] = static_cast<std::uint8_t>(message.step);
    if (message.step == UpdateStep::Begin) {
        put_le<std::uint32_t>(out + update_at::kSize, message.size);
        put_le<std::uint32_t>(out + update_at::kCrc, message.crc);
    } else if (message.step == UpdateStep::Piece) {
        put_le<std::uint32_t>(out + update_at::kOffset, message.offset);
        std::memcpy(out + update_at::kData, message.data, message.length);
    }
    return length;
}

inline bool decode(const std::uint8_t *in, std::size_t length, UpdateMessage &out)
{
    if (!has_header(in, length, kUpdateStepLen)) {
        return false;
    }
    const std::uint8_t step = in[update_at::kStep];
    if (step < static_cast<std::uint8_t>(UpdateStep::Begin) ||
        step > static_cast<std::uint8_t>(UpdateStep::Abandon)) {
        return false;
    }
    out = UpdateMessage{};
    out.step = static_cast<UpdateStep>(step);
    if (out.step == UpdateStep::Begin) {
        if (length < kUpdateBeginLen) {
            return false;
        }
        out.size = get_le<std::uint32_t>(in + update_at::kSize);
        out.crc  = get_le<std::uint32_t>(in + update_at::kCrc);
    } else if (out.step == UpdateStep::Piece) {
        if (length <= update_at::kData) {
            return false;
        }
        out.offset = get_le<std::uint32_t>(in + update_at::kOffset);
        out.data   = in + update_at::kData;
        out.length = length - update_at::kData;
    }
    return true;
}

}  // namespace deskproto
