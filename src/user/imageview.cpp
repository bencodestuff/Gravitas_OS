#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <gravity/image.h>

// imageview — a basic image viewer built on the gravity_image library.
//
// Opens argv[1] if given, otherwise the first picture in /Pictures, and
// shows it scaled to fit the window. Left/Right step through the other
// pictures in the same directory.

#define W        640
#define H        480
#define HEADER_H 20
#define MAXI     128

static struct gravity_app app;

static char dir[128];
static char paths[MAXI][192];
static const char* items[MAXI];
static int  nimages = 0;
static int  cur = 0;

static struct gravity_image img;
static int have_img = 0;

static const char* base_name(const char* p) {
    const char* b = p;
    for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static void show(int idx) {
    if (nimages == 0) return;
    if (idx < 0) idx = nimages - 1;
    if (idx >= nimages) idx = 0;
    cur = idx;

    if (have_img) { gravity_image_free(&img); have_img = 0; }
    if (gravity_image_load_mapped(&img, items[cur]) == 0) have_img = 1;
    printf("imageview: %s (%d/%d)\n", items[cur], cur + 1, nimages);
}

static int cmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void build_list(const char* start) {
    const char* base = 0;
    int n = 0;
    while (start && start[n]) n++;
    int slash = -1;
    for (int i = 0; i < n; i++) if (start[i] == '/') slash = i;

    if (slash >= 0) {
        int dlen = slash == 0 ? 1 : slash;
        for (int i = 0; i < dlen; i++) dir[i] = start[i];
        dir[dlen] = '\0';
        base = start + slash + 1;
    } else {
        const char* d = "/Pictures";
        int i = 0;
        while (d[i]) { dir[i] = d[i]; i++; }
        dir[i] = '\0';
    }

    static char listing[2048];
    grv_readdir(dir, listing, sizeof(listing));

    char* p = listing;
    while (*p && nimages < MAXI) {
        char* nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len > 0) {
            char name[80];
            int k = 0;
            for (int i = 0; i < len && k < 79; i++) name[k++] = p[i];
            name[k] = '\0';
            if (gravity_image_supported(name)) {
                int off = 0;
                for (int i = 0; dir[i] && off < 180; i++) paths[nimages][off++] = dir[i];
                if (off == 0 || paths[nimages][off - 1] != '/')
                    paths[nimages][off++] = '/';
                for (int i = 0; name[i] && off < 191; i++) paths[nimages][off++] = name[i];
                paths[nimages][off] = '\0';
                items[nimages] = paths[nimages];
                nimages++;
            }
        }
        p = *nl ? nl + 1 : nl;
    }

    // sort, then pick the requested file (else the first)
    for (int i = 1; i < nimages; i++) {
        const char* key = items[i];
        int j = i - 1;
        while (j >= 0 && cmp(key, items[j]) < 0) { items[j + 1] = items[j]; j--; }
        items[j + 1] = key;
    }
    int pick = 0;
    if (base) {
        for (int i = 0; i < nimages; i++)
            if (cmp(base_name(items[i]), base) == 0) { pick = i; break; }
    }
    show(pick);
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type != GRV_EV_KEY) return;
    long v = ev->a;
    if (v == GRV_KEY_LEFT || v == GRV_KEY_UP ||
        v == GRV_KEY_PAGEUP) {
        show(cur - 1);
        gravity_refresh(a);
    } else if (v == GRV_KEY_RIGHT || v == GRV_KEY_DOWN ||
               v == GRV_KEY_PAGEDOWN) {
        show(cur + 1);
        gravity_refresh(a);
    }
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_clear(c, 0xFF14181E);

    gravity_fill(c, 0, 0, W, HEADER_H, 0xFF2C3546);
    char cap[112];
    int n = 0;
    const char* b = nimages ? base_name(items[cur]) : "no images";
    while (b[n] && n < 70) { cap[n] = b[n]; n++; }
    if (nimages) {
        cap[n++] = ' ';
        cap[n++] = '(';
        int i1 = cur + 1, i2 = nimages;
        char t[8]; int k = 0;
        if (i1 >= 10) t[k++] = '0' + (i1 / 10);
        t[k++] = '0' + (i1 % 10); t[k++] = '/';
        char u[8]; int m = 0;
        if (i2 >= 10) u[m++] = '0' + (i2 / 10);
        u[m++] = '0' + (i2 % 10);
        for (int i = 0; i < k; i++) cap[n++] = t[i];
        for (int i = 0; i < m; i++) cap[n++] = u[i];
        cap[n++] = ')';
    }
    cap[n] = '\0';
    gravity_text(c, 6, (HEADER_H - gravity_font_height()) / 2, cap, 0xFFE6EEF6);

    if (!have_img) {
        gravity_text(c, W / 2 - 48, H / 2, "cannot open image",
                     0xFFC08080);
        return;
    }

    int areaY = HEADER_H, areaH = H - HEADER_H;
    unsigned long sx = ((unsigned long)W << 16) / (unsigned long)img.w;
    unsigned long sy = ((unsigned long)areaH << 16) / (unsigned long)img.h;
    unsigned long scale = sx < sy ? sx : sy;
    if (scale == 0) scale = 1;
    int dw = (int)(((unsigned long)img.w * scale) >> 16);
    int dh = (int)(((unsigned long)img.h * scale) >> 16);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (W - dw) / 2;
    int oy = areaY + (areaH - dh) / 2;

    static int colmap[4096], rowmap[4096];
    if (dw > 4096) dw = 4096;
    if (dh > 4096) dh = 4096;
    for (int x = 0; x < dw; x++) {
        int s = (int)(((unsigned long)x << 16) / scale);
        colmap[x] = s < 0 ? 0 : (s >= img.w ? img.w - 1 : s);
    }
    for (int y = 0; y < dh; y++) {
        int s = (int)(((unsigned long)y << 16) / scale);
        rowmap[y] = s < 0 ? 0 : (s >= img.h ? img.h - 1 : s);
    }

    unsigned int* px = c->px;
    for (int y = 0; y < dh; y++) {
        const unsigned int* srow = img.px + (unsigned long)rowmap[y] * (unsigned long)img.w;
        unsigned int* drow = px + (unsigned long)(oy + y) * W + ox;
        for (int x = 0; x < dw; x++) drow[x] = srow[colmap[x]];
    }
    gravity_invalidate(c, ox, oy, dw, dh);
}

int main(int argc, char** argv) {
    build_list(argc > 1 ? argv[1] : 0);

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.title = "Images";
    app.user = 0;
    gravity_run(&app, W, H);
    return 0;
}
