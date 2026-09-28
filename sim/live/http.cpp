#include "http.h"

#include <curl/curl.h>

namespace live {
namespace {
constexpr long TIMEOUT_S = 15;

std::size_t take(char *data, std::size_t size, std::size_t count, void *into)
{
    static_cast<std::string *>(into)->append(data, size * count);
    return size * count;
}
}  // namespace

Answer get(const std::string &url, const std::vector<std::string> &headers)
{
    Answer answer;
    CURL  *curl = curl_easy_init();
    if (curl == nullptr) {
        return answer;
    }
    curl_slist *list = nullptr;
    for (const std::string &header : headers) {
        list = curl_slist_append(list, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "smart-flexispot-sim");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, take);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer.body);
    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &answer.status);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return answer;
}
}  // namespace live
