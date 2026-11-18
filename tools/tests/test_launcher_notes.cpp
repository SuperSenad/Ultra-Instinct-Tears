// [UIT 0377] The launcher's release notes blocks (launcher/notes.cpp): soft line breaks joined, headings, lists,
// inline Markdown, Unicode (never a '?'), the excerpt cut at a block boundary.
// build (repo root; clang -x c++ links without a C++ library): clang -x c++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-exceptions -fno-rtti -nostdinc++ \
//        -Ilauncher tools/tests/test_launcher_notes.cpp launcher/notes.cpp -o /tmp/test_launcher_notes && /tmp/test_launcher_notes
#include "notes.h"
#include <stdio.h>
#include <string.h>

using namespace ut;
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define EQ(a, b) do { checks++; mini::string _a = (a); if (strcmp(_a.c_str(), (b))) { printf("FAIL %s:%d %s\n  got  \"%s\"\n  want \"%s\"\n", __FILE__, __LINE__, #a, _a.c_str(), (b)); fails++; } } while (0)

static bool ascii_font(unsigned, void *) { return false; }               // a font with no glyph past ASCII
static bool inter_like(unsigned cp, void *) { return cp == 0x2013 || cp == 0x2026 || cp == 0x2022; }

int main() {
    // soft line breaks: one paragraph, one line
    auto b = notes_parse("Test build 5 plus a fix,\nthe no-copy lists on,\n  and a clearer screen.\n\nSecond paragraph.");
    CHECK(b.size() == 2);
    EQ(b[0].text, "Test build 5 plus a fix, the no-copy lists on, and a clearer screen.");
    CHECK(b[0].kind == NoteBlock::Para && b[1].kind == NoteBlock::Para);
    EQ(b[1].text, "Second paragraph.");
    CHECK(notes_parse("a\r\nb\r\n").size() == 1 && notes_parse("a\r\nb\r\n")[0].text == "a b");

    // headings: levels, closing #s, a heading ends the paragraph above
    b = notes_parse("# Title\nIntro\n## What changed ##\n### Small\n#notaheading");
    CHECK(b.size() == 5);
    CHECK(b[0].kind == NoteBlock::Heading && b[0].level == 1); EQ(b[0].text, "Title");
    CHECK(b[1].kind == NoteBlock::Para);
    CHECK(b[2].kind == NoteBlock::Heading && b[2].level == 2); EQ(b[2].text, "What changed");
    CHECK(b[3].level == 3);
    CHECK(b[4].kind == NoteBlock::Para); EQ(b[4].text, "#notaheading");

    // lists: bullets and numbers, continuation lines joined into the item, nested items
    b = notes_parse("- **[UIT 0374] Pieces** find each other.\n  The compiler refused them.\n  Setting: CPU > Split.\n* two\n+ three\n  - nested\n1. first\n   still first\n12) twelfth\n\n3.5 is not a list");
    CHECK(b.size() == 7);
    CHECK(b[0].kind == NoteBlock::Bullet && b[0].level == 0);
    EQ(b[0].text, "[UIT 0374] Pieces find each other. The compiler refused them. Setting: CPU > Split.");
    EQ(b[0].marker, "\xE2\x80\xA2");
    CHECK(b[1].kind == NoteBlock::Bullet && b[2].kind == NoteBlock::Bullet);
    CHECK(b[3].kind == NoteBlock::Bullet && b[3].level == 1); EQ(b[3].text, "nested");
    CHECK(b[4].kind == NoteBlock::Number); EQ(b[4].marker, "1."); EQ(b[4].text, "first still first");
    EQ(b[5].marker, "12."); EQ(b[5].text, "twelfth");
    CHECK(b[6].kind == NoteBlock::Para);   // "3.5 is..." has no space after the dot: a paragraph line

    // inline Markdown
    EQ(notes_inline("**Bold** and __bold__, `TOTK_LIFT_SPLIT_HUGE`, [the page](https://x.y/z) ![pic](a.png)"),
       "Bold and bold, TOTK_LIFT_SPLIT_HUGE, the page");
    EQ(notes_inline("*em* and 2 * 3 = 6, a*b"), "em and 2 * 3 = 6, a*b");
    EQ(notes_inline("x<br>y <b>z</b> <https://a.b> a < b"), "x y z https://a.b a < b");
    EQ(notes_inline("\\*not em\\*  spaced   out"), "*not em* spaced out");

    // comments, rules, fences, the checksum line
    b = notes_parse("<!-- hidden\nlines -->Shown\n\n---\n\n```\ncode line\n```\nSHA-256 (UltraInstinctTears-src.zip): 4d66d106a705c4f872ab3049a41da013301c1f80ad81edf5fe563c85386de2fd\n<!-- unclosed");
    CHECK(b.size() == 2);
    EQ(b[0].text, "Shown");
    EQ(b[1].text, "code line");

    // Unicode: valid UTF-8 kept, Windows-1252 bytes repaired, missing glyphs as ASCII, never '?'
    EQ(notes_utf8_fix("1\xE2\x80\x93" "5"), "1\xE2\x80\x93" "5");
    EQ(notes_utf8_fix("1\x96" "5 \x93q\x94 caf\xE9"), "1\xE2\x80\x93" "5 \xE2\x80\x9Cq\xE2\x80\x9D caf\xC3\xA9");
    EQ(notes_glyph_safe("1\xE2\x80\x93" "5 \xE2\x80\x9Cq\xE2\x80\x9D it\xE2\x80\x99s\xE2\x80\xA6 72 \xE2\x86\x92 77 \xE2\x89\x88" "1", ascii_font, nullptr),
       "1-5 \"q\" it's... 72 -> 77 ~1");
    EQ(notes_glyph_safe("a\xE2\x80\x93" "b\xE2\x80\xA6 \xF0\x9F\x8E\x89!", inter_like, nullptr), "a\xE2\x80\x93" "b\xE2\x80\xA6 !");   // an emoji: dropped
    EQ(notes_glyph_safe("x\x96y", ascii_font, nullptr), "x-y");
    CHECK(notes_glyph_safe("\xE2\x80\x94\xE2\x80\x98\xE2\x86\x90\xE2\x87\x92\xC2\xA0", ascii_font, nullptr).find("?") == mini::string::npos);
    b = notes_parse("# Ultra Instinct Tears 1.0.3 \xE2\x80\x94 test build 6\nbuilds 1\x96" "5");
    EQ(b[0].text, "Ultra Instinct Tears 1.0.3 \xE2\x80\x94 test build 6");
    EQ(b[1].text, "builds 1\xE2\x80\x93" "5");

    // excerpt: whole blocks, never a trailing heading, cut flagged
    b = notes_parse("# T\n\nPara one is here.\n\n## Next\n\n- a\n- b\n\n## Install\n\nLong install text.");
    bool cut = false;
    auto e = notes_excerpt(b, 1000, 20, &cut);
    CHECK(e.size() == b.size() && !cut);
    e = notes_excerpt(b, 1000, 3, &cut);   // T, Para, Next: the heading at the end goes
    CHECK(e.size() == 2 && cut && e.back().kind == NoteBlock::Para);
    e = notes_excerpt(b, 23, 20, &cut);   // T(1) + Para(17) + Next(4) + a(1) fit, b does not
    CHECK(cut && e.back().kind == NoteBlock::Bullet);
    mini::vector<NoteBlock> one = notes_parse("First sentence is fine. Second sentence runs on and on and on and on.");
    e = notes_excerpt(one, 40, 20, &cut);
    CHECK(cut && e.size() == 1); EQ(e[0].text, "First sentence is fine.");
    one = notes_parse("averyveryverylongwordthatgoes on and on without any sentence end at all here");
    e = notes_excerpt(one, 40, 20, &cut);
    CHECK(cut); EQ(e[0].text, "averyveryverylongwordthatgoes on and on...");
    e = notes_excerpt(notes_parse("\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94"), 4, 20, &cut);   // never half a character
    CHECK(e.size() == 1 && notes_utf8_fix(e[0].text) == e[0].text);
    CHECK(notes_parse("").empty() && notes_parse("\n\n  \n").empty());

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
