#ifndef _GRAVITY_IMAGE_H
#define _GRAVITY_IMAGE_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Image decoding for userland: decode a bitmap file into a 32bpp buffer of
   opaque 0xFFRRGGBB pixels. The API is format-agnostic; today it reads BMP
   (24- and 32-bit uncompressed), and more formats can be added behind
   gravity_image_load() without touching callers. */

struct gravity_image {
    unsigned int* px;   /* w*h pixels, malloc'd — free with gravity_image_free */
    int w, h;
};

/* Decode 'path' into 'img'. Returns 0 on success (the caller then owns the
   buffer), or -1 (unsupported format / read or decode failure). For a file
   that has more than one frame, only the first is returned. */
int  gravity_image_load(struct gravity_image* img, const char* path);

/* Like gravity_image_load, but mapping the file instead of reading it when the
   filesystem can: a picture in the immutable image is decoded in place, with no
   copy of its bytes. Falls back to a plain read. */
int  gravity_image_load_mapped(struct gravity_image* img, const char* path);

/* Decode bytes that are already in memory (a mapped file, say). */
int  gravity_image_decode(struct gravity_image* img, const void* data,
                          unsigned long size);

/* Release a loaded image. Safe on an already-free image. */
void gravity_image_free(struct gravity_image* img);

/* True if 'path' has an extension gravity_image_load() can decode (no I/O). */
int  gravity_image_supported(const char* path);

__END_DECLS

#endif
