#pragma once

// LVGL's profiler hooks, for a profiling build (build/prof): each step's time
// added up as it ends, rather than traced. main/remote.cpp has them.
#ifdef __cplusplus
extern "C" {
#endif
void draw_profile_begin(const char *step);
void draw_profile_end(const char *step);
#ifdef __cplusplus
}
#endif

#define LV_PROFILER_BEGIN          draw_profile_begin(__func__)
#define LV_PROFILER_END            draw_profile_end(__func__)
#define LV_PROFILER_BEGIN_TAG(tag) draw_profile_begin(tag)
#define LV_PROFILER_END_TAG(tag)   draw_profile_end(tag)
