#include "segments.h"

#include "cJSON.h"

#include <cstdint>
#include <cstring>

namespace room::segments {
namespace {
constexpr double TICKS_PER_SECOND = 10'000'000.0;  // Jellyfin counts in 100 ns

bool kind_of(const char *type, Kind &out)
{
    if (std::strcmp(type, "Intro") == 0 || std::strcmp(type, "Recap") == 0) {
        out = Kind::Intro;
        return true;
    }
    if (std::strcmp(type, "Outro") == 0 || std::strcmp(type, "Preview") == 0) {
        out = Kind::Credits;
        return true;
    }
    return false;
}

int seconds_of(const cJSON *ticks)
{
    return cJSON_IsNumber(ticks) ? static_cast<int>(ticks->valuedouble / TICKS_PER_SECOND) : -1;
}
}  // namespace

std::string path_for(const std::string &item)
{
    return "/MediaSegments/" + item;
}

std::vector<Segment> parse(const std::string &answer)
{
    std::vector<Segment> out;
    cJSON       *root  = cJSON_ParseWithLength(answer.data(), answer.size());
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "Items");
    const cJSON *item  = nullptr;
    cJSON_ArrayForEach(item, items)
    {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "Type");
        Segment      segment{};
        if (!cJSON_IsString(type) || !kind_of(type->valuestring, segment.kind)) {
            continue;
        }
        segment.start_s = seconds_of(cJSON_GetObjectItemCaseSensitive(item, "StartTicks"));
        segment.end_s   = seconds_of(cJSON_GetObjectItemCaseSensitive(item, "EndTicks"));
        if (segment.start_s >= 0 && segment.end_s > segment.start_s) {
            out.push_back(segment);
        }
    }
    cJSON_Delete(root);
    return out;
}
}  // namespace room::segments
