#include "picks.h"

#include "cJSON.h"

namespace room::picks {
namespace {
constexpr char URI_SCHEME[]  = "spotify:";
constexpr char EMBED[]       = "https://open.spotify.com/oembed?url=https://open.spotify.com/";

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
}  // namespace

std::string embed_url(const std::string &uri)
{
    // spotify:playlist:ID is open.spotify.com/playlist/ID
    const std::size_t scheme = sizeof(URI_SCHEME) - 1;
    const std::size_t colon  = uri.find(':', scheme);
    if (uri.compare(0, scheme, URI_SCHEME) != 0 || colon == std::string::npos ||
        colon == scheme || colon + 1 >= uri.size()) {
        return "";
    }
    return EMBED + uri.substr(scheme, colon - scheme) + "/" + uri.substr(colon + 1);
}

bool take_embed(const std::string &answer, Pick &pick)
{
    cJSON            *root  = cJSON_ParseWithLength(answer.data(), answer.size());
    const std::string title = text_of(root, "title");
    if (!title.empty()) {
        pick.name  = title;
        pick.image = text_of(root, "thumbnail_url");
    }
    cJSON_Delete(root);
    return !title.empty();
}

std::string play_request(const Pick &pick, const std::string &speaker)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", "spotcast");
    cJSON_AddStringToObject(root, "service", "play_media");
    cJSON *data   = cJSON_CreateObject();
    cJSON *target = cJSON_CreateObject();
    cJSON_AddStringToObject(target, "entity_id", speaker.c_str());
    cJSON_AddItemToObject(data, "media_player", target);
    cJSON_AddStringToObject(data, "spotify_uri", pick.uri.c_str());
    cJSON_AddItemToObject(root, "service_data", data);
    return print_and_free(root);
}
}  // namespace room::picks
