#include "font.h"

#include <limine.h>
#include "io.h"
#include "heap.h"
#include "../fs/vfs.h"
#include "../drivers/font.h"   // font8x8_basic — the embedded fallback

// --- Font module ---
// Mirrors the fb-access pattern of compositor.cpp: extern the limine
// request and derive a direct pointer. (-Isrc makes <limine.h> resolve
// to src/limine.h, same as framebuffer.cpp.)

extern volatile struct limine_framebuffer_request framebuffer_request;

// (declared in main.cpp / framebuffer.cpp; local externs per the
//  established pattern in vfs.cpp, tar.cpp, etc.)
extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// --- PSF2 (Linux console font) parsing ---
// The authoritative magic. A PSF2 file is: 32-byte header, then
// `length` glyphs of `charsize` bytes each (row-major, one row = one
// byte for 8-px-wide fonts, bits MSB-left), then an optional Unicode
// mapping table (we ignore it; we only need ASCII).
//
// Field-order note: the spec puts (length, width, height, charsize) at
// offsets 16..28, but the Bizcat file in the tree writes that run as
// (length, charsize, height, width) — 0xd5, 0x10, 0x10, 0x08. For a
// font whose glyph stride is `height` bytes (the only shape we accept:
// width==8, one byte/row), width and charsize are the two values that
// swap; both produce (8, 16, stride=16) for a correct 8x16 font.
// So we take length@16, then read the run @20..28 and pick the
// consistent interpretation rather than trusting one order.
struct Psf2 {
    const uint8_t* glyphs;   // start of glyph bitmap data
    uint32_t length;         // number of glyphs
    uint32_t width;          // pixels (we require 8)
    uint32_t height;         // rows per glyph
};

static bool parse_psf2(const uint8_t* data, uint64_t size, Psf2* out) {
    if (size < 32) return false;

    auto rd32 = [&](int off) -> uint32_t {
        return (uint32_t)data[off]
             | ((uint32_t)data[off + 1] << 8)
             | ((uint32_t)data[off + 2] << 16)
             | ((uint32_t)data[off + 3] << 24);
    };

    if (rd32(0) != 0x864ab572u) return false;   // PSF2 magic

    uint32_t headersize = rd32(8);
    uint32_t flags      = rd32(12);
    uint32_t length     = rd32(16);
    uint32_t f20        = rd32(20);
    uint32_t f24        = rd32(24);
    uint32_t f28        = rd32(28);
    (void)flags; // unicode table ignored

    // Bizcat's quirk: the generator emitted (charsize, height, width)
    // where the spec says (width, height, charsize). Accept either
    // interpretation, only when it is self-consistent for the one shape
    // we render: 8 px wide, one byte per row.
    uint32_t width = 0, height = 0, charsize = 0;
    if (f20 == 8 && f28 == f24) {            // spec order: (w,h,c) = (8,h,c)
        width = f20; height = f24; charsize = f28;
    } else if (f28 == 8 && f20 == f24) {     // Bizcat order: (c,h,w) = (16,16,8)
        width = f28; height = f24; charsize = f20;
    } else {
        return false;
    }

    // One byte per glyph row (8 px), stride == height.
    if (charsize != height || height == 0 || charsize == 0) return false;
    // Need every ASCII printable glyph to exist in range.
    if (length < 128) return false;
    // Glyph data + (header) must fit in the file.
    if (headersize < 32 || headersize > size) return false;
    if ((uint64_t)headersize + (uint64_t)length * charsize > size) return false;

    out->glyphs   = data + headersize;
    out->length   = length;
    out->width    = width;
    out->height   = height;
    return true;
}

// --- Active-font state ---
// Default: the embedded 8x8 (no init required; kprint works from the
// first instruction). A successful PSF2 load replaces these.

static const uint8_t* psf_glyphs   = nullptr;   // PSF2 mode
static uint32_t       psf_length   = 0;
static uint32_t       psf_height   = 0;

