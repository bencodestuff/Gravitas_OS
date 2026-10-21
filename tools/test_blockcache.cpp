// The page cache sits between ext2 and the block layer, so a bug here shows up
// as filesystem corruption rather than as a failed read: a page written back to
// the wrong offset, or a dirty page dropped on eviction, still looks fine in
// RAM. This test drives the cache against a fake sector device and checks the
// bytes on that device, not just the bytes the cache hands back.
//
// It runs on the host, so it is part of the fast half (`ninja check`).
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "kernel/core/blockcache.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok - %s\n", what);
    } else {
        std::printf("FAIL - %s\n", what);
        failures++;
    }
}

// A sector device over a host buffer, counting what reaches it so the test can
// tell a cache hit from a device read.
struct Fake {
    std::vector<unsigned char> bytes;
    uint64_t sectors = 0;
    uint64_t read_calls = 0;
    uint64_t read_sectors = 0;
    uint64_t write_calls = 0;
    uint64_t write_sectors = 0;
    uint64_t flush_calls = 0;
};

struct Disk {
    Fake fake;
    Block::Device dev;
};

static uint64_t fake_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Fake* f = static_cast<Fake*>(priv);
    if (lba > f->sectors || count > f->sectors - lba) return 0;
    f->read_calls++;
    f->read_sectors += count;
    std::memcpy(buf, f->bytes.data() + lba * 512, (size_t)count * 512);
    return count;
}

static uint64_t fake_write(void* priv, uint64_t lba, uint64_t count, const void* buf) {
    Fake* f = static_cast<Fake*>(priv);
    if (lba > f->sectors || count > f->sectors - lba) return 0;
    f->write_calls++;
    f->write_sectors += count;
    std::memcpy(f->bytes.data() + lba * 512, buf, (size_t)count * 512);
    return count;
}

static bool fake_flush(void* priv) {
    Fake* f = static_cast<Fake*>(priv);
    f->flush_calls++;
    return true;
}

// Every byte of the disk is a function of its offset, so a check can name any
// byte without carrying a copy of the image.
static unsigned char disk_byte(uint64_t off, unsigned seed) {
    return (unsigned char)(seed + off * 7 + (off >> 9));
}

// The cache keeps entries alive across tests, and an entry holds a Device*, so
// every fake disk has to outlive the cache's reference to it.
static std::vector<std::unique_ptr<Disk>> disks;

static Disk* make_disk(uint64_t sectors, unsigned seed) {
    auto d = std::make_unique<Disk>();
    d->fake.sectors = sectors;
    d->fake.bytes.resize((size_t)sectors * 512);
    for (size_t i = 0; i < d->fake.bytes.size(); i++)
        d->fake.bytes[i] = disk_byte(i, seed);
    d->dev.name = "fake";
    d->dev.sectors = sectors;
    d->dev.read = fake_read;
    d->dev.write = fake_write;
    d->dev.flush = fake_flush;
    d->dev.priv = &d->fake;
    disks.push_back(std::move(d));
    return disks.back().get();
}

// 1. A sector read misses and fetches the whole 4 KB page; the next sector of
//    that page is a hit and costs nothing.
static void test_read_fills_page() {
    std::printf("-- read fills a whole page\n");
    Disk* d = make_disk(320, 11);
    unsigned char buf[512];

    BlockCache::reset_stats();
    bool ok = BlockCache::read(&d->dev, 0, 1, buf);
    BlockCache::Stats s = BlockCache::stats();
    check(ok && std::memcmp(buf, d->fake.bytes.data(), 512) == 0,
          "a one-sector read returns the device's bytes");
    check(s.misses == 1 && s.hits == 0, "the first sector is a miss");
    check(d->fake.read_calls == 1 && d->fake.read_sectors == 8,
          "the miss reads one 8-sector page, not one sector");

    ok = BlockCache::read(&d->dev, 1, 1, buf);
    s = BlockCache::stats();
    check(ok && std::memcmp(buf, d->fake.bytes.data() + 512, 512) == 0,
          "the next sector of the page reads back correctly");
    check(s.hits == 1 && s.misses == 1, "the next sector is a hit");
    check(d->fake.read_calls == 1, "a hit issues no device read");
}

// 2. A transfer that starts in one page and ends in the next is split, and both
//    halves come back from the right offsets.
static void test_read_across_page() {
    std::printf("-- read across a page boundary\n");
    Disk* d = make_disk(320, 23);
    unsigned char buf[3 * 512];

    BlockCache::reset_stats();
    bool ok = BlockCache::read(&d->dev, 7, 3, buf);
    check(ok, "a read crossing a page boundary succeeds");
    check(std::memcmp(buf, d->fake.bytes.data() + 7 * 512, sizeof(buf)) == 0,
          "both pages contribute the right bytes");
    BlockCache::Stats s = BlockCache::stats();
    check(s.misses == 2 && d->fake.read_calls == 2 && d->fake.read_sectors == 16,
          "the crossing read fills two pages, each in one request");

    ok = BlockCache::read(&d->dev, 8, 1, buf);
    check(ok && std::memcmp(buf, d->fake.bytes.data() + 8 * 512, 512) == 0 &&
          BlockCache::stats().hits == 1,
          "the far page is already resident");
}

