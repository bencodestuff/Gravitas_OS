#pragma once
#include <stdint.h>

// --- Console: the kernel text screen as a cell model ---
//
// The console is SEMANTIC, not pixels: a grid of cells, each a character
// plus a foreground/background color, with the exact putc semantics the
// old screen printer had ('\n', '\b', wrap, scroll). It owns no
// framebuffer and writes no pixels — Compositor paints the grid as the
// deepest layer, beneath every surface, so screen text and windows can
// never fight over the same pixels.
//
// Storage is a STATIC grid (BSS): the console must accept output from
// the very first kprint in boot, long before the heap exists, and it
// lives for the kernel's lifetime. Its size is bounded by the
// framebuffer and the active font (<= MAX_COLS x MAX_ROWS); the live
// grid is derived in init() and again whenever the font changes.

namespace Console {

    // 16-color palette ids (VGA-ish ordering).
    enum : uint8_t {
        BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LT_GREY,
        DK_GREY, LT_BLUE, LT_GREEN, LT_CYAN, LT_RED, LT_MAGENTA, YELLOW, WHITE
    };

    // (Re)derive the grid from the framebuffer geometry and the active
    // font, then clear it and reset the cursor. Called lazily by print()
    // and again on a font change.
    void init();

    // Feed text: identical semantics to the old framebuffer printer.
    void print(const char* s);

    // Set the ink for subsequent cells. Default: WHITE on BLACK.
    void ink(uint8_t fg, uint8_t bg);

    // Clear the grid (same geometry, cursor back to the top-left).
    void clear();

    // --- Geometry + cell access for the compositor's painters ---
    int cols();
    int rows();
    char cell(int col, int row);
    uint32_t cell_fg(int col, int row);  // resolved 0x00RRGGBB
    uint32_t cell_bg(int col, int row);  // resolved 0x00RRGGBB
    uint32_t background();               // the fill behind/between cells

    // --- Dirty tracking ---
    // A row is dirty while cells in [c0, c1] have changed since it was
    // cleaned. The compositor paints that span and calls clean_row().
    bool row_dirty(int row, int* c0, int* c1);
    void clean_row(int row);
    void clean_all();
}
