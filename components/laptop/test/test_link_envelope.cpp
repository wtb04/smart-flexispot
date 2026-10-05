#include "link_envelope.h"

#include <gtest/gtest.h>

#include <string>

using namespace laptop::link;

namespace {
// Not a cipher, but one that fails as a real one would: its tag covers the
// nonce, what is authenticated and every byte, and nothing else opens it.
class StandIn : public Cipher {
public:
    explicit StandIn(std::uint8_t key) : key_(key) {}

    bool seal(const Nonce &nonce, const std::string &aad, const std::string &plain, std::string &sealed) override
    {
        sealed.clear();
        for (char c : plain) {
            sealed.push_back(static_cast<char>(c ^ key_));
        }
        sealed += tag(nonce, aad, sealed);
        return true;
    }

    bool open(const Nonce &nonce, const std::string &aad, const std::string &sealed, std::string &plain) override
    {
        if (sealed.size() < kTagLen) {
            return false;
        }
        const std::string body = sealed.substr(0, sealed.size() - kTagLen);
        if (sealed.substr(body.size()) != tag(nonce, aad, body)) {
            return false;
        }
        plain.clear();
        for (char c : body) {
            plain.push_back(static_cast<char>(c ^ key_));
        }
        return true;
    }

private:
    std::string tag(const Nonce &nonce, const std::string &aad, const std::string &body) const
    {
        std::uint32_t h = 2166136261u ^ key_;
        for (std::uint8_t b : nonce) {
            h = (h ^ b) * 16777619u;
        }
        for (char c : aad + '|' + body) {
            h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
        }
        std::string out(kTagLen, '\0');
        for (std::size_t i = 0; i < kTagLen; ++i) {
            out[i] = static_cast<char>(h >> (8 * (i % 4)));
            h      = h * 16777619u + static_cast<std::uint32_t>(i);
        }
        return out;
    }

    std::uint8_t key_;
};

Nonce nonce_of(std::uint8_t first)
{
    Nonce nonce{};
    nonce[0] = first;
    nonce[1] = 0x5a;
    return nonce;
}
}  // namespace

TEST(LinkEnvelope, round_trip)
{
    StandIn           cipher(0x42);
    const std::string sealed = seal(cipher, nonce_of(1), "/link", R"({"type":"state"})");
    ASSERT_EQ(sealed.substr(0, 3), "DL1");
    EXPECT_EQ(sealed.size(), 3 + kNonceLen + 16 + kTagLen);

    std::string plain;
    Nonce       nonce{};
    EXPECT_EQ(open(cipher, "/link", sealed.data(), sealed.size(), plain, nonce), Opened::Ok);
    EXPECT_EQ(plain, R"({"type":"state"})");
    EXPECT_EQ(nonce, nonce_of(1));
}

TEST(LinkEnvelope, another_key_cannot_open_it)
{
    StandIn           ours(0x42);
    StandIn           theirs(0x17);
    const std::string sealed = seal(ours, nonce_of(1), "/link", "hello");
    std::string       plain;
    Nonce             nonce{};
    EXPECT_EQ(open(theirs, "/link", sealed.data(), sealed.size(), plain, nonce), Opened::Forged);
}

TEST(LinkEnvelope, a_changed_byte_is_caught)
{
    StandIn     cipher(0x42);
    std::string sealed = seal(cipher, nonce_of(1), "/link", "pause");
    sealed[3 + kNonceLen] ^= 1;
    std::string plain;
    Nonce       nonce{};
    EXPECT_EQ(open(cipher, "/link", sealed.data(), sealed.size(), plain, nonce), Opened::Forged);
}

TEST(LinkEnvelope, bound_to_where_it_went)
{
    StandIn           cipher(0x42);
    const std::string sealed = seal(cipher, nonce_of(1), "/link", "pause");
    std::string       plain;
    Nonce             nonce{};
    EXPECT_EQ(open(cipher, "/cover", sealed.data(), sealed.size(), plain, nonce), Opened::Forged)
        << "a message to one path is no message to another";
}

TEST(LinkEnvelope, what_is_not_an_envelope)
{
    StandIn     cipher(0x42);
    std::string plain;
    Nonce       nonce{};
    const std::string json = R"({"port":47801,"title":"x"})";
    EXPECT_EQ(open(cipher, "/link", json.data(), json.size(), plain, nonce), Opened::Malformed)
        << "the plain reports of before";
    EXPECT_EQ(open(cipher, "/link", "DL1", 3, plain, nonce), Opened::Malformed);
}

TEST(LinkEnvelope, fresh_refuses_the_stale_and_the_repeated)
{
    Fresh              fresh;
    const std::int64_t now = 1'791'223'792'000;
    EXPECT_TRUE(fresh.take(nonce_of(1), now - 5'000, now));
    EXPECT_FALSE(fresh.take(nonce_of(1), now - 5'000, now)) << "the same one again";
    EXPECT_FALSE(fresh.take(nonce_of(2), now - Fresh::kWindowMs - 1, now)) << "too old";
    EXPECT_FALSE(fresh.take(nonce_of(3), now + Fresh::kWindowMs + 1, now)) << "from too far ahead";
    EXPECT_TRUE(fresh.take(nonce_of(4), now + 2'000, now)) << "a clock a little ahead";
}

TEST(LinkEnvelope, parse_key)
{
    std::array<std::uint8_t, kKeyLen> key{};
    const std::string hex(64, 'a');
    ASSERT_TRUE(parse_key(hex.c_str(), key));
    EXPECT_EQ(key[0], 0xaa);
    EXPECT_FALSE(parse_key("abc", key)) << "too short";
    EXPECT_FALSE(parse_key(std::string(64, 'g').c_str(), key)) << "not hex";
    EXPECT_FALSE(parse_key(nullptr, key));
}
