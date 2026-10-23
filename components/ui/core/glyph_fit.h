#pragma once

#include <cstddef>
#include <cstdint>

// Text from outside, a page's or a player's, in a font that lacks some of
// what it holds: drawn as it is, each such character was an empty box. And
// its dots between words, which the screen never shows.
namespace ui::glyph_fit {

namespace detail {
inline bool space_like(std::uint32_t c)
{
    return (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Dots that set words apart: never shown, as the screen joins with commas.
inline bool separator_like(std::uint32_t c)
{
    return c == 0x00B7 || c == 0x2022 || c == 0x2027 || c == 0x2219 || c == 0x22C5 || c == 0x2981 || c == 0x2E31 || c == 0x30FB ||
           c == 0xFF65 || c == 0x25CF || c == 0x25AA || c == 0x2023 || c == 0x2043 || c == 0x2024;
}

// One UTF-8 character at `in`: its code point, and how many bytes; 0 for a
// byte that begins none.
inline int decode(const unsigned char *in, std::uint32_t &c)
{
    if (in[0] < 0x80) {
        c = in[0];
        return 1;
    }
    const int length = (in[0] & 0xE0) == 0xC0 ? 2 : (in[0] & 0xF0) == 0xE0 ? 3 : (in[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (length == 0) {
        return 0;
    }
    c = in[0] & (0x7F >> length);
    for (int i = 1; i < length; ++i) {
        if ((in[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = (c << 6) | (in[i] & 0x3F);
    }
    return length;
}
}  // namespace detail

/** `in` into `out` with what `has(code point)` says the font lacks put right:
 *  a space of another width as a space, anything else left out, and the
 *  spaces that leaves behind tidied. A dot between words is a comma whether
 *  the font has it or not. Line breaks stay. */
template <typename Has>
void fit(const char *in, char *out, std::size_t size, Has has)
{
    if (size == 0) {
        return;
    }
    std::size_t n = 0;
    const auto  put = [&](char ch) {
        if (n + 1 < size) {
            out[n++] = ch;
        }
    };
    const auto trim = [&] {
        while (n > 0 && out[n - 1] == ' ') {
            --n;
        }
    };
    bool        apart = false;  // a separator lacked, put as a comma once a word follows
    const auto *at    = reinterpret_cast<const unsigned char *>(in != nullptr ? in : "");
    while (*at != 0) {
        std::uint32_t c      = 0;
        const int     length = detail::decode(at, c);
        if (length == 0) {
            ++at;
            continue;
        }
        const bool space = c == ' ' || detail::space_like(c);
        if (c == '\n') {
            trim();
            put('\n');  // a break between lines, which no font draws
            apart = false;
        } else if (space) {
            if (!apart && n > 0 && out[n - 1] != ' ' && out[n - 1] != '\n') {
                put(' ');
            }
        } else if (detail::separator_like(c)) {
            trim();
            apart = n > 0 && out[n - 1] != '\n';
        } else if (has(c)) {
            if (apart) {
                put(',');
                put(' ');
                apart = false;
            }
            if (n + length >= size) {
                break;  // never half a character
            }
            for (int i = 0; i < length; ++i) {
                out[n++] = static_cast<char>(at[i]);
            }
        }
        at += length;
    }
    trim();
    out[n] = '\0';
}

}  // namespace ui::glyph_fit
