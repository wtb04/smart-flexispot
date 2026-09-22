#include "loctek_proto.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {
using namespace loctek;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> in)
{
    return std::vector<std::uint8_t>(in.begin(), in.end());
}

/** Feeds a byte stream and returns every frame the parser accepts. */
std::vector<Frame> feed(const std::vector<std::uint8_t> &stream)
{
    Parser             parser;
    std::vector<Frame> frames;
    for (std::uint8_t b : stream) {
        if (std::optional<Frame> frame = parser.push(b)) {
            frames.push_back(*frame);
        }
    }
    return frames;
}

void test_key_frames()
{
    const struct {
        Key                       key;
        std::vector<std::uint8_t> expect;
        const char               *name;
    } cases[] = {
        {Key::None, bytes({0x9b, 0x06, 0x02, 0x00, 0x00, 0x6c, 0xa1, 0x9d}), "wake/stop"},
        {Key::Up, bytes({0x9b, 0x06, 0x02, 0x01, 0x00, 0xfc, 0xa0, 0x9d}), "up"},
        {Key::Down, bytes({0x9b, 0x06, 0x02, 0x02, 0x00, 0x0c, 0xa0, 0x9d}), "down"},
        {Key::Preset1, bytes({0x9b, 0x06, 0x02, 0x04, 0x00, 0xac, 0xa3, 0x9d}), "preset 1"},
        {Key::Preset2, bytes({0x9b, 0x06, 0x02, 0x08, 0x00, 0xac, 0xa6, 0x9d}), "preset 2"},
        {Key::Stand, bytes({0x9b, 0x06, 0x02, 0x10, 0x00, 0xac, 0xac, 0x9d}), "stand"},
        {Key::Memory, bytes({0x9b, 0x06, 0x02, 0x20, 0x00, 0xac, 0xb8, 0x9d}), "memory"},
        {Key::Alarm, bytes({0x9b, 0x06, 0x02, 0x40, 0x00, 0xac, 0x90, 0x9d}), "alarm"},
        {Key::Sit, bytes({0x9b, 0x06, 0x02, 0x00, 0x01, 0xac, 0x60, 0x9d}), "sit"},
    };

    for (const auto &c : cases) {
        const KeyFrame got = build_key_frame(c.key);
        check(std::vector<std::uint8_t>(got.begin(), got.end()) == c.expect, c.name);
    }
}

static_assert(build_key_frame(Key::Up) ==
              KeyFrame{0x9b, 0x06, 0x02, 0x01, 0x00, 0xfc, 0xa0, 0x9d});
static_assert(build_key_frame(Key::None) ==
              KeyFrame{0x9b, 0x06, 0x02, 0x00, 0x00, 0x6c, 0xa1, 0x9d});

void test_presets()
{
    const struct {
        Preset                    preset;
        std::vector<std::uint8_t> expect;
        const char               *name;
    } cases[] = {
        {Preset::One, bytes({0x9b, 0x06, 0x02, 0x04, 0x00, 0xac, 0xa3, 0x9d}), "preset 1 frame"},
        {Preset::Two, bytes({0x9b, 0x06, 0x02, 0x08, 0x00, 0xac, 0xa6, 0x9d}), "preset 2 frame"},
        {Preset::Three, bytes({0x9b, 0x06, 0x02, 0x10, 0x00, 0xac, 0xac, 0x9d}), "preset 3 frame"},
        {Preset::Four, bytes({0x9b, 0x06, 0x02, 0x00, 0x01, 0xac, 0x60, 0x9d}), "preset 4 frame"},
    };

    for (const auto &c : cases) {
        const KeyFrame got = build_key_frame(key_for(c.preset));
        check(std::vector<std::uint8_t>(got.begin(), got.end()) == c.expect, c.name);
    }

    check(build_key_frame(Key::Memory) ==
              KeyFrame{0x9b, 0x06, 0x02, 0x20, 0x00, 0xac, 0xb8, 0x9d},
          "memory key frame");

    const Key keys[] = {key_for(Preset::One), key_for(Preset::Two), key_for(Preset::Three),
                        key_for(Preset::Four)};
    bool distinct = true;
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = i + 1; j < 4; ++j) {
            distinct &= keys[i] != keys[j];
        }
        distinct &= keys[i] != Key::None;
    }
    check(distinct, "presets map to four distinct keys");
}

