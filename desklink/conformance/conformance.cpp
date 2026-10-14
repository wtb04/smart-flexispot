// Speaks for the panel in a Desk Link's tests: writes fixtures.json, and opens,
// seals and reads what a Desk Link hands it, with the panel's own code.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

#include "cJSON.h"
#include "laptop_protocol.h"
#include "link_envelope.h"
#include "mbedtls/gcm.h"

using namespace laptop;

namespace {
constexpr std::int64_t AT = 1791223792000;
constexpr const char *KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr const char *OTHER_KEY = "ff0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

// As laptop.cpp's Gcm, without its lock.
class Gcm : public link::Cipher {
public:
    explicit Gcm(const char *hex)
    {
        std::array<std::uint8_t, link::kKeyLen> key{};
        mbedtls_gcm_init(&context_);
        ready_ = link::parse_key(hex, key) &&
                 mbedtls_gcm_setkey(&context_, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8) == 0;
    }
    ~Gcm() override { mbedtls_gcm_free(&context_); }
    bool ready() const { return ready_; }

    bool seal(const link::Nonce &nonce, const std::string &aad, const std::string &plain, std::string &sealed) override
    {
        sealed.assign(plain.size() + link::kTagLen, '\0');
        auto *out = reinterpret_cast<unsigned char *>(sealed.data());
        return mbedtls_gcm_crypt_and_tag(&context_, MBEDTLS_GCM_ENCRYPT, plain.size(), nonce.data(), nonce.size(),
                                         reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
                                         reinterpret_cast<const unsigned char *>(plain.data()), out, link::kTagLen,
                                         out + plain.size()) == 0;
    }

    bool open(const link::Nonce &nonce, const std::string &aad, const std::string &sealed, std::string &plain) override
    {
        if (sealed.size() < link::kTagLen) {
            return false;
        }
        const std::size_t length = sealed.size() - link::kTagLen;
        plain.assign(length, '\0');
        const auto *in = reinterpret_cast<const unsigned char *>(sealed.data());
        return mbedtls_gcm_auth_decrypt(&context_, length, nonce.data(), nonce.size(),
                                        reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
                                        in + length, link::kTagLen, in,
                                        reinterpret_cast<unsigned char *>(plain.data())) == 0;
    }

private:
    mbedtls_gcm_context context_{};
    bool ready_ = false;
};

std::string hex(const std::string &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

link::Nonce nonce_of(std::uint8_t fill)
{
    link::Nonce nonce{};
    nonce.fill(fill);
    nonce[0] = 0xd1;  // never all zero: Fresh's empty slots are
    return nonce;
}

std::string stdin_all()
{
    return std::string(std::istreambuf_iterator<char>(std::cin), {});
}

void add_sealed(cJSON *list, const char *name, const char *path, const char *opens_on, const std::string &body,
                const char *result, const std::string *plain)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "path", path);
    cJSON_AddStringToObject(item, "opens_on", opens_on);
    cJSON_AddStringToObject(item, "body", hex(body).c_str());
    cJSON_AddStringToObject(item, "result", result);
    if (plain != nullptr) {
        cJSON_AddStringToObject(item, "plain", plain->c_str());
    } else {
        cJSON_AddNullToObject(item, "plain");
    }
    cJSON_AddItemToArray(list, item);
}

const char *opened_name(link::Opened opened)
{
    switch (opened) {
        case link::Opened::Ok:
            return "ok";
        case link::Opened::Malformed:
            return "malformed";
        case link::Opened::Forged:
            return "forged";
    }
    return "";
}

