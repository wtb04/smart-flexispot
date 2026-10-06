#include "laptop_protocol.h"

#include <cstring>

#include "cJSON.h"

namespace laptop {
namespace {
constexpr int MAX_PORT = 65535;

std::string text_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

int seconds_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) && item->valuedouble > 0 ? static_cast<int>(item->valuedouble) : 0;
}

std::string print_and_free(cJSON *root)
{
    char       *text = cJSON_PrintUnformatted(root);
    std::string out  = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

cJSON *message(const char *type, std::int64_t at_ms)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "at", static_cast<double>(at_ms));
    return root;
}

const char *command_name(Command command)
{
    switch (command) {
        case Command::Play:
            return "play";
        case Command::Pause:
            return "pause";
        case Command::Next:
            return "next";
        case Command::Previous:
            return "previous";
        case Command::Seek:
            return "seek";
        case Command::Volume:
            return "volume";
        case Command::Mute:
            return "mute";
        case Command::VolumeUp:
            return "volume_up";
        case Command::VolumeDown:
            return "volume_down";
    }
    return "";
}
}  // namespace

bool read_head(const char *body, std::size_t length, Head &out)
{
    cJSON       *root = cJSON_ParseWithLength(body, length);
    const cJSON *at   = cJSON_GetObjectItemCaseSensitive(root, "at");
    out.type          = text_of(root, "type");
    out.at_ms         = cJSON_IsNumber(at) ? static_cast<std::int64_t>(at->valuedouble) : 0;
    cJSON_Delete(root);
    return !out.type.empty() && out.at_ms > 0;
}

std::string claude_event(const char *body, std::size_t length)
{
    cJSON       *root  = cJSON_ParseWithLength(body, length);
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    char        *text  = cJSON_IsObject(event) ? cJSON_PrintUnformatted(event) : nullptr;
    std::string  out   = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool read(const char *body, std::size_t length, NowPlaying &out)
{
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (!cJSON_IsNumber(port) || port->valueint <= 0 || port->valueint > MAX_PORT) {
        cJSON_Delete(root);
        return false;
    }
    out            = NowPlaying{};
    out.port       = port->valueint;
    out.machine    = text_of(root, "machine");
    out.app        = text_of(root, "app");
    out.title      = text_of(root, "title");
    out.artist     = text_of(root, "artist");
    out.art        = text_of(root, "art");
    out.square     = text_of(root, "square");
    out.video      = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "video"));
    out.active     = !out.title.empty();
    out.playing    = out.active && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playing"));
    out.position_s = seconds_of(root, "position");
    out.duration_s = seconds_of(root, "duration");
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    out.volume     = cJSON_IsNumber(volume) && volume->valuedouble >= 0 ? static_cast<int>(volume->valuedouble + 0.5) : -1;
    out.muted      = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "muted"));
    const cJSON *take = nullptr;
    cJSON_ArrayForEach(take, cJSON_GetObjectItemCaseSensitive(root, "takes"))
    {
        if (!cJSON_IsString(take)) {
            continue;
        }
        const char *name   = take->valuestring;
        out.takes_pause    = out.takes_pause || std::strcmp(name, "pause") == 0;
        out.takes_seek     = out.takes_seek || std::strcmp(name, "seek") == 0;
        out.takes_next     = out.takes_next || std::strcmp(name, "next") == 0;
        out.takes_previous = out.takes_previous || std::strcmp(name, "previous") == 0;
        out.takes_volume   = out.takes_volume || std::strcmp(name, "volume") == 0;
        out.takes_volume_step = out.takes_volume_step || std::strcmp(name, "volume_step") == 0;
    }
    cJSON_Delete(root);
    return true;
}

std::string command_body(Command command, int value, std::int64_t at_ms)
{
    cJSON *root = message("command", at_ms);
    cJSON_AddStringToObject(root, "command", command_name(command));
    if (command == Command::Seek) {
        cJSON_AddNumberToObject(root, "position", value);
    } else if (command == Command::Volume) {
        cJSON_AddNumberToObject(root, "level", value);
    } else if (command == Command::Mute) {
        cJSON_AddBoolToObject(root, "muted", value != 0);
    }
    return print_and_free(root);
}

std::string pong_body(const char *panel, const char *firmware, int reaches, std::int64_t at_ms)
{
    cJSON *root = message("pong", at_ms);
    cJSON_AddStringToObject(root, "panel", panel);
    cJSON_AddStringToObject(root, "firmware", firmware);
    if (reaches >= 0) {
        cJSON_AddBoolToObject(root, "reaches", reaches != 0);
    }
    return print_and_free(root);
}

std::string ping_body(std::int64_t at_ms)
{
    return print_and_free(message("ping", at_ms));
}

std::string ok_body(std::int64_t at_ms)
{
    return print_and_free(message("ok", at_ms));
}

std::string cover_body(const std::string &art, std::int64_t at_ms)
{
    cJSON *root = message("cover", at_ms);
    cJSON_AddStringToObject(root, "art", art.c_str());
    return print_and_free(root);
}
}  // namespace laptop
