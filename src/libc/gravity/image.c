#include <gravity/image.h>
#include <grv/syscall.h>
#include <stdlib.h>

// Image decoding. The whole file is read into a heap buffer first (there is
// no fstat), then handed to the decoder the magic bytes select.

#define MAX_IMAGE_BYTES (32ul << 20) // 32 MB cap: a clip on runaway input

static unsigned long rd16(const unsigned char* d, int o) {
    return (unsigned long)d[o] | ((unsigned long)d[o + 1] << 8);
}

static unsigned long rd32(const unsigned char* d, int o) {
    return (unsigned long)d[o] | ((unsigned long)d[o + 1] << 8)
         | ((unsigned long)d[o + 2] << 16) | ((unsigned long)d[o + 3] << 24);
}

// Read a whole file, growing the buffer as needed. Returns a malloc'd buffer
// (caller frees) and sets *out_size, or 0 on open/alloc failure. A file that
// would exceed the cap is truncated to it.
static unsigned char* read_file(const char* path, unsigned long* out_size) {
    int fd = grv_open(path, GRV_O_RDONLY);
    if (fd < 0) return 0;

    unsigned long cap = 1ul << 16;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (!buf) { grv_close(fd); return 0; }

    unsigned long off = 0;
    for (;;) {
        if (off == cap) {
            if (cap >= MAX_IMAGE_BYTES) break;
            unsigned long ncap = cap * 2;
            if (ncap > MAX_IMAGE_BYTES) ncap = MAX_IMAGE_BYTES;
            unsigned char* nbuf = (unsigned char*)malloc(ncap);
            if (!nbuf) break;
            for (unsigned long i = 0; i < off; i++) nbuf[i] = buf[i];
            free(buf);
            buf = nbuf;
            cap = ncap;
        }
        long n = grv_read_fd(fd, buf + off, cap - off);
        if (n <= 0) break;
        off += (unsigned long)n;
    }
    grv_close(fd);
    *out_size = off;
    return buf;
}

// BMP: 24- or 32-bit uncompressed, bottom-up (positive height) or top-down.
static int decode_bmp(struct gravity_image* img, const unsigned char* d,
                      unsigned long size) {
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return -1;

    unsigned long off = rd32(d, 10);
    long iw = (long)(int)rd32(d, 18);
    long ih = (long)(int)rd32(d, 22);
    int bpp = (int)rd16(d, 28);
    unsigned long comp = rd32(d, 30);
    if (iw <= 0 || ih == 0 || comp != 0) return -1;
    if (bpp != 24 && bpp != 32) return -1;

    int topdown = (ih < 0);
    if (ih < 0) ih = -ih;

    unsigned long px_bytes = (unsigned long)bpp / 8;
    unsigned long stride = ((unsigned long)iw * px_bytes + 3) & ~3ul;
    if (off > size || stride * (unsigned long)ih > size - off) return -1;

    unsigned int* px = (unsigned int*)malloc((unsigned long)iw * (unsigned long)ih
                                             * sizeof(unsigned int));
    if (!px) return -1;

    for (long y = 0; y < ih; y++) {
        const unsigned char* srow =
            d + off + (unsigned long)(topdown ? y : (ih - 1 - y)) * stride;
        unsigned int* drow = px + (unsigned long)y * (unsigned long)iw;
        for (long x = 0; x < iw; x++) {
            const unsigned char* p = srow + (unsigned long)x * px_bytes;
            drow[x] = 0xFF000000u | ((unsigned int)p[2] << 16)
                    | ((unsigned int)p[1] << 8) | (unsigned int)p[0];
        }
    }
    img->px = px;
    img->w = (int)iw;
    img->h = (int)ih;
    return 0;
}

int gravity_image_load(struct gravity_image* img, const char* path) {
    img->px = 0;
    img->w = 0;
    img->h = 0;

    unsigned long size = 0;
    unsigned char* d = read_file(path, &size);
    if (!d) return -1;

    int rc = -1;
    if (size >= 2 && d[0] == 'B' && d[1] == 'M')
        rc = decode_bmp(img, d, size);
    // More formats plug in here (check their magic, decode into img).

    free(d);
    return rc;
}

void gravity_image_free(struct gravity_image* img) {
    if (img->px) free(img->px);
    img->px = 0;
    img->w = 0;
    img->h = 0;
}

int gravity_image_supported(const char* path) {
    int n = 0;
    while (path[n]) n++;
    if (n < 4) return 0;
    const char* e = path + n - 4;
    return e[0] == '.' &&
           (e[1] == 'b' || e[1] == 'B') &&
           (e[2] == 'm' || e[2] == 'M') &&
           (e[3] == 'p' || e[3] == 'P');
}
