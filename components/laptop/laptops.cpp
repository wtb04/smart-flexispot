#include "laptops.h"

namespace laptop {

int Laptops::find(const std::string &origin) const
{
    for (int i = 0; i < count_; ++i) {
        if (rows_[i].origin == origin) {
            return i;
        }
    }
    return -1;
}

int Laptops::make_room()
{
    if (count_ < kMax) {
        return count_++;
    }
    int oldest = -1;
    for (int i = 0; i < count_; ++i) {
        if (i != current_ && (oldest < 0 || rows_[i].heard_us < rows_[oldest].heard_us)) {
            oldest = i;
        }
    }
    rows_[oldest] = Seen{};
    return oldest;
}

void Laptops::choose()
{
    int best = -1;
    for (int i = 0; i < count_; ++i) {
        const Seen &row = rows_[i];
        if (row.now.playing && (best < 0 || row.started_us > rows_[best].started_us)) {
            best = i;
        }
    }
    if (best < 0 && current_ >= 0 && rows_[current_].now.active) {
        best = current_;  // none plays: it stays on the one it had
    }
    if (best < 0) {
        for (int i = 0; i < count_; ++i) {
            const Seen &row = rows_[i];
            if (!row.now.active) {
                continue;
            }
            if (best < 0 || row.started_us > rows_[best].started_us ||
                (row.started_us == rows_[best].started_us && row.heard_us > rows_[best].heard_us)) {
                best = i;
            }
        }
    }
    current_ = best;
}

Taken Laptops::take(const std::string &origin, const NowPlaying &now, std::int64_t now_us,
                    std::int64_t ping_every_us)
{
    int index = find(origin);
    if (index < 0 && !now.machine.empty()) {
        // the same laptop at a new address, as one restarted or docked onto another network
        for (int i = 0; i < count_; ++i) {
            if (rows_[i].now.machine == now.machine) {
                index               = i;
                rows_[i].origin     = origin;
                rows_[i].pinged_us  = -1;
                rows_[i].reaches    = -1;
                rows_[i].missed     = 0;
                break;
            }
        }
    }
    if (index < 0) {
        index               = make_room();
        rows_[index]        = Seen{};
        rows_[index].origin = origin;
    }
    Seen &row = rows_[index];
    if (now.playing && !row.now.playing) {
        row.started_us = now_us;
    }
    row.now      = now;
    row.heard_us = now_us;

    Taken taken;
    taken.ping = row.pinged_us < 0 || now_us - row.pinged_us >= ping_every_us;
    if (taken.ping) {
        row.pinged_us = now_us;
    }
    taken.reaches = row.reaches;

    const std::string before = current_origin();
    choose();
    taken.tell = current_origin() != before || current_origin() == origin;
    return taken;
}

bool Laptops::forget_quiet(std::int64_t now_us, std::int64_t quiet_us)
{
    const std::string before = current_origin();
    int               kept   = 0;
    for (int i = 0; i < count_; ++i) {
        if (now_us - rows_[i].heard_us < quiet_us) {
            rows_[kept++] = rows_[i];
        }
    }
    if (kept == count_) {
        return false;
    }
    for (int i = kept; i < count_; ++i) {
        rows_[i] = Seen{};
    }
    count_   = kept;
    current_ = -1;
    for (int i = 0; i < count_; ++i) {
        if (rows_[i].origin == before) {
            current_ = i;
        }
    }
    choose();
    return current_origin() != before;
}

bool Laptops::reached(const std::string &origin, bool yes)
{
    const int index = find(origin);
    if (index < 0) {
        return false;
    }
    Seen &row     = rows_[index];
    row.missed    = yes ? 0 : row.missed + 1;
    const int now = yes ? 1 : row.missed >= 2 ? 0 : row.reaches;
    const bool changed = now != row.reaches;
    row.reaches        = now;
    return changed;
}

bool Laptops::set_playing(bool playing, std::int64_t now_us)
{
    if (current_ < 0) {
        return false;
    }
    Seen &row = rows_[current_];
    if (playing && !row.now.playing) {
        row.started_us = now_us;
    }
    row.now.playing = playing;
    choose();
    return true;
}

NowPlaying Laptops::current_now() const
{
    return current_ >= 0 ? rows_[current_].now : NowPlaying{};
}

std::string Laptops::current_origin() const
{
    return current_ >= 0 ? rows_[current_].origin : "";
}

int Laptops::reaches_of(const std::string &origin) const
{
    const int index = find(origin);
    return index < 0 ? -2 : rows_[index].reaches;
}

}  // namespace laptop
