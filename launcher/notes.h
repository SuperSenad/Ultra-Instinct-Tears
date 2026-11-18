// Ultra Instinct Tears launcher: release notes for the "Update available" dialog ([UIT 0377]).
// A light Markdown subset as blocks the dialog lays out itself: headings, paragraphs (soft line breaks joined), bullet
// and numbered list items (continuation lines joined); **bold**, __bold__, `code`, links and images reduced to their
// text, HTML comments and tags dropped. Text that is not valid UTF-8 is read as Windows-1252 (what a Python helper
// writes to a pipe or file on Windows unless told otherwise), and characters the font cannot draw become an ASCII
// stand-in or nothing, never a '?'. No ImGui here: tools/tests/test_launcher_notes.cpp tests it on its own.
#pragma once
#include "mini.h"

namespace ut {

struct NoteBlock {
    enum Kind { Heading, Para, Bullet, Number } kind = Para;
    int level = 0;          // Heading: 1..6; Bullet / Number: 0, or 1 for a nested item
    mini::string marker;    // Bullet: the bullet; Number: "3."
    mini::string text;      // one line: the dialog wraps it
};

mini::string notes_utf8_fix(const mini::string &s);            // invalid UTF-8 bytes read as Windows-1252
const char *notes_ascii_for(unsigned cp);                      // an ASCII stand-in for common punctuation, or nullptr
// Keeps every character has(cp, ctx) says the font draws; the rest become notes_ascii_for or are dropped.
mini::string notes_glyph_safe(const mini::string &s, bool (*has)(unsigned cp, void *ctx), void *ctx);
mini::string notes_inline(const mini::string &s);              // inline Markdown reduced to its text
mini::vector<NoteBlock> notes_parse(const mini::string &md);
// Whole blocks from the top, up to max_chars of text and max_blocks blocks; a first block longer than the limit is cut
// at a sentence end. Never ends on a heading. *cut: something was left out.
mini::vector<NoteBlock> notes_excerpt(const mini::vector<NoteBlock> &b, size_t max_chars, size_t max_blocks, bool *cut);

}  // namespace ut
