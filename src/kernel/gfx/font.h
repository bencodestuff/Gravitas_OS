#pragma once
#include <stdint.h>

// --- Font module ---
// Owns the ACTIVE console font and the low-level glyph renderer.
//
// Two sources:
//   * an embedded 8x8 VGA-style font (font8x8.h) — the default,
//     so early-boot kprint output works before the filesystem exists;
//   * a PSF2 font loaded from the tarfs at boot (load_from_disk), which
//     replaces it mid-boot (8x16 Bizcat today).
//
// Only 8-pixel-wide fonts are supported (1 byte per glyph row), which
// covers both. Glyphs are addressed linearly (PSF2 index == codepoint)
// for ASCII; anything outside [32,126] renders as a blank cell.
//
// Rendering writes rows directly through the framebuffer pointer (same
// access pattern as Compositor and the console scroll path) — one write
// per pixel-run, not one call per pixel.

namespace Font {

    // Metrics of the active font (usable immediately; no init needed).
    int cell_w();
    int cell_h();

    // Install a PSF2 font from a raw file image (parsed in place — the
    // buffer must outlive this module, e.g. a kernel-heap allocation).
    // Returns true and installs the font on success; on failure the
    // previously active font (usually the 8x8 fallback) stays active.
    bool use_psf2(const uint8_t* data, uint64_t size);

    // Boot-time load: read a PSF2 font from the tarfs (via the VFS) and
    // install it. Never fatal — on any failure it logs and keeps the
    // embedded 8x8. Returns true if a PSF2 font is now active.
    bool load_from_disk(const char* path = "/fonts/bizcat.psf");

    // True if a PSF2 font is active (diagnostics only).
    bool psf2_active();

    // Render one glyph cell at pixel (x, y). 'code' outside [32,126]
    // (or '\0') draws a fully blank cell (bg only) — that's how the
    // console erases glyphs. Returns the next x (x + cell width).
    int draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

    // Redirect glyph rendering to a compositor back buffer (same geometry as
    // the framebuffer). Pass nullptr to render straight to the framebuffer.
    void set_target(uint32_t* base, uint64_t pitch_px, int width, int height);

    // Render an ASCII string left to right, one cell per char.
    // Returns the x after the last glyph.
    int draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg);
}
