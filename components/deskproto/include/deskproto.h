#pragma once

#include <array>
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
inline constexpr std::array<std::uint8_t, 16> kServiceUuid = {
    0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f, 0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x01, 0x00, 0xa5, 0xde};
inline constexpr std::array<std::uint8_t, 16> kEchoUuid = {
    0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f, 0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x02, 0x00, 0xa5, 0xde};

/** The panel repeats a hold this often; the companion lets go of a hold that has
 *  not been repeated for this long. Two repeats may go missing, not three. */
inline constexpr int kHoldPeriodMs  = 100;
inline constexpr int kHoldTimeoutMs = 300;
static_assert(kHoldTimeoutMs >= 2 * kHoldPeriodMs + kHoldPeriodMs / 2,
              "a single late repeat must not stop the desk");

inline constexpr std::uint8_t MAGIC0  = 'D';
inline constexpr std::uint8_t MAGIC1  = 'K';
inline constexpr std::uint8_t VERSION = 1;

inline constexpr std::size_t COMMAND_LEN = 12;
inline constexpr std::size_t STATUS_LEN  = 16;

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

inline void put32(std::uint8_t *out, std::uint32_t value)
{
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

inline std::uint32_t get32(const std::uint8_t *in)
{
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

inline void encode(const Command &command, std::uint8_t *out)
{
    std::memset(out, 0, COMMAND_LEN);
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = VERSION;
    out[3] = static_cast<std::uint8_t>(command.op);
    put32(out + 4, command.seq);
    out[8] = static_cast<std::uint8_t>(command.direction);
    out[9] = command.preset;
    out[10] = static_cast<std::uint8_t>(command.height_mm);
    out[11] = static_cast<std::uint8_t>(command.height_mm >> 8);
}

inline bool decode(const std::uint8_t *in, std::size_t length, Command &out)
{
    if (in == nullptr || length < COMMAND_LEN || in[0] != MAGIC0 || in[1] != MAGIC1 ||
        in[2] != VERSION) {
        return false;
    }
    const std::uint8_t op = in[3];
    if (op < static_cast<std::uint8_t>(Op::Hold) || op > static_cast<std::uint8_t>(Op::GoTo)) {
        return false;
    }
    const std::uint8_t direction = in[8];
    if (direction > static_cast<std::uint8_t>(Motion::Down)) {
        return false;
    }

    const std::uint8_t preset = in[9];
    const bool         keyed  = op == static_cast<std::uint8_t>(Op::Preset) ||
                       op == static_cast<std::uint8_t>(Op::Store);
    if (keyed && preset >= kBoxPresets) {
        return false;
    }

    out.op        = static_cast<Op>(op);
    out.seq       = get32(in + 4);
    out.direction = static_cast<Motion>(direction);
    out.preset    = preset;
    out.height_mm = static_cast<std::uint16_t>(in[10] | (in[11] << 8));
    return true;
}

inline void encode(const Status &status, std::uint8_t *out)
{
    std::memset(out, 0, STATUS_LEN);
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = VERSION;
    out[3] = static_cast<std::uint8_t>((status.linked ? 1 : 0) | (status.holding ? 2 : 0) |
                                       (status.driving ? 4 : 0));
    put32(out + 4, status.seq);
    put32(out + 8, static_cast<std::uint32_t>(status.height_mm));
    out[12] = static_cast<std::uint8_t>(status.motion);
}

inline bool decode(const std::uint8_t *in, std::size_t length, Status &out)
{
    if (in == nullptr || length < STATUS_LEN || in[0] != MAGIC0 || in[1] != MAGIC1 ||
        in[2] != VERSION) {
        return false;
    }
    out.linked    = (in[3] & 1) != 0;
    out.holding   = (in[3] & 2) != 0;
    out.driving   = (in[3] & 4) != 0;
    out.seq       = get32(in + 4);
    out.height_mm = static_cast<std::int32_t>(get32(in + 8));
    out.motion    = in[12] <= static_cast<std::uint8_t>(Motion::Down)
                        ? static_cast<Motion>(in[12])
                        : Motion::Idle;
    return true;
}

}  // namespace deskproto