int fixtures()
{
    Gcm gcm(KEY);
    Gcm other(OTHER_KEY);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "key", KEY);
    cJSON_AddNumberToObject(root, "now_ms", static_cast<double>(AT));

    cJSON *bodies = cJSON_AddObjectToObject(root, "bodies");
    const std::pair<const char *, std::string> named[] = {
        {"ping", ping_body(AT)},
        {"ok", ok_body(AT)},
        {"cover", cover_body("3f2a1b0c9d8e", AT)},
        {"pong", pong_body("smart-flexispot", "v0.9.0", 1, AT)},
        {"pong_untried", pong_body("smart-flexispot", "v0.9.0", -1, AT)},
        {"command_play", command_body(Command::Play, 0, AT)},
        {"command_pause", command_body(Command::Pause, 0, AT)},
        {"command_next", command_body(Command::Next, 0, AT)},
        {"command_previous", command_body(Command::Previous, 0, AT)},
        {"command_seek", command_body(Command::Seek, 167, AT)},
        {"command_volume", command_body(Command::Volume, 40, AT)},
        {"command_mute", command_body(Command::Mute, 1, AT)},
        {"command_volume_up", command_body(Command::VolumeUp, 0, AT)},
        {"command_volume_down", command_body(Command::VolumeDown, 0, AT)},
    };
    for (const auto &[name, body] : named) {
        cJSON_AddStringToObject(bodies, name, body.c_str());
    }

    cJSON *sealed = cJSON_AddArrayToObject(root, "sealed");
    std::uint8_t fill = 1;
    for (const auto &[name, body] : named) {
        for (const char *path : {"/link", "/link reply", "/cover reply"}) {
            const std::string out = link::seal(gcm, nonce_of(fill++), path, body);
            const std::string label = std::string(name) + " on " + path;
            add_sealed(sealed, label.c_str(), path, path, out, "ok", &body);
        }
    }
    const std::string ping = ping_body(AT);
    std::string body = link::seal(other, nonce_of(200), "/link", ping);
    add_sealed(sealed, "wrong key", "/link", "/link", body, "forged", nullptr);
    body = link::seal(gcm, nonce_of(201), "/cover", ping);
    add_sealed(sealed, "wrong path", "/cover", "/link", body, "forged", nullptr);
    body = link::seal(gcm, nonce_of(202), "/link", ping);
    body[body.size() / 2] ^= 0x01;
    add_sealed(sealed, "changed byte", "/link", "/link", body, "forged", nullptr);
    body = link::seal(gcm, nonce_of(203), "/link", ping).substr(0, 30);
    add_sealed(sealed, "too short", "/link", "/link", body, "malformed", nullptr);
    body = link::seal(gcm, nonce_of(204), "/link", ping);
    body[0] = 'X';
    add_sealed(sealed, "bad magic", "/link", "/link", body, "malformed", nullptr);
    // check the panel's own reading of every case agrees with the label
    for (cJSON *item = sealed->child; item != nullptr; item = item->next) {
        const std::string bytes_hex = cJSON_GetObjectItem(item, "body")->valuestring;
        std::string bytes;
        for (std::size_t i = 0; i < bytes_hex.size(); i += 2) {
            bytes += static_cast<char>(std::stoi(bytes_hex.substr(i, 2), nullptr, 16));
        }
        std::string plain;
        link::Nonce nonce{};
        const auto opened = link::open(gcm, cJSON_GetObjectItem(item, "opens_on")->valuestring, bytes.data(),
                                       bytes.size(), plain, nonce);
        if (std::string(opened_name(opened)) != cJSON_GetObjectItem(item, "result")->valuestring) {
            std::cerr << "case disagrees: " << cJSON_GetObjectItem(item, "name")->valuestring << "\n";
            return 1;
        }
    }

    cJSON *fresh = cJSON_AddArrayToObject(root, "fresh");
    link::Fresh judge;
    const std::pair<std::uint8_t, std::int64_t> sequence[] = {
        {1, AT}, {1, AT}, {2, AT - 60000}, {3, AT - 60001}, {4, AT + 60001}, {5, AT + 60000},
    };
    for (const auto &[fill_byte, at] : sequence) {
        const link::Nonce nonce = nonce_of(fill_byte);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "nonce", hex(std::string(nonce.begin(), nonce.end())).c_str());
        cJSON_AddNumberToObject(item, "at", static_cast<double>(at));
        cJSON_AddBoolToObject(item, "taken", judge.take(nonce, at, AT));
        cJSON_AddItemToArray(fresh, item);
    }

    char *text = cJSON_Print(root);
    std::cout << text << "\n";
    cJSON_free(text);
    cJSON_Delete(root);
    return 0;
}

