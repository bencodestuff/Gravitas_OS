#include "kernel/gfx/console.h"
#include "kernel/gfx/font.h"
#include "kernel/klib/klib.h"
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

// See console.h for the model. The grid is static (BSS) so the console
// works from the first boot print, before the heap or the scheduler.

namespace Console {

constexpr int MAX_COLS = 256;
constexpr int MAX_ROWS = 160;

static char cells[MAX_ROWS * MAX_COLS];
static uint8_t attrs[MAX_ROWS * MAX_COLS]; // fg << 4 | bg

static int grid_cols = 0;
static int grid_rows = 0;
static int cur_x = 0, cur_y = 0;
static uint8_t cur_fg = WHITE, cur_bg = BLACK;

struct RowDirty { bool any; int c0, c1; };
static RowDirty dirty[MAX_ROWS];

static const uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static inline int attrs_of(int fg, int bg) { return (fg << 4) | bg; }

static void mark_dirty(int col, int row) {
    RowDirty* d = &dirty[row];
    if (!d->any) { d->any = true; d->c0 = col; d->c1 = col; }
    else {
        if (col < d->c0) d->c0 = col;
        if (col > d->c1) d->c1 = col;
    }
}

static void mark_all_dirty() {
    for (int r = 0; r < grid_rows; r++) {
        dirty[r].any = true;
        dirty[r].c0 = 0;
        dirty[r].c1 = grid_cols - 1;
    }
}

static void put_cell(int col, int row, char c) {
    int i = row * grid_cols + col;
    cells[i] = c;
    attrs[i] = (uint8_t)attrs_of(cur_fg, cur_bg);
    mark_dirty(col, row);
}

static void blank_span(char* dst, uint8_t* adst, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = ' ';
        adst[i] = (uint8_t)attrs_of(cur_fg, cur_bg);
    }
}

// Shift every row up one line and blank the last (cell memmove — the old
// pixel-level scroll is gone).
static void scroll() {
    int n = (grid_rows - 1) * grid_cols;
    memmove(cells, cells + grid_cols, n);
    memmove(attrs, attrs + grid_cols, n);
    blank_span(cells + n, attrs + n, grid_cols);
    mark_all_dirty();
}

void init() {
    uint64_t fw = 0, fh = 0;
    if (framebuffer_request.response != nullptr &&
        framebuffer_request.response->framebuffer_count >= 1) {
        fw = framebuffer_request.response->framebuffers[0]->width;
        fh = framebuffer_request.response->framebuffers[0]->height;
    }

    int cw = Font::cell_w();
    int ch = Font::cell_h();
    if (cw <= 0) cw = 8;
    if (ch <= 0) ch = 8;

    grid_cols = (int)(fw / (uint64_t)cw);
    grid_rows = (int)(fh / (uint64_t)ch);
    if (grid_cols > MAX_COLS) grid_cols = MAX_COLS;
    if (grid_rows > MAX_ROWS) grid_rows = MAX_ROWS;
    if (grid_cols < 1) grid_cols = 1;
    if (grid_rows < 1) grid_rows = 1;

    blank_span(cells, attrs, grid_rows * grid_cols);
    cur_x = 0;
    cur_y = 0;
    mark_all_dirty();
}

void clear() {
    blank_span(cells, attrs, grid_rows * grid_cols);
    cur_x = 0;
    cur_y = 0;
    mark_all_dirty();
}

void ink(uint8_t fg, uint8_t bg) {
    cur_fg = fg;
    cur_bg = bg;
}

void print(const char* s) {
    if (grid_cols == 0) init(); // first kprint builds the grid lazily
    if (s == nullptr) return;

    for (int i = 0; s[i] != '\0'; i++) {
        char c = s[i];

        if (c == '\b') {
            if (cur_x > 0) {
                cur_x--;
                put_cell(cur_x, cur_y, ' ');
            }
            continue;
        }

        if (c == '\r') {
            // Back to the start of the line, leaving what is there: a reader
            // that edits in place redraws by returning here and printing again.
            cur_x = 0;
            continue;
        }

        if (c == '\n') {
            cur_x = 0;
            cur_y++;
            if (cur_y >= grid_rows) { cur_y = grid_rows - 1; scroll(); }
            continue;
        }

        if (c >= 32 && c <= 126) {
            put_cell(cur_x, cur_y, c);
            cur_x++;
            if (cur_x >= grid_cols) {
                cur_x = 0;
                cur_y++;
                if (cur_y >= grid_rows) { cur_y = grid_rows - 1; scroll(); }
            }
        }
        // other control bytes are swallowed, exactly like the old printer
    }
}

int cols() { return grid_cols; }
int rows() { return grid_rows; }

char cell(int col, int row) { return cells[row * grid_cols + col]; }
uint32_t cell_fg(int col, int row) { return palette[attrs[row * grid_cols + col] >> 4]; }
uint32_t cell_bg(int col, int row) { return palette[attrs[row * grid_cols + col] & 0x0F]; }
uint32_t background() { return palette[BLACK]; }

bool row_dirty(int row, int* c0, int* c1) {
    if (row < 0 || row >= grid_rows || !dirty[row].any) return false;
    if (c0) *c0 = dirty[row].c0;
    if (c1) *c1 = dirty[row].c1;
    return true;
}

void clean_row(int row) {
    if (row >= 0 && row < MAX_ROWS) dirty[row].any = false;
}

void clean_all() {
    for (int r = 0; r < MAX_ROWS; r++) dirty[r].any = false;
}

} // namespace Console
