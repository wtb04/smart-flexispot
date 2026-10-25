#include "glyph_fit.h"

#include <gtest/gtest.h>

#include <string>

namespace {
// As the kit's fonts: ASCII, Latin-1 and a few punctuation marks.
bool kit_has(std::uint32_t c)
{
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0x17E) || c == 0x2013 || c == 0x2014 || c == 0x2022 ||
           c == 0x2026;
}

std::string fit(const char *in, std::size_t size = 64)
{
    char out[64];
    ui::glyph_fit::fit(in, out, size, kit_has);
    return out;
}

TEST(GlyphFit, what_the_font_has_stays)
{
    EXPECT_EQ(fit("Café, Brötchen – 2…"), "Café, Brötchen – 2…");
}

TEST(GlyphFit, a_dot_between_words_is_a_comma)
{
    EXPECT_EQ(fit("RTL 5 · Kees van der Spek ontmaskert"), "RTL 5, Kees van der Spek ontmaskert")
        << "even as the font has it";
    EXPECT_EQ(fit("Café • Brötchen"), "Café, Brötchen");
    EXPECT_EQ(fit("Vandaag 20:27 \u2027 1u 34m"), "Vandaag 20:27, 1u 34m");
    EXPECT_EQ(fit("Vandaag 20:27\u22C51u 34m"), "Vandaag 20:27, 1u 34m");
    EXPECT_EQ(fit("\u2027 Live"), "Live") << "nothing to set apart from";
}

TEST(GlyphFit, a_space_of_another_width_is_a_space)
{
    EXPECT_EQ(fit("20:27\u202F1u\u2009 34m"), "20:27 1u 34m");
}

TEST(GlyphFit, anything_else_it_lacks_is_left_out)
{
    EXPECT_EQ(fit("Live \U0001F534 now"), "Live now");
    EXPECT_EQ(fit("\U0001F3B5 Song \u200B"), "Song");
    EXPECT_EQ(fit("bad \xFF byte"), "bad byte");
}

TEST(GlyphFit, line_breaks_stay)
{
    EXPECT_EQ(fit("Severance \u2027\nS2, E3"), "Severance\nS2, E3") << "a video's series, then its episode";
    EXPECT_EQ(fit("Show\n\u2027 S1"), "Show\nS1");
}

TEST(GlyphFit, never_half_a_character)
{
    EXPECT_EQ(fit("abé", 4), "ab") << "é takes two bytes, and the terminator one";
}
}  // namespace