// 3. A partial write stays in the cache until flush, then exactly one page goes
//    to the device before the device's own flush.
static void test_partial_write_back() {
    std::printf("-- partial write is written back on flush\n");
    Disk* d = make_disk(320, 31);
    unsigned char pattern[512];
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (unsigned char)(200 + i % 40);
    const size_t off = 2 * 512;

    BlockCache::reset_stats();
    bool ok = BlockCache::write(&d->dev, 2, 1, pattern);
    check(ok, "a one-sector partial write succeeds");
    BlockCache::Stats s = BlockCache::stats();
    check(s.misses == 1 && d->fake.read_calls == 1,
          "a partial write into a cold page reads it first");
    check(d->fake.write_calls == 0 &&
          std::memcmp(d->fake.bytes.data() + off, pattern, 512) != 0,
          "the device is untouched before flush");

    ok = BlockCache::flush(&d->dev);
    s = BlockCache::stats();
    check(ok, "flush succeeds");
    check(d->fake.write_calls == 1 && d->fake.write_sectors == 8,
          "flush writes exactly the dirty page");
    check(std::memcmp(d->fake.bytes.data() + off, pattern, 512) == 0,
          "the written sector is on the device");
    bool rest_ok = true;
    for (size_t i = 0; i < 512; i++)
        if (d->fake.bytes[3 * 512 + i] != disk_byte(3 * 512 + i, 31)) {
            rest_ok = false;
            break;
        }
    check(rest_ok, "the rest of the page is unchanged");
    check(d->fake.flush_calls == 1 && s.writebacks == 1 && s.flushes == 1,
          "the dirty page goes out before the device flush");

    ok = BlockCache::flush(&d->dev);
    check(ok && d->fake.write_calls == 1 && d->fake.flush_calls == 2,
          "a second flush writes nothing, but still flushes the device");
}

// 4. A write that covers a whole page replaces every byte, so it must not read
//    the old contents first.
static void test_whole_page_write() {
    std::printf("-- whole page write skips the read\n");
    Disk* d = make_disk(320, 41);
    unsigned char page[BlockCache::PAGE_SIZE];
    for (size_t i = 0; i < sizeof(page); i++) page[i] = (unsigned char)(17 + i * 3);

    BlockCache::reset_stats();
    bool ok = BlockCache::write(&d->dev, 8, 8, page);
    BlockCache::Stats s = BlockCache::stats();
    check(ok, "a whole-page write succeeds");
    check(d->fake.read_calls == 0, "a whole-page write reads nothing first");
    check(s.misses == 1 && s.hits == 0, "the page is still a miss");
    check(d->fake.write_calls == 0, "the write is held in the cache");

    ok = BlockCache::flush(&d->dev);
    check(ok && d->fake.write_sectors == 8 &&
          std::memcmp(d->fake.bytes.data() + 4096, page, sizeof(page)) == 0,
          "the whole page reaches the device on flush");
}

// 5. More pages than the budget evicts the least recently used page; a dirty
//    victim is written back, not dropped.
static void test_eviction_writes_back() {
    std::printf("-- eviction writes a dirty page back\n");
    Disk* d = make_disk(100 * 8, 53);
    unsigned char page[BlockCache::PAGE_SIZE];
    for (size_t i = 0; i < sizeof(page); i++) page[i] = 0;

    BlockCache::reset_stats();
    bool ok = true;
    for (uint64_t p = 0; p < BlockCache::PAGES + 1; p++) {
        std::memset(page, (int)(p + 1), sizeof(page));
        if (!BlockCache::write(&d->dev, p * 8, 8, page)) { ok = false; break; }
    }
    BlockCache::Stats s = BlockCache::stats();
    check(ok, "writing more pages than the budget succeeds");
    check(s.misses == BlockCache::PAGES + 1, "every new page is a miss");
    check(s.evictions >= 1, "the cache evicted at least one page");
    check(s.writebacks == 1, "only the one dirty victim was written back");
    check(d->fake.write_calls == 1,
          "the eviction issued one device write, not a flush");
    ok = true;
    for (size_t i = 0; i < sizeof(page); i++)
        if (d->fake.bytes[i] != 1) { ok = false; break; }
    check(ok, "the evicted page reached the device with the right contents");

    ok = BlockCache::read(&d->dev, 0, 1, page);
    check(ok && page[0] == 1, "the evicted page reads back from the device");

    // Leave nothing dirty behind: later checks count this device's writes.
    BlockCache::flush(&d->dev);
}