const char *key_from_env()
{
    const char *key = std::getenv("DESKLINK_KEY");
    return key != nullptr ? key : KEY;
}

int open_cmd(const char *path)
{
    Gcm gcm(key_from_env());
    const std::string body = stdin_all();
    std::string plain;
    link::Nonce nonce{};
    switch (link::open(gcm, path, body.data(), body.size(), plain, nonce)) {
        case link::Opened::Ok:
            std::cout << plain;
            return 0;
        case link::Opened::Malformed:
            return 2;
        case link::Opened::Forged:
            return 3;
    }
    return 1;
}

int seal_cmd(const char *path)
{
    Gcm gcm(key_from_env());
    std::random_device random;
    link::Nonce nonce{};
    for (auto &byte : nonce) {
        byte = static_cast<std::uint8_t>(random());
    }
    std::cout << link::seal(gcm, nonce, path, stdin_all());
    return 0;
}

int read_state()
{
    const std::string body = stdin_all();
    NowPlaying now;
    const bool ok = read(body.data(), body.size(), now);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddBoolToObject(root, "active", now.active);
    cJSON_AddBoolToObject(root, "playing", now.playing);
    cJSON_AddStringToObject(root, "machine", now.machine.c_str());
    cJSON_AddNumberToObject(root, "port", now.port);
    cJSON_AddStringToObject(root, "app", now.app.c_str());
    cJSON_AddStringToObject(root, "title", now.title.c_str());
    cJSON_AddStringToObject(root, "artist", now.artist.c_str());
    cJSON_AddStringToObject(root, "art", now.art.c_str());
    cJSON_AddStringToObject(root, "square", now.square.c_str());
    cJSON_AddBoolToObject(root, "video", now.video);
    cJSON_AddNumberToObject(root, "position_s", now.position_s);
    cJSON_AddNumberToObject(root, "duration_s", now.duration_s);
    cJSON_AddNumberToObject(root, "volume", now.volume);
    cJSON_AddBoolToObject(root, "muted", now.muted);
    cJSON_AddBoolToObject(root, "takes_pause", now.takes_pause);
    cJSON_AddBoolToObject(root, "takes_seek", now.takes_seek);
    cJSON_AddBoolToObject(root, "takes_next", now.takes_next);
    cJSON_AddBoolToObject(root, "takes_previous", now.takes_previous);
    cJSON_AddBoolToObject(root, "takes_volume", now.takes_volume);
    cJSON_AddBoolToObject(root, "takes_volume_step", now.takes_volume_step);
    char *text = cJSON_PrintUnformatted(root);
    std::cout << text;
    cJSON_free(text);
    cJSON_Delete(root);
    return 0;
}

int read_head_cmd()
{
    const std::string body = stdin_all();
    Head head;
    const bool ok = read_head(body.data(), body.size(), head);
    std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"type\":\"" << head.type << "\",\"at\":" << head.at_ms
              << "}";
    return 0;
}
}  // namespace

int main(int argc, char **argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "fixtures") {
        return fixtures();
    }
    if (command == "open" && argc == 3) {
        return open_cmd(argv[2]);
    }
    if (command == "seal" && argc == 3) {
        return seal_cmd(argv[2]);
    }
    if (command == "read-state") {
        return read_state();
    }
    if (command == "read-head") {
        return read_head_cmd();
    }
    if (command == "claude-event") {
        const std::string body = stdin_all();
        std::cout << claude_event(body.data(), body.size());
        return 0;
    }
    std::cerr << "usage: conformance fixtures | open <path> | seal <path> | read-state | read-head | claude-event\n";
    return 1;
}