static bool active_psf2() { return psf_glyphs != nullptr; }

int Font::cell_w() { return 8; }

int Font::cell_h() { return active_psf2() ? (int)psf_height : 8; }

bool Font::psf2_active() { return active_psf2(); }

// --- Rendering ---

// Framebuffer geometry for direct row writes. Expects the caller to
// have verified bounds.
struct FbInfo {
    uint32_t* base;
    int width, height;
    uint64_t pitch_px;   // pitch / 4
};

static bool fb_ready(FbInfo* out) {
    if (framebuffer_request.response == nullptr ||
        framebuffer_request.response->framebuffer_count < 1) return false;
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    out->base     = (uint32_t*)(uintptr_t)fb->address;
    out->width    = (int)fb->width;
    out->height   = (int)fb->height;
    out->pitch_px = fb->pitch / 4;
    return true;
}

// Pointer to an active-font glyph for an ASCII code, or nullptr when
// the code has no glyph (blank cell).
static const uint8_t* glyph_for(char c) {
    if (c < 32 || c > 126) return nullptr;
    unsigned char code = (unsigned char)c;

    if (active_psf2()) {
        if ((uint32_t)code >= psf_length) return nullptr;
        return psf_glyphs + (uint64_t)code * psf_height;
    }
    return font8x8_basic[code - 32];
}

int Font::draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    FbInfo fb;
    if (!fb_ready(&fb)) return x;

    // Fully off-screen horizontally: still return the advance so
    // callers can run off to the edge safely.
    if (x + 8 <= 0 || x >= fb.width) return x + 8;

    const uint8_t* glyph = glyph_for(c);

    for (int row = 0; row < cell_h(); row++) {
        if (y + row < 0 || y + row >= fb.height) continue;
        uint32_t* px = fb.base + (uint64_t)(y + row) * fb.pitch_px + x;

        // The one row of glyph bits (PSF2 and 8x8 both: 1 byte, MSB left)
        uint8_t bits = glyph ? glyph[row] : (uint8_t)0;

        for (int col = 0; col < 8; col++, px++) {
            if (x + col < 0 || x + col >= fb.width) continue;
            *px = (bits & (0x80u >> col)) ? fg : bg;
        }
    }
    return x + 8;
}

int Font::draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    while (*s) x = draw_char(x, y, *s++, fg, bg);
    return x;
}

// --- Boot-time font from the tarfs ---

bool Font::load_from_disk() {
    const char* path = "/bin/fonts/bizcat.psf";

    int fd = VFS::open_fd(path, VFS::O_RDONLY);
    if (fd < 0) {
        kprint("[font] no /bin/fonts/bizcat.psf; keeping 8x8\n");
        return false;
    }

    uint64_t size = VFS::fd_size(fd);

    // A PSF2: 32-byte header plus at least 128 glyphs of >= 8 bytes.
    if (size < 32 + 128 * 8) {
        kprint("[font] font file too small; keeping 8x8\n");
        VFS::close_fd(fd);
        return false;
    }

    // Parse in place from the kernel heap; intentionally leaked — the
    // font stays in use for the life of the kernel.
    uint8_t* data = (uint8_t*)Heap::malloc(size);
    if (data == nullptr) {
        kprint("[font] OOM loading font; keeping 8x8\n");
        VFS::close_fd(fd);
        return false;
    }

    long got = VFS::read_fd(fd, data, size);
    VFS::close_fd(fd);

    if (got != (long)size) {
        kprint("[font] short read; keeping 8x8\n");
        return false;
    }

    Psf2 psf;
    if (!parse_psf2(data, size, &psf)) {
        kprint("[font] bad PSF2 (unsupported layout); keeping 8x8\n");
        return false;
    }

    psf_glyphs  = psf.glyphs;
    psf_length  = psf.length;
    psf_height  = psf.height;

    kprint("[font] loaded bizcat.psf 8x");
    kprint_uint(psf_height);
    kprint(" from tarfs\n");
    return true;
}
