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
    }
    return "";
}
}  // namespace

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
    out.active     = !out.title.empty();
    out.playing    = out.active && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playing"));
    out.position_s = seconds_of(root, "position");
    out.duration_s = seconds_of(root, "duration");
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
    }
    cJSON_Delete(root);
    return true;
}

std::string command_body(Command command, int position_s)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", command_name(command));
    if (command == Command::Seek) {
        cJSON_AddNumberToObject(root, "position", position_s);
    }
    char       *text = cJSON_PrintUnformatted(root);
    std::string out  = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string art_path(const std::string &art)
{
    return art.empty() ? "" : "/art.jpg?v=" + art;
}
}  // namespace laptop
