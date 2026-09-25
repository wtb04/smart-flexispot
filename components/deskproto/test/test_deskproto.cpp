#include "deskproto.h"

#include <cstdio>
#include <cstring>

namespace {
using namespace deskproto;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

void test_command_round_trip()
{
    Command in{};
    in.op        = Op::GoTo;
    in.height_mm = 1234;
    in.seq       = 0x01020304;

    std::uint8_t packet[kCommandLen];
    encode(in, packet);
    Command out{};
    check(decode(packet, sizeof(packet), out), "goto decodes");
    check(out.op == Op::GoTo && out.height_mm == 1234 && out.seq == 0x01020304,
          "goto carries height and sequence");
    check(out.direction == Motion::Idle && out.preset == 0, "goto leaves the other fields idle");

    in           = Command{};
    in.op        = Op::Hold;
    in.direction = Motion::Down;
    in.seq       = 7;
    encode(in, packet);
    check(decode(packet, sizeof(packet), out) && out.op == Op::Hold &&
              out.direction == Motion::Down && out.height_mm == 0,
          "hold round trip");
}

void test_status_round_trip()
{
    Status in{};
    in.height_mm = 1123;
    in.motion    = Motion::Up;
    in.linked    = true;
    in.holding   = false;
    in.driving   = true;
    in.seq       = 99;

    std::uint8_t packet[kStatusLen];
    encode(in, packet);
    Status out{};
    check(decode(packet, sizeof(packet), out), "status decodes");
    check(out.height_mm == 1123 && out.motion == Motion::Up && out.seq == 99, "status fields");
    check(out.linked && !out.holding && out.driving, "status flags kept apart");

    in.height_mm = -1;
    encode(in, packet);
    check(decode(packet, sizeof(packet), out) && out.height_mm == -1, "unknown height survives");
}

// Pinned byte for byte, so a change to the layout cannot slip through on both
// sides of the link at once.
void test_wire_layout()
{
    Command command{};
    command.op        = Op::GoTo;
    command.height_mm = 1234;
    command.seq       = 0x01020304;
    std::uint8_t packet[kCommandLen];
    encode(command, packet);
    const std::uint8_t command_bytes[] = {'D', 'K', 1, 7, 0x04, 0x03, 0x02, 0x01, 0, 0, 0xd2, 0x04};
    check(sizeof(command_bytes) == kCommandLen &&
              std::memcmp(packet, command_bytes, sizeof(command_bytes)) == 0,
          "command bytes on the wire");

    Status status{};
    status.height_mm = 1123;
    status.motion    = Motion::Up;
    status.linked    = true;
    status.driving   = true;
    status.seq       = 99;
    std::uint8_t status_packet[kStatusLen];
    encode(status, status_packet);
    const std::uint8_t status_bytes[] = {'D', 'K', 1, 0x05, 99, 0, 0, 0,
                                         0x63, 0x04, 0, 0, 1, 0, 0, 0};
    check(sizeof(status_bytes) == kStatusLen &&
              std::memcmp(status_packet, status_bytes, sizeof(status_bytes)) == 0,
          "status bytes on the wire");
}

void test_rejects()
{
    Command command{};
    command.op = Op::Ping;
    std::uint8_t packet[kCommandLen];
    encode(command, packet);
    Command out{};

    check(!decode(packet, kCommandLen - 1, out), "short command rejected");
    packet[command_at::kOp] = 0;
    check(!decode(packet, kCommandLen, out), "op below range rejected");
    packet[command_at::kOp] = static_cast<std::uint8_t>(Op::GoTo) + 1;
    check(!decode(packet, kCommandLen, out), "op above range rejected");
    packet[command_at::kOp] = static_cast<std::uint8_t>(Op::Hold);
    packet[command_at::kDirection] = static_cast<std::uint8_t>(Motion::Down) + 1;
    check(!decode(packet, kCommandLen, out), "bad direction rejected");
    packet[command_at::kDirection] = 0;
    packet[command_at::kOp] = static_cast<std::uint8_t>(Op::Preset);
    packet[command_at::kPreset] = kBoxPresets;
    check(!decode(packet, kCommandLen, out), "preset the box does not have rejected");
    packet[command_at::kPreset] = kBoxPresets - 1;
    check(decode(packet, kCommandLen, out) && out.preset == kBoxPresets - 1, "last box preset accepted");
    packet[command_at::kOp] = static_cast<std::uint8_t>(Op::GoTo);
    packet[command_at::kPreset] = 200;
    check(decode(packet, kCommandLen, out), "preset byte ignored for other ops");
    packet[command_at::kPreset] = 0;
    packet[kMagicAt] = 'X';
    check(!decode(packet, kCommandLen, out), "bad magic rejected");

    Status status{};
    std::uint8_t status_packet[kStatusLen];
    encode(status, status_packet);
    status_packet[status_at::kMotion] = 7;
    Status decoded{};
    check(decode(status_packet, kStatusLen, decoded) && decoded.motion == Motion::Idle,
          "unknown motion reads as idle");
}

void test_vocabulary()
{
    check(std::strcmp(preset_label(0), "Preset 1") == 0 &&
              std::strcmp(preset_label(kPresetCount - 1), "Preset 6") == 0,
          "presets are labelled 1 to 6");
    check(std::strcmp(preset_label(-1), kBetween) == 0 &&
              std::strcmp(preset_label(kPresetCount), kBetween) == 0,
          "anything else is between");
    check(direction_of(Motion::Up) == 1 && direction_of(Motion::Down) == -1 &&
              direction_of(Motion::Idle) == 0,
          "motion to direction");
    check(motion_of(1) == Motion::Up && motion_of(-1) == Motion::Down && motion_of(0) == Motion::Idle,
          "direction to motion");
}

void test_update_messages()
{
    std::uint8_t out[64] = {};

    UpdateMessage begin;
    begin.step = UpdateStep::Begin;
    begin.size = 484160;
    begin.crc  = 0xdeadbeef;
    const std::size_t begin_len = encode(begin, out, sizeof(out));
    UpdateMessage     back;
    check(begin_len == kUpdateBeginLen && decode(out, begin_len, back) &&
              back.step == UpdateStep::Begin && back.size == 484160 && back.crc == 0xdeadbeef,
          "begin round trip");

    const std::uint8_t bytes[5] = {1, 2, 3, 4, 5};
    UpdateMessage      piece;
    piece.step   = UpdateStep::Piece;
    piece.offset = 4096;
    piece.data   = bytes;
    piece.length = sizeof(bytes);
    const std::size_t piece_len = encode(piece, out, sizeof(out));
    check(piece_len == update_at::kData + sizeof(bytes) && decode(out, piece_len, back) &&
              back.step == UpdateStep::Piece && back.offset == 4096 && back.length == 5 &&
              std::memcmp(back.data, bytes, 5) == 0,
          "piece round trip");

    check(encode(piece, out, update_at::kData + 4) == 0, "a piece too big for the write is refused");

    UpdateMessage finish;
    finish.step = UpdateStep::Finish;
    check(encode(finish, out, sizeof(out)) == kUpdateStepLen &&
              decode(out, kUpdateStepLen, back) && back.step == UpdateStep::Finish,
          "finish round trip");

    const char *check_value = "123456789";
    const auto *digits      = reinterpret_cast<const std::uint8_t *>(check_value);
    check(crc32(0, digits, 9) == 0xCBF43926u, "crc32 of the standard check value");
    check(crc32(crc32(0, digits, 4), digits + 4, 5) == 0xCBF43926u, "crc32 carried on in pieces");

    out[update_at::kStep] = 9;
    check(!decode(out, kUpdateStepLen, back), "an unknown step is refused");
    encode(begin, out, sizeof(out));
    check(!decode(out, kUpdateBeginLen - 1, back), "a short begin is refused");
    encode(piece, out, sizeof(out));
    check(!decode(out, update_at::kData, back), "an empty piece is refused");
}

static_assert(kBoxPresets < kPresetCount);
static_assert(kServiceUuid != kEchoUuid);
static_assert(kUpdateUuid != kEchoUuid && kUpdateUuid != kServiceUuid);

}  // namespace

int main()
{
    test_vocabulary();
    test_command_round_trip();
    test_status_round_trip();
    test_wire_layout();
    test_rejects();
    test_update_messages();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
