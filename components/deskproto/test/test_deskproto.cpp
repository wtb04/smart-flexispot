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

    std::uint8_t packet[COMMAND_LEN];
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

    std::uint8_t packet[STATUS_LEN];
    encode(in, packet);
    Status out{};
    check(decode(packet, sizeof(packet), out), "status decodes");
    check(out.height_mm == 1123 && out.motion == Motion::Up && out.seq == 99, "status fields");
    check(out.linked && !out.holding && out.driving, "status flags kept apart");

    in.height_mm = -1;
    encode(in, packet);
    check(decode(packet, sizeof(packet), out) && out.height_mm == -1, "unknown height survives");
}

void test_rejects()
{
    Command command{};
    command.op = Op::Ping;
    std::uint8_t packet[COMMAND_LEN];
    encode(command, packet);
    Command out{};

    check(!decode(packet, COMMAND_LEN - 1, out), "short command rejected");
    packet[3] = 0;
    check(!decode(packet, COMMAND_LEN, out), "op below range rejected");
    packet[3] = static_cast<std::uint8_t>(Op::GoTo) + 1;
    check(!decode(packet, COMMAND_LEN, out), "op above range rejected");
    packet[3] = static_cast<std::uint8_t>(Op::Hold);
    packet[8] = 3;
    check(!decode(packet, COMMAND_LEN, out), "bad direction rejected");
    packet[8] = 0;
    packet[3] = static_cast<std::uint8_t>(Op::Preset);
    packet[9] = kBoxPresets;
    check(!decode(packet, COMMAND_LEN, out), "preset the box does not have rejected");
    packet[9] = kBoxPresets - 1;
    check(decode(packet, COMMAND_LEN, out) && out.preset == kBoxPresets - 1, "last box preset accepted");
    packet[3] = static_cast<std::uint8_t>(Op::GoTo);
    packet[9] = 200;
    check(decode(packet, COMMAND_LEN, out), "preset byte ignored for other ops");
    packet[9] = 0;
    packet[0] = 'X';
    check(!decode(packet, COMMAND_LEN, out), "bad magic rejected");

    Status status{};
    std::uint8_t status_packet[STATUS_LEN];
    encode(status, status_packet);
    status_packet[12] = 7;
    Status decoded{};
    check(decode(status_packet, STATUS_LEN, decoded) && decoded.motion == Motion::Idle,
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

static_assert(kBoxPresets < kPresetCount);
static_assert(kServiceUuid != kEchoUuid);

}  // namespace

int main()
{
    test_vocabulary();
    test_command_round_trip();
    test_status_round_trip();
    test_rejects();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
