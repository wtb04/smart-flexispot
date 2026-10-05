#include "link_envelope.h"

#include <cstring>

namespace laptop::link {
namespace {
constexpr char MAGIC[] = "DL1";
constexpr std::size_t MAGIC_LEN = sizeof(MAGIC) - 1;

std::string aad_for(const std::string &path)
{
    return std::string(MAGIC) + ' ' + path;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
}  // namespace

std::string seal(Cipher &cipher, const Nonce &nonce, const std::string &path, const std::string &plain)
{
    std::string sealed;
    if (!cipher.seal(nonce, aad_for(path), plain, sealed)) {
        return "";
    }
    std::string out(MAGIC, MAGIC_LEN);
    out.append(reinterpret_cast<const char *>(nonce.data()), nonce.size());
    out += sealed;
    return out;
}

Opened open(Cipher &cipher, const std::string &path, const char *body, std::size_t length, std::string &plain,
            Nonce &nonce)
{
    if (length < MAGIC_LEN + kNonceLen + kTagLen || std::memcmp(body, MAGIC, MAGIC_LEN) != 0) {
        return Opened::Malformed;
    }
    std::memcpy(nonce.data(), body + MAGIC_LEN, kNonceLen);
    const std::string sealed(body + MAGIC_LEN + kNonceLen, length - MAGIC_LEN - kNonceLen);
    return cipher.open(nonce, aad_for(path), sealed, plain) ? Opened::Ok : Opened::Forged;
}

bool Fresh::take(const Nonce &nonce, std::int64_t at_ms, std::int64_t now_ms)
{
    const std::int64_t off = at_ms - now_ms;
    if (off > kWindowMs || off < -kWindowMs) {
        return false;
    }
    for (const Nonce &seen : seen_) {
        if (seen == nonce) {
            return false;
        }
    }
    seen_[next_] = nonce;
    next_        = (next_ + 1) % SEEN;
    return true;
}

bool parse_key(const char *hex, std::array<std::uint8_t, kKeyLen> &key)
{
    if (hex == nullptr || std::strlen(hex) != kKeyLen * 2) {
        return false;
    }
    for (std::size_t i = 0; i < kKeyLen; ++i) {
        const int high = hex_digit(hex[2 * i]);
        const int low  = hex_digit(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        key[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return true;
}
}  // namespace laptop::link
