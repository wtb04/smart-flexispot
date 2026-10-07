#include "lesson.h"

#include <cstdlib>

namespace desk::detail {

void Lesson::start(int index, int from_mm, std::uint32_t asked, std::uint32_t now_ms)
{
    index_   = index;
    from_    = from_mm;
    last_    = from_mm;
    dir_     = 0;
    asked_   = asked;
    started_ = now_ms;
    why_     = "";
}

void Lesson::end(const char *why)
{
    index_ = -1;
    why_   = why;
}

int Lesson::step(int height_mm, std::uint32_t asked, std::uint32_t now_ms, std::uint32_t still_ms)
{
    if (!active()) {
        return -1;
    }
    if (asked != asked_) {
        end("something else was asked of the desk");
        return -1;
    }
    if (now_ms - started_ > LONGEST_MS) {
        end("the run took too long");
        return -1;
    }
    if (height_mm < 0 || from_ < 0) {
        if (dir_ == 0 && now_ms - started_ > LEAVE_MS) {
            end("the desk did not leave");
        }
        return -1;
    }
    if (dir_ == 0) {
        if (std::abs(height_mm - from_) > DEPARTED_MM) {
            dir_  = height_mm > from_ ? 1 : -1;
            last_ = height_mm;
        } else if (now_ms - started_ > LEAVE_MS) {
            end("the desk did not leave");
        }
        return -1;
    }
    if ((height_mm - last_) * dir_ < -BACK_MM) {
        end("the desk turned back");
        return -1;
    }
    last_ = height_mm;
    if (still_ms < SETTLE_MS) {
        return -1;
    }
    end("learned");
    return height_mm;
}

}  // namespace desk::detail
