#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Desk Link's envelope: every message between the panel and a laptop, each
// way, sealed with AES-256-GCM under the key the two share, so a network that
// can see them learns nothing and can forge nothing. The bytes are "DL1", a
// 12-byte nonce, the ciphertext and the 16-byte tag; what is authenticated
// with them is "DL1 " and the path it went to, so a message cannot be taken
// for one sent elsewhere. Inside is JSON with "type" and "at", when it was
// sent in Unix milliseconds. Pure, the cipher handed in, so the host tests
// can run it.
namespace laptop::link {
inline constexpr std::size_t kNonceLen = 12;
inline constexpr std::size_t kTagLen   = 16;
inline constexpr std::size_t kKeyLen   = 32;
using Nonce = std::array<std::uint8_t, kNonceLen>;

/** AES-256-GCM: mbedTLS on the panel, a stand-in in the tests. */
class Cipher {
public:
    virtual ~Cipher() = default;
    /** `sealed` becomes the ciphertext and then the tag. */
    virtual bool seal(const Nonce &nonce, const std::string &aad, const std::string &plain, std::string &sealed) = 0;
    /** False when the tag does not hold: the wrong key, or bytes changed on the way. */
    virtual bool open(const Nonce &nonce, const std::string &aad, const std::string &sealed, std::string &plain) = 0;
};

std::string seal(Cipher &cipher, const Nonce &nonce, const std::string &path, const std::string &plain);

enum class Opened { Ok, Malformed, Forged };
Opened open(Cipher &cipher, const std::string &path, const char *body, std::size_t length, std::string &plain,
            Nonce &nonce);

/** Refuses what is stale, as an old message replayed would be, and what has
 *  come before, as one replayed at once would. */
class Fresh {
public:
    static constexpr std::int64_t kWindowMs = 60 * 1000;
    bool take(const Nonce &nonce, std::int64_t at_ms, std::int64_t now_ms);

private:
    static constexpr int SEEN = 64;  // more than come in the window
    Nonce seen_[SEEN]{};
    int   next_ = 0;
};

/** The parsed hex key, false unless it is kKeyLen bytes of it. */
bool parse_key(const char *hex, std::array<std::uint8_t, kKeyLen> &key);
}  // namespace laptop::link
