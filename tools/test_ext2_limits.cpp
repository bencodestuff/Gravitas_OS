// The ext2 write path's size rule is arithmetic, and arithmetic is the one part
// of it a guest can never demonstrate: the shipped image is 16 MB, so a write at
// 4 GiB fails for want of a block long before it reaches the interesting edge,
// and the wrong answer there is a wrapped i_size rather than a refused write.
//
// So this test drives the rule itself (Ext2::write_past_limit) and, alongside it,
// the 32-bit store it replaced: for every offset and length around the limit the
// guard must agree with a lossless comparison, must refuse exactly the writes
// whose length cannot be held in an inode, and those are exactly the writes whose
// old recorded length fell below the write's own start.
//
// It runs on the host, so it is part of the fast half (`ninja check`).
#include <cstdint>
#include <cstdio>

#include "kernel/dev/ext2.h"

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        std::printf("FAIL - %s\n", what);
        failures++;
    }
}

int main() {
    const uint64_t MAX = Ext2::MAX_FILE_BYTES;

    // Inside the limit, including a write that fills it exactly.
    check(!Ext2::write_past_limit(0, 1), "a one-byte write at zero fits");
    check(!Ext2::write_past_limit(0, MAX), "a write of the whole limit fits");
    check(!Ext2::write_past_limit(MAX - 16, 16),
          "a write ending exactly on the limit fits");
    check(!Ext2::write_past_limit(MAX, 0), "a zero-length write at the limit fits");

    // Past it, by one byte or by a great deal.
    check(Ext2::write_past_limit(MAX - 16, 17),
          "a write one byte past the limit is refused");
    check(Ext2::write_past_limit(MAX, 1),
          "a single byte at the last offset is refused");
    check(Ext2::write_past_limit(MAX + 1, 1),
          "a write starting past the limit is refused");
    check(Ext2::write_past_limit(UINT64_MAX, 1),
          "the farthest offset does not overflow the check");
    check(Ext2::write_past_limit(UINT64_MAX, UINT64_MAX),
          "two huge operands do not overflow the check");

    // The rule, over the whole neighbourhood of the limit. Aggregated into one
    // check each so a failure names the property, not one of thousands of pairs.
    bool agrees = true;          // guard == "the end is past MAX", computed losslessly
    bool refused_is_lossy = true;    // every refused write really could not be stored
    bool allowed_is_lossless = true; // every allowed write stores exactly
    bool fell_below_start = false;   // some refused write wrapped below its offset
    for (uint64_t off = MAX - 64; off <= MAX; off++) {
        for (uint64_t count = 1; count <= 72; count++) {
            const uint64_t end = off + count;
            const bool refuse = Ext2::write_past_limit(off, count);
            const uint32_t stored = (uint32_t)end;   // what the old store recorded
            const bool lossy = (uint64_t)stored != end;
            if (refuse != (end > MAX)) agrees = false;
            if (refuse && !lossy) refused_is_lossy = false;
            if (!refuse && lossy) allowed_is_lossless = false;
            if (refuse && (uint64_t)stored < off) fell_below_start = true;
        }
    }
    check(agrees, "the guard agrees with an exact comparison at every edge");
    check(refused_is_lossy,
          "every refused write is one whose length the inode cannot hold");
    check(allowed_is_lossless,
          "every allowed write's length stores exactly");
    check(fell_below_start,
          "a refused write would have recorded a length below its own start");

    // The concrete case, in the numbers a reader can check by hand: 32 bytes at
    // 0xFFFFFFF0 used to be written and then recorded as a 16-byte file, so the
    // caller was told 32 bytes landed while the length lost everything before
    // the last block.
    check((uint32_t)(0xFFFFFFF0ull + 32) == 16,
          "the old store turned a 32-byte write at the limit into 16 bytes");
    check(Ext2::write_past_limit(0xFFFFFFF0ull, 32),
          "that same write is now refused");

    // The block index is the other 32-bit narrowing in the write path, and it is
    // not why this bug bites at 4 GiB: an index only wraps once a file passes
    // 2^32 blocks, which is 4 TiB at 1 KB blocks and 16 TiB at 4 KB. The size cap
    // is what keeps the cast honest, and it does so with room to spare.
    check((uint32_t)((1ull << 32) * 4096 / 4096) == 0,
          "a block index wraps only past 2^32 blocks");
    check(MAX / 1024 < 0xFFFFFFFFull,
          "a capped file's block index fits a uint32_t at 1 KB blocks");

    if (failures == 0) {
        std::printf("test_ext2_limits: all %d limit checks pass\n", checks);
        return 0;
    }
    std::printf("test_ext2_limits: %d of %d limit checks failed\n", failures,
                checks);
    return 1;
}
