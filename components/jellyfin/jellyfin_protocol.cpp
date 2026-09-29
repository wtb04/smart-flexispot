#include "jellyfin_protocol.h"

#include "cJSON.h"

namespace jellyfin {
namespace {
constexpr double       TICKS_PER_SECOND = 10'000'000.0;  // Jellyfin counts in 100 ns
constexpr std::int64_t TICKS_PER_S      = 10'000'000;

std::string print_and_free(cJSON *root)
{
    char       *text = cJSON_PrintUnformatted(root);
    std::string out  = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string text_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

int int_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

int seconds_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? static_cast<int>(item->valuedouble / TICKS_PER_SECOND) : 0;
}

constexpr char DROPPED_KEY[] = "\"NowPlayingQueueFullItems\":";

bool ends_with(const std::string &text, const char *tail)
{
    const std::size_t n = std::char_traits<char>::length(tail);
    return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

NowPlaying read_session(const cJSON *session)
{
    NowPlaying   out;
    const cJSON *item  = cJSON_GetObjectItemCaseSensitive(session, "NowPlayingItem");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(session, "PlayState");
    out.session    = text_of(session, "Id");
    out.item       = text_of(item, "Id");
    out.active     = cJSON_IsObject(item) && !out.item.empty() && !out.session.empty();
    out.paused     = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "IsPaused"));
    out.kind       = text_of(item, "Type");
    out.title      = text_of(item, "Name");
    out.series     = text_of(item, "SeriesName");
    out.series_id  = text_of(item, "SeriesId");
    out.season_id  = text_of(item, "SeasonId");
    out.season     = int_of(item, "ParentIndexNumber");
    out.episode    = int_of(item, "IndexNumber");
    out.position_s = seconds_of(state, "PositionTicks");
    out.duration_s = seconds_of(item, "RunTimeTicks");
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(state, "VolumeLevel");
    out.volume          = cJSON_IsNumber(volume) ? volume->valueint : -1;
    const cJSON *shown  = cJSON_GetObjectItemCaseSensitive(state, "SubtitleStreamIndex");
    out.subtitle        = cJSON_IsNumber(shown) ? shown->valueint : -1;
    const cJSON *stream = nullptr;
    cJSON_ArrayForEach(stream, cJSON_GetObjectItemCaseSensitive(item, "MediaStreams"))
    {
        if (text_of(stream, "Type") != "Subtitle") {
            continue;
        }
        const bool is_default = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(stream, "IsDefault"));
        if (out.subtitle_track < 0 || is_default) {
            out.subtitle_track = int_of(stream, "Index");
        }
        if (is_default) {
            break;
        }
    }
    return out;
}
}  // namespace

void Trimmer::reset()
{
    *this = Trimmer{};
}

void Trimmer::feed(const char *data, std::size_t size, std::string &out)
{
    for (std::size_t i = 0; i < size; ++i) {
        if (skipping_) {
            skip(data[i], out);
        } else {
            pass(data[i], out);
        }
    }
}

void Trimmer::pass(char c, std::string &out)
{
    out.push_back(c);
    if (in_string_) {
        if (escaped_) {
            escaped_ = false;
        } else if (c == '\\') {
            escaped_ = true;
        } else if (c == '"') {
            in_string_ = false;
        }
        return;
    }
    if (c == '"') {
        in_string_ = true;
    } else if (c == ':' && ends_with(out, DROPPED_KEY)) {
        skipping_ = true;
        opened_   = false;
    }
}

void Trimmer::skip(char c, std::string &out)
{
    if (!opened_) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            return;
        }
        if (c != '[' && c != '{') {
            skipping_ = false;  // not a list after all: let it through
            pass(c, out);
            return;
        }
        opened_ = true;
        depth_  = 1;
        return;
    }
    if (in_string_) {
        if (escaped_) {
            escaped_ = false;
        } else if (c == '\\') {
            escaped_ = true;
        } else if (c == '"') {
            in_string_ = false;
        }
        return;
    }
    if (c == '"') {
        in_string_ = true;
    } else if (c == '[' || c == '{') {
        ++depth_;
    } else if ((c == ']' || c == '}') && --depth_ == 0) {
        skipping_ = false;
        out += "null";
    }
}

std::string sessions_start(int interval_ms)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "MessageType", "SessionsStart");
    cJSON_AddStringToObject(root, "Data", ("0," + std::to_string(interval_ms)).c_str());
    return print_and_free(root);
}

std::string keep_alive()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "MessageType", "KeepAlive");
    return print_and_free(root);
}

std::string message_type(const std::string &message)
{
    cJSON            *root = cJSON_ParseWithLength(message.data(), message.size());
    const std::string type = text_of(root, "MessageType");
    cJSON_Delete(root);
    return type;
}

NowPlaying now_playing(const std::string &message, const std::string &own_device)
{
    NowPlaying   chosen;
    cJSON       *root     = cJSON_ParseWithLength(message.data(), message.size());
    const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(root, "Data");
    const cJSON *session  = nullptr;
    cJSON_ArrayForEach(session, sessions)
    {
        if (text_of(session, "DeviceId") == own_device) {
            continue;
        }
        const NowPlaying found = read_session(session);
        if (found.active && (!chosen.active || (chosen.paused && !found.paused))) {
            chosen = found;
        }
    }
    cJSON_Delete(root);
    return chosen;
}

std::string pause_path(const std::string &session, bool pause)
{
    return "/Sessions/" + session + (pause ? "/Playing/Pause" : "/Playing/Unpause");
}

std::string seek_path(const std::string &session, int position_s)
{
    return "/Sessions/" + session + "/Playing/Seek?SeekPositionTicks=" +
           std::to_string(static_cast<std::int64_t>(position_s) * TICKS_PER_S);
}

std::string command_path(const std::string &session)
{
    return "/Sessions/" + session + "/Command";
}

std::string set_volume_body(int percent)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "Name", "SetVolume");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "Volume", std::to_string(percent).c_str());
    cJSON_AddItemToObject(root, "Arguments", args);
    return print_and_free(root);
}

std::string set_subtitle_body(int stream)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "Name", "SetSubtitleStreamIndex");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "Index", std::to_string(stream).c_str());
    cJSON_AddItemToObject(root, "Arguments", args);
    return print_and_free(root);
}

std::string neighbours_path(const std::string &series, const std::string &episode)
{
    return "/Shows/" + series + "/Episodes?adjacentTo=" + episode;
}

Neighbours neighbours(const std::string &answer, const std::string &episode)
{
    Neighbours   out;
    cJSON       *root  = cJSON_ParseWithLength(answer.data(), answer.size());
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "Items");
    const cJSON *item  = nullptr;
    bool         past  = false;  // the episode itself has gone by
    cJSON_ArrayForEach(item, items)
    {
        const std::string id = text_of(item, "Id");
        if (id == episode) {
            past = true;
        } else if (!past) {
            out.previous = id;
        } else if (out.next.empty()) {
            out.next = id;
        }
    }
    cJSON_Delete(root);
    if (!past) {
        return {};  // an answer about another episode says nothing of this one
    }
    return out;
}

std::string play_now_path(const std::string &session, const std::string &item)
{
    return "/Sessions/" + session + "/Playing?playCommand=PlayNow&itemIds=" + item;
}

std::string cover_path(const std::string &item, int height)
{
    return "/Items/" + item + "/Images/Primary?maxHeight=" + std::to_string(height) +
           "&format=Jpg&quality=85";
}
}  // namespace jellyfin
