#pragma once

#include <cstdint>
#include <cstring>

// What the panel and the proxy say to each other. Header only, shared by both,
// so the two ends cannot disagree about the layout.
//
// Not authenticated. The link is point to point over Bluetooth, so reaching it
// means being in the room, and the deadman below means the worst a stray packet
// can do is a third of a second of travel. The tag bytes are reserved so that
// can be added without moving anything else.
namespace deskproto {

inline constexpr std::uint8_t MAGIC0  = 'D';
inline constexpr std::uint8_t MAGIC1  = 'K';
inline constexpr std::uint8_t VERSION = 1;

inline constexpr std::size_t COMMAND_LEN = 12;
inline constexpr std::size_t STATUS_LEN  = 16;

enum class Op : std::uint8_t {
    // Sent repeatedly for as long as the button is held. The proxy stops the
    // desk when they stop arriving, so a panel that crashes, loses the link or
    // runs out of battery stops it too.
    Hold   = 1,
    Stop   = 2,
    Preset = 3,
    Store  = 4,
    Wake   = 5,
    // Costs nothing and moves nothing; used to prove the path.
    Ping   = 6,
};

enum class Motion : std::uint8_t {
    Idle = 0,
    Up   = 1,
    Down = 2,
};

struct Command {
    Op            op        = Op::Ping;
    Motion        direction = Motion::Idle;  // Hold only
    std::uint8_t  preset    = 0;             // Preset and Store only
    std::uint32_t seq       = 0;
};

struct Status {
    std::int32_t  height_mm = -1;     // negative when the desk has not said
    Motion        motion    = Motion::Idle;
    bool          linked    = false;  // the control box is answering
    bool          holding   = false;  // a hold is live, so the deadman is armed
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
}

inline bool decode(const std::uint8_t *in, std::size_t length, Command &out)
{
    if (in == nullptr || length < COMMAND_LEN || in[0] != MAGIC0 || in[1] != MAGIC1 ||
        in[2] != VERSION) {
        return false;
    }
    const std::uint8_t op = in[3];
    if (op < static_cast<std::uint8_t>(Op::Hold) || op > static_cast<std::uint8_t>(Op::Ping)) {
        return false;
    }
    const std::uint8_t direction = in[8];
    if (direction > static_cast<std::uint8_t>(Motion::Down)) {
        return false;
    }

    out.op        = static_cast<Op>(op);
    out.seq       = get32(in + 4);
    out.direction = static_cast<Motion>(direction);
    out.preset    = in[9];
    return true;
}

inline void encode(const Status &status, std::uint8_t *out)
{
    std::memset(out, 0, STATUS_LEN);
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = VERSION;
    out[3] = static_cast<std::uint8_t>((status.linked ? 1 : 0) | (status.holding ? 2 : 0));
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
    out.seq       = get32(in + 4);
    out.height_mm = static_cast<std::int32_t>(get32(in + 8));
    out.motion    = in[12] <= static_cast<std::uint8_t>(Motion::Down)
                        ? static_cast<Motion>(in[12])
                        : Motion::Idle;
    return true;
}

}  // namespace deskproto