// 6. A device whose size is not a whole number of pages has a short tail page.
//    The cache loads and writes back only the sectors that exist, and a request
//    starting in the tail page and ending past the device is refused whole.
//
// The zero fill past the device end cannot be observed through this API: the
// all-or-nothing range check means no caller can ask for those bytes. It is
// still worth doing, because the bytes sit in a shared page slot and a future
// caller that does see them must not get the previous page's contents.
static void test_tail_page() {
    std::printf("-- tail page of a device that is not whole pages\n");
    Disk* d = make_disk(10, 67);   // one full page and two sectors
    unsigned char buf[512];

    BlockCache::reset_stats();
    bool ok = BlockCache::read(&d->dev, 8, 2, buf);
    check(ok && std::memcmp(buf, d->fake.bytes.data() + 8 * 512, 512) == 0,
          "the tail page reads its first sector");
    check(d->fake.read_calls == 1 && d->fake.read_sectors == 2,
          "the tail page is fetched with only the sectors that exist");
    ok = BlockCache::read(&d->dev, 9, 1, buf);
    check(ok && std::memcmp(buf, d->fake.bytes.data() + 9 * 512, 512) == 0 &&
          d->fake.read_calls == 1,
          "the second tail sector is a hit");

    unsigned char w[512];
    std::memset(w, 0x5a, sizeof(w));
    ok = BlockCache::write(&d->dev, 9, 1, w);
    check(ok && d->fake.write_calls == 0, "a tail-sector write is cached");
    ok = BlockCache::flush(&d->dev);
    check(ok && d->fake.write_calls == 1 && d->fake.write_sectors == 2,
          "the tail page is written back short, not as eight sectors");
    check(std::memcmp(d->fake.bytes.data() + 9 * 512, w, 512) == 0,
          "the tail sector is on the device");

    // A dirty tail page must survive eviction too.
    std::memset(w, 0x6b, sizeof(w));
    check(BlockCache::write(&d->dev, 8, 1, w), "dirty the tail page again");
    Disk* other = make_disk(64 * 8, 71);
    unsigned char page[BlockCache::PAGE_SIZE];
    for (uint64_t p = 0; p < BlockCache::PAGES; p++)
        BlockCache::read(&other->dev, p * 8, 8, page);
    check(d->fake.write_sectors == 4,
          "evicting the dirty tail page writes its two sectors");

    check(!BlockCache::read(&d->dev, 10, 1, buf),
          "a read past the device end fails");
}

// 7. Bounds are all-or-nothing, exactly like Block's contract.
static void test_bounds() {
    std::printf("-- out-of-range requests move nothing\n");
    Disk* d = make_disk(320, 83);
    unsigned char buf[2 * 512];
    std::memset(buf, 0xaa, sizeof(buf));
    unsigned char src[2 * 512];
    std::memset(src, 0x3c, sizeof(src));

    check(!BlockCache::read(&d->dev, 319, 2, buf),
          "a read starting inside and ending past the end fails");
    check(buf[0] == 0xaa && buf[1023] == 0xaa,
          "the failed read moved nothing");
    check(!BlockCache::write(&d->dev, 319, 2, src),
          "a write starting inside and ending past the end fails");
    check(d->fake.write_calls == 0 && d->fake.write_sectors == 0,
          "the failed write reached no device");
    check(!BlockCache::read(&d->dev, 320, 1, buf),
          "a read starting at the end fails");
    check(BlockCache::read(&d->dev, 320, 0, buf) &&
          BlockCache::write(&d->dev, 320, 0, src),
          "zero-length transfers at the end succeed and move nothing");
}

int main() {
    test_read_fills_page();
    test_read_across_page();
    test_partial_write_back();
    test_whole_page_write();
    test_eviction_writes_back();
    test_tail_page();
    test_bounds();

    BlockCache::Stats s = BlockCache::stats();
    check(s.misses > 0 && s.writebacks > 0 && s.evictions > 0 && s.flushes > 0,
          "the cache saw misses, writebacks, evictions and flushes");
    std::printf("test_blockcache: hits=%llu misses=%llu writebacks=%llu "
                "evictions=%llu flushes=%llu\n",
                (unsigned long long)s.hits, (unsigned long long)s.misses,
                (unsigned long long)s.writebacks,
                (unsigned long long)s.evictions,
                (unsigned long long)s.flushes);

    if (failures == 0) {
        std::printf("test_blockcache: all checks pass\n");
        return 0;
    }
    std::printf("test_blockcache: %d check(s) failed\n", failures);
    return 1;
}