void test_height_decode()
{
    const struct {
        std::vector<std::uint8_t> frame;
        std::optional<int>        expect;
        const char               *name;
    } cases[] = {
        {bytes({0x9b, 0x07, 0x12, 0x07, 0xe6, 0x06, 0x1b, 0xef, 0x9d}), 741, "74.1 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x07, 0xe6, 0x4f, 0xed, 0x2e, 0x9d}), 743, "74.3 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x07, 0xe6, 0x7d, 0x38, 0xaf, 0x9d}), 746, "74.6 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x7f, 0xbf, 0x3f, 0x40, 0x95, 0x9d}), 800, "80.0 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x7f, 0xbf, 0x5b, 0xab, 0x94, 0x9d}), 802, "80.2 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x07, 0xed, 0x3f, 0x39, 0x28, 0x9d}), 750, "75.0 cm"},
        {bytes({0x9b, 0x07, 0x12, 0x06, 0x06, 0x6d, 0xf4, 0xb6, 0x9d}), 1150, "115 cm, no point"},
        {bytes({0x9b, 0x07, 0x12, 0x00, 0x00, 0x00, 0xb8, 0x94, 0x9d}), std::nullopt, "blank"},
        {bytes({0x9b, 0x07, 0x12, 0x6d, 0x40, 0x06, 0xa7, 0xb4, 0x9d}), std::nullopt, "\"S-1\""},
        {bytes({0x9b, 0x07, 0x12, 0x79, 0x3f, 0x06, 0x93, 0xd4, 0x9d}), std::nullopt, "\"E01\""},
    };

    for (const auto &c : cases) {
        const std::vector<Frame> frames = feed(c.frame);
        check(frames.size() == 1 && decode_height_mm(frames[0]) == c.expect, c.name);
    }
}

void test_stream()
{
    const std::vector<Frame> frames = feed(bytes({0x9b, 0x04, 0x15, 0xbf, 0xc2, 0x9d,
                                                  0x9b, 0x07, 0x12, 0x07, 0xe6, 0x06, 0x1b, 0xef,
                                                  0x9d, 0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d}));
    check(frames.size() == 3, "three concatenated frames");
    if (frames.size() == 3) {
        check(frames[1].type() == FrameType::Height, "height frame type");
        check(frames[2].type() == FrameType::Heartbeat, "heartbeat frame type");
    }
}

void test_payload_bounds()
{
    const std::vector<Frame> height =
        feed(bytes({0x9b, 0x07, 0x12, 0x07, 0xe6, 0x06, 0x1b, 0xef, 0x9d}));
    check(height.size() == 1 && height[0].payload().size() == 3,
          "height frame carries exactly three payload bytes");

    const std::vector<Frame> heartbeat = feed(bytes({0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d}));
    check(heartbeat.size() == 1 && heartbeat[0].payload().size() == 0,
          "heartbeat carries no payload");
    check(heartbeat.size() == 1 && !decode_height_mm(heartbeat[0]).has_value(),
          "a payloadless frame decodes to no height");
}

void test_rejects()
{
    check(feed(bytes({0x9b, 0x07, 0x12, 0x07, 0xe6, 0x06, 0x00, 0x00, 0x9d})).empty(),
          "rejects bad CRC");
    check(feed(bytes({0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x00})).empty(), "rejects bad end byte");
    check(feed(bytes({0x00, 0xff, 0x12, 0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d})).size() == 1,
          "resynchronises after garbage");
    check(feed(bytes({0x9b, 0x07, 0x12,
                      0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d,
                      0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d})).size() == 1,
          "recovers one frame after a truncation");
    check(feed(bytes({0x9b, 0xff, 0x12, 0x9b, 0x04, 0x11, 0x7c, 0xc3, 0x9d})).size() == 1,
          "rejects oversized length and recovers");
}

}  // namespace

int main()
{
    test_key_frames();
    test_presets();
    test_height_decode();
    test_stream();
    test_payload_bounds();
    test_rejects();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
