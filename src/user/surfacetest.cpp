#include <stdio.h>
#include <grv/syscall.h>

// surfacetest — M4 acceptance: surface lifecycle, z-order, enumeration,
// the owner check, death cleanup, and the registry cap/recycle.
// Partner program: src/user/surfhelper.cpp (spawned in the background).

#define MAXS 40
static struct grv_surface_info recs[MAXS];

static void die(const char* what) {
    printf("[surfacetest] FAIL: %s\n", what);
    grv_exit(1);
}

static int list_all(void) {
    int n = grv_surface_list(recs, MAXS);
    if (n < 0) die("surface_list returned -1");
    return n;
}

// The list must contain exactly 'ids' bottom-to-top, each with z == index.
static void expect_order(const int* ids, int n, const char* what) {
    if (list_all() != n) die(what);
    for (int i = 0; i < n; i++) {
        if ((int)recs[i].id != ids[i]) die(what);
        if ((int)recs[i].z != i) die(what);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    struct grv_surface_info s;

    if (list_all() != 0) die("pre-existing surfaces at test start");

    // --- creation order seeds the z-order, and the record is complete ---
    int id1 = grv_surface_create(64, 64, &s);
    if (id1 < 0 || s.width != 64 || s.height != 64 ||
        (int)s.id != id1 || s.z != 0 || s.address == 0) {
        die("create record");
    }
    int id2 = grv_surface_create(64, 64, &s);
    int id3 = grv_surface_create(64, 64, &s);
    if (id2 <= id1 || id3 <= id2) die("ids not monotonic");
    int born[3] = {id1, id2, id3};
    expect_order(born, 3, "creation order");

    // --- restack: raise to top, lower to bottom, exact position ---
    if (grv_surface_raise(id1) != 0) die("raise");
    int up[3] = {id2, id3, id1};
    expect_order(up, 3, "raise to top");

    if (grv_surface_lower(id3) != 0) die("lower");
    int low[3] = {id3, id2, id1};
    expect_order(low, 3, "lower to bottom");

    if (grv_surface_z(id1, 1) != 0) die("z exact");
    int mid[3] = {id3, id1, id2};
    expect_order(mid, 3, "z exact position");

    // --- destroy releases the entry; bad ids are refused ---
    if (grv_surface_destroy(id1) != 0) die("destroy own");
    if (grv_surface_destroy(id1) != -1) die("double destroy not refused");
    if (grv_surface_destroy(9999) != -1) die("unknown destroy not refused");
    if (grv_surface_z(9999, 0) != -1) die("unknown restack not refused");

    // --- a new surface starts on top, not in the recycled slot's place ---
    int id4 = grv_surface_create(64, 64, &s);
    if (id4 <= id3) die("id not monotonic after recycle");
    int after[3] = {id3, id2, id4};
    expect_order(after, 3, "recycled slot goes to top");

    // --- cross-task: another owner's surface is untouchable ---
    const char* harg[] = {"surfhelper.elf"};
    long H = grv_spawn_bg("surfhelper.elf", harg, 1);
    if (H <= 0) die("spawn_bg surfhelper");
    grv_sleep(200); // let the helper create its surface

    int n = list_all();
    int hid = -1;
    for (int i = 0; i < n; i++) {
        if ((int)recs[i].owner == (int)H) { hid = (int)recs[i].id; break; }
    }
    if (hid < 0) die("helper surface not visible in list");
    if (grv_surface_destroy(hid) != -1) die("foreign destroy allowed");
    if (grv_surface_z(hid, 0) != -1) die("foreign restack allowed");
    if (grv_surface_resize(hid, 32, 32, &s) != -1) die("foreign resize allowed");

    // --- death releases the surface ---
    if (grv_wait(H) != 0) die("helper did not exit 0");
    if (list_all() != 3) die("death did not release the surface");
    int stable[3] = {id3, id2, id4};
    expect_order(stable, 3, "order after helper death");

    // --- resize: the buffer moves, the id/position/z-order do not ---
    if (grv_surface_resize(id2, 128, 32, &s) != 0) die("resize own");
    if ((int)s.id != id2 || s.width != 128 || s.height != 32 || s.address == 0)
        die("resize record");
    // The new buffer must be mapped and writable in this address space.
    volatile unsigned int* px = (volatile unsigned int*)s.address;
    px[0] = 0xDEADBEEF;
    px[128 * 31 + 127] = 0xCAFEBABE;   // the last pixel of the new size
    if (px[0] != 0xDEADBEEF || px[128 * 31 + 127] != 0xCAFEBABE)
        die("resize buffer not writable");
    int resized[3] = {id3, id2, id4};
    expect_order(resized, 3, "resize keeps the order");

    // A same-size resize is a no-op and keeps the buffer.
    if (grv_surface_resize(id2, 128, 32, &s) != 0) die("same-size resize");
    if (px[0] != 0xDEADBEEF) die("same-size resize moved the buffer");

    // Bad sizes and unknown ids are refused.
    if (grv_surface_resize(id2, 0, 32, &s) != -1) die("resize to 0 not refused");
    if (grv_surface_resize(id2, 40000, 32, &s) != -1) die("huge resize not refused");
    if (grv_surface_resize(9999, 32, 32, &s) != -1) die("unknown resize not refused");

    // --- registry cap, then a full drain still leaves it reusable ---
    int made = 0;
    while (1) {
        int r = grv_surface_create(8, 8, &s);
        if (r < 0) break;
        made++;
        if (made > 64) die("cap never reached");
    }
    if (made != 32 - 3) die("cap count");

    while (list_all() > 0) {
        if (grv_surface_destroy((int)recs[0].id) != 0) die("destroy during drain");
    }

    int r1 = grv_surface_create(16, 16, &s);
    int r2 = grv_surface_create(16, 16, &s);
    if (r1 < 0 || r2 < 0) die("create after drain");
    if (grv_surface_destroy(r1) != 0) die("destroy after drain 1");
    if (grv_surface_destroy(r2) != 0) die("destroy after drain 2");
    if (list_all() != 0) die("not empty after drain");

    printf("[surfacetest] surface contract ok\n");
    return 0;
}
