#ifndef _GRAVITY_H
#define _GRAVITY_H

/* Gravity — the Gravitas userland graphics/text library: a surface-backed
   canvas with damage-tracked present, an event loop, a small widget set, and
   the shared primitives (font blitting, image decoding, an editable text
   buffer).

   The split: <grv/...> is the kernel-facing contract (syscalls, input event
   wire format, WM protocol messages) — everything here builds on it and
   nothing here is part of the ABI. */

#include <gravity/canvas.h>
#include <gravity/app.h>
#include <gravity/widget.h>
#include <gravity/list.h>
#include <gravity/filer.h>
#include <gravity/term.h>
#include <gravity/buffer.h>
#include <gravity/editor.h>
#include <gravity/font.h>
#include <gravity/image.h>

#endif
