// The screen as the design kit draws it, on the shared core in ui.cpp: being
// built page by page beside classic_front.cpp, which the panel still runs.

#include "ui_core.h"

#include "kit.h"

#include <cstdio>

namespace ui::front {
void init(std::uint32_t accent)
{
    kit::init(kit::Look::Dark);
    if (accent != 0) {
        kit::set_accent(accent);
    }
    kit::screen(lv_screen_active());
}

bool build_next_part() { return false; }

void apply() {}
}  // namespace ui::front

namespace ui::detail {
bool         notice_on_show() { return false; }
std::int64_t cinema_dark_after() { return 0; }
bool         cinema_offers_skip() { return false; }
}  // namespace ui::detail

namespace ui::detail {
void select_page(int index) { s_page = index; }
}  // namespace ui::detail

namespace ui {
esp_err_t splash_step(const char *) { return ESP_OK; }  // the start-up screen comes later
esp_err_t splash_done() { return ESP_OK; }
bool      radar_choose(const char *) { return false; }
void      set_radar_range(int) {}

// The benches of the old screen's radar, dock and pages; the kit's own is the lab's.
namespace {
int not_here(char *out, std::size_t size)
{
    return std::snprintf(out, size, "not in the design kit's screen: its bench is /bench?lab\n");
}
}  // namespace
int bench_radar(char *out, std::size_t size) { return not_here(out, size); }
int bench_radar_open(char *out, std::size_t size) { return not_here(out, size); }
int bench_radar_map(char *out, std::size_t size, bool) { return not_here(out, size); }
int bench_desk_card(char *out, std::size_t size) { return not_here(out, size); }
int bench_pages(char *out, std::size_t size) { return not_here(out, size); }
}  // namespace ui
