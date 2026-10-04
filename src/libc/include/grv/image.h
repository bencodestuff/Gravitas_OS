#ifndef _GRV_IMAGE_H
#define _GRV_IMAGE_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Image decoding for userland: decode a bitmap file into a 32bpp buffer of
   opaque 0xFFRRGGBB pixels. The API is format-agnostic; today it reads BMP
   (24- and 32-bit uncompressed), and more formats can be added behind
   grv_image_load() without touching callers. */

struct grv_image {
    unsigned int* px;   /* w*h pixels, malloc'd — free with grv_image_free */
    int w, h;
};

/* Decode 'path' into 'img'. Returns 0 on success (the caller then owns the
   buffer), or -1 (unsupported format / read or decode failure). For a file
   that has more than one frame, only the first is returned. */
int  grv_image_load(struct grv_image* img, const char* path);

/* Release a loaded image. Safe on an already-free image. */
void grv_image_free(struct grv_image* img);

/* True if 'path' has an extension grv_image_load() can decode (no I/O). */
int  grv_image_supported(const char* path);

__END_DECLS

#endif
