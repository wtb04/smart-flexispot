#include "lesson.h"

#include <gtest/gtest.h>

namespace {
using desk::detail::Lesson;

constexpr std::uint32_t TICK = 200;

// A trip as the supervisor sees it: a height a tick, how long each has stood.
struct Trip {
    Lesson        lesson;
    std::uint32_t now = 0, still_since = 0, asked = 1;
    int           height = -1;

    explicit Trip(int from) : height(from) { lesson.start(0, from, asked, now); }
    int at(int h)
    {
        now += TICK;
        if (h != height) {
            height      = h;
            still_since = now;
        }
        return lesson.step(height, asked, now, now - still_since);
    }
    int rest(std::uint32_t ms)
    {
        int learned = -1;
        for (std::uint32_t t = 0; t < ms; t += TICK) {
            const int got = at(height);
            learned       = got >= 0 ? got : learned;
        }
        return learned;
    }
};

TEST(Lesson, learns_a_clean_run)
{
    Trip trip(750);
    for (int h = 760; h <= 1150; h += 10) {
        EXPECT_LT(trip.at(h), 0) << "nothing while it moves";
    }
    EXPECT_EQ(trip.rest(2000), 1150) << "where it came to rest";
    EXPECT_FALSE(trip.lesson.active());
}

TEST(Lesson, ends_at_the_first_rest)
{
    Trip trip(1150);
    for (int h = 1140; h >= 750; h -= 10) {
        trip.at(h);
    }
    EXPECT_EQ(trip.rest(2000), 750);
    // what the desk does afterwards is never this preset's
    for (int h = 760; h <= 1000; h += 10) {
        EXPECT_LT(trip.at(h), 0);
    }
    EXPECT_LT(trip.rest(3000), 0) << "a second rest teaches nothing";
}

TEST(Lesson, anything_asked_on_the_way_ends_it)
{
    Trip trip(750);
    trip.at(800);
    trip.at(850);
    ++trip.asked;  // the same preset again, which stops the box, or a move, a stop, another preset
    EXPECT_LT(trip.at(850), 0);
    EXPECT_FALSE(trip.lesson.active());
    EXPECT_LT(trip.rest(3000), 0) << "the height it was stopped at is not the preset's";
}

TEST(Lesson, a_desk_that_never_leaves_teaches_nothing)
{
    Trip trip(750);  // at the preset already, or the box did not take the key
    EXPECT_LT(trip.rest(Lesson::LEAVE_MS + 2 * TICK), 0);
    EXPECT_FALSE(trip.lesson.active());
    // a hold soon after, as once was taken for the preset
    for (int h = 760; h <= 900; h += 10) {
        EXPECT_LT(trip.at(h), 0);
    }
    EXPECT_LT(trip.rest(3000), 0);
}

TEST(Lesson, turning_back_ends_it)
{
    Trip trip(750);
    trip.at(800);
    trip.at(850);
    EXPECT_LT(trip.at(840), 0);
    EXPECT_FALSE(trip.lesson.active());
}

TEST(Lesson, the_readings_jitter_a_little)
{
    Trip trip(750);
    trip.at(800);
    trip.at(850);
    trip.at(849);
    EXPECT_TRUE(trip.lesson.active()) << "a millimetre back is the reading, not the desk";
    trip.at(900);
    EXPECT_EQ(trip.rest(2000), 900);
}

TEST(Lesson, too_long_a_run_ends_it)
{
    Trip trip(750);
    trip.at(800);
    int h = 800;
    while (trip.now <= Lesson::LONGEST_MS) {
        trip.at(++h);
    }
    EXPECT_FALSE(trip.lesson.active());
}

TEST(Lesson, an_unknown_height_waits_but_not_for_ever)
{
    Trip trip(-1);
    EXPECT_LT(trip.rest(Lesson::LEAVE_MS + 2 * TICK), 0);
    EXPECT_FALSE(trip.lesson.active());
}
}  // namespace
