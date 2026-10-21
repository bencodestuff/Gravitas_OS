#include "kernel/dev/ata.h"
#include "kernel/core/block.h"
#include "kernel/io.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// --- ATA PIO ---
//
// The task file is eight registers on the legacy ports; the data register is
// 16 bits wide, so a 512-byte sector is 256 `insw`/`outsw`. Everything is
// polled: there is no interrupt here, which keeps the driver independent of the
// IRQ wiring and makes it deterministic in a test.
//
// Two things this deliberately does not do. It does not use the secondary
// channel (0x170) — one system, one disk, and a driver that claims less is a
// driver that can be believed. And it does not fall back to CHS addressing: a
// drive that cannot do LBA is reported unusable rather than driven wrongly.
//
// The status register is polled with a spin cap everywhere. A device that never
// answers must not hang the kernel, and "no drive here" is a normal outcome.

namespace Ata {

// Primary channel. The secondary channel's task file is 0x170/0x376.
constexpr uint16_t IO_BASE   = 0x1F0;
constexpr uint16_t CTRL_BASE = 0x3F6;

// Register offsets from the task file base.
constexpr uint16_t R_DATA   = 0;   // 16-bit, the only one that moves data
constexpr uint16_t R_ERROR  = 1;   // read: error; write: features
constexpr uint16_t R_COUNT  = 2;
constexpr uint16_t R_LBA0   = 3;
constexpr uint16_t R_LBA1   = 4;
constexpr uint16_t R_LBA2   = 5;
constexpr uint16_t R_DRIVE  = 6;
constexpr uint16_t R_STATUS = 7;   // read: status; write: command

// Status bits.
constexpr uint8_t ST_ERR  = 0x01;
constexpr uint8_t ST_DRQ  = 0x08;
constexpr uint8_t ST_DF   = 0x20;
constexpr uint8_t ST_DRDY = 0x40;
constexpr uint8_t ST_BSY  = 0x80;

// Commands.
constexpr uint8_t CMD_READ     = 0x20;
constexpr uint8_t CMD_READ48   = 0x24;
constexpr uint8_t CMD_WRITE    = 0x30;
constexpr uint8_t CMD_WRITE48  = 0x34;
constexpr uint8_t CMD_FLUSH    = 0xE7;
constexpr uint8_t CMD_IDENTIFY = 0xEC;

constexpr uint32_t SECTOR    = 512;
constexpr uint32_t SECTOR_WORDS = SECTOR / 2;

// The sector count register is 8 bits, and 0 there means 256. Staying under 256
// keeps that corner out of the driver entirely.
constexpr uint32_t MAX_CHUNK = 255;

// Select bytes: bits 7-5 set, bit 4 picks the drive.
constexpr uint8_t SEL_MASTER = 0xA0;
constexpr uint8_t SEL_SLAVE  = 0xB0;

struct Drive {
    bool     lba48;
    uint64_t sectors;
    uint8_t  select;
};

static Drive drives[2];
static Block::Device devs[2];
static int ndrives = 0;

// --- port-level helpers ---

// The controller needs a moment after a command before its status means
// anything. Reading the alternate status four times is the standard 400ns.
static void delay_400ns() {
    for (int i = 0; i < 4; i++) (void)inb(CTRL_BASE);
}

// Wait for BSY to clear. False on timeout, or when the bus reads as floating
// (0xFF), which is what "no drive on this select" looks like.
static bool wait_ready(uint64_t spins) {
    for (uint64_t i = 0; i < spins; i++) {
        uint8_t s = inb(IO_BASE + R_STATUS);
        if (s == 0xFF) return false;
        if (!(s & ST_BSY)) return (s & ST_DRDY) != 0;
    }
    return false;
}

// Wait for the drive to be ready to hand over a sector's worth of data.
static bool wait_drq(uint64_t spins) {
    for (uint64_t i = 0; i < spins; i++) {
        uint8_t s = inb(IO_BASE + R_STATUS);
        if (s & (ST_ERR | ST_DF)) return false;
        if (s & ST_DRQ) return true;
        if (s == 0xFF) return false;
    }
    return false;
}

// `ins`/`outs` move one 16-bit word per count through the data register. A
// sector is little-endian on the wire and x86 is little-endian, so the words
// land in memory exactly as the bytes should. In 64-bit mode the string
// instructions' segment is flat, so ES:DI and DS:SI are just DI and SI.
static void read_words(uint16_t port, void* addr, uint64_t count) {
    asm volatile("rep insw"
                 : "+D"(addr), "+c"(count)
                 : "d"(port)
                 : "memory");
}

static void write_words(uint16_t port, const void* addr, uint64_t count) {
    asm volatile("rep outsw"
                 : "+S"(addr), "+c"(count)
                 : "d"(port)
                 : "memory");
}

// --- commands ---

// Load the task file and issue a read or write of 'nsect' sectors at 'lba'.
static bool submit(Drive* d, bool writing, uint64_t lba, uint32_t nsect) {
    // Select the drive *first*. The status register reports the selected drive,
    // so waiting for "ready" before selecting is waiting on whichever drive was
    // selected last — which is how a command came to be issued to a drive whose
    // status read as busy. The drive select also has to settle: the controller
    // needs a moment before the rest of the task file means anything.
    if (d->lba48) {
        outb(IO_BASE + R_DRIVE, 0x40 | (d->select & 0x10));
    } else {
        outb(IO_BASE + R_DRIVE,
             0xE0 | (d->select & 0x10) | ((lba >> 24) & 0x0F));
    }
    delay_400ns();

    if (!wait_ready(500000)) return false;

    if (d->lba48) {
        // LBA48 wants the high halves first, then the low halves.
        outb(IO_BASE + R_COUNT, (nsect >> 8) & 0xFF);
        outb(IO_BASE + R_LBA0, (lba >> 24) & 0xFF);
        outb(IO_BASE + R_LBA1, (lba >> 32) & 0xFF);
        outb(IO_BASE + R_LBA2, (lba >> 40) & 0xFF);
        outb(IO_BASE + R_COUNT, nsect & 0xFF);
        outb(IO_BASE + R_LBA0, lba & 0xFF);
        outb(IO_BASE + R_LBA1, (lba >> 8) & 0xFF);
        outb(IO_BASE + R_LBA2, (lba >> 16) & 0xFF);
    } else {
        outb(IO_BASE + R_COUNT, nsect & 0xFF);
        outb(IO_BASE + R_LBA0, lba & 0xFF);
        outb(IO_BASE + R_LBA1, (lba >> 8) & 0xFF);
        outb(IO_BASE + R_LBA2, (lba >> 16) & 0xFF);
    }

    uint8_t cmd;
    if (writing) cmd = d->lba48 ? CMD_WRITE48 : CMD_WRITE;
    else         cmd = d->lba48 ? CMD_READ48 : CMD_READ;
    outb(IO_BASE + R_STATUS, cmd);
    delay_400ns();
    return true;
}

// The write cache has to be told to empty, or a completed write is only
// completed as far as the drive's own buffer is concerned.
static bool flush(Drive* d) {
    if (!wait_ready(500000)) return false;
    outb(IO_BASE + R_DRIVE, d->select);
    outb(IO_BASE + R_STATUS, CMD_FLUSH);
    delay_400ns();
    return wait_ready(2000000);
}

static bool ata_flush(void* priv) { return flush((Drive*)priv); }

// IDENTIFY DEVICE: 256 words describing the drive.
static bool identify(Drive* d, uint16_t* words) {
    outb(IO_BASE + R_DRIVE, d->select);
    delay_400ns();
    outb(IO_BASE + R_COUNT, 0);
    outb(IO_BASE + R_LBA0, 0);
    outb(IO_BASE + R_LBA1, 0);
    outb(IO_BASE + R_LBA2, 0);
    outb(IO_BASE + R_STATUS, CMD_IDENTIFY);
    delay_400ns();

    uint8_t s = inb(IO_BASE + R_STATUS);
    if (s == 0 || s == 0xFF) return false;   // nothing on this select
    if (!wait_ready(2000000)) return false;

    // An ATAPI device answers IDENTIFY by leaving a signature in the LBA
    // registers. This is a disk driver, so that is a no.
    if (inb(IO_BASE + R_LBA1) != 0 || inb(IO_BASE + R_LBA2) != 0) return false;
    if (!wait_drq(2000000)) return false;

    read_words(IO_BASE + R_DATA, words, SECTOR_WORDS);
    return true;
}

// --- the Block ops ---

// A short transfer is a hardware problem, and a hardware problem should say
// something rather than look like a disk that ran out of room.
static void report_error(const char* what) {
    kprint("[ATA] ");
    kprint(what);
    kprint(" failed, error register ");
    kprint_uint(inb(IO_BASE + R_ERROR));
    kprint("\n");
}

static uint64_t ata_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Drive* d = (Drive*)priv;
    if (lba >= d->sectors) return 0;
    if (count > d->sectors - lba) return 0;

    uint8_t* p = (uint8_t*)buf;
    uint64_t done = 0;
    bool ok = true;
    while (done < count && ok) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;

        if (!submit(d, false, lba + done, chunk)) { ok = false; break; }
        for (uint32_t s = 0; s < chunk; s++) {
            if (!wait_drq(2000000)) { ok = false; break; }
            read_words(IO_BASE + R_DATA, p, SECTOR_WORDS);
            p += SECTOR;
        }
        done += chunk;
    }
    if (!ok) report_error("read");
    return done;
}

static uint64_t ata_write(void* priv, uint64_t lba, uint64_t count,
                          const void* buf) {
    Drive* d = (Drive*)priv;
    if (lba >= d->sectors) return 0;
    if (count > d->sectors - lba) return 0;

    const uint8_t* p = (const uint8_t*)buf;
    uint64_t done = 0;
    bool ok = true;
    while (done < count && ok) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;

        if (!submit(d, true, lba + done, chunk)) { ok = false; break; }
        for (uint32_t s = 0; s < chunk; s++) {
            if (!wait_drq(2000000)) { ok = false; break; }
            write_words(IO_BASE + R_DATA, p, SECTOR_WORDS);
            p += SECTOR;
        }
        done += chunk;
    }
    if (!ok) report_error("write");
    else if (done > 0) flush(d);
    return done;
}

// --- probe ---

// Try one select. Returns whether a usable drive answered.
static bool probe(uint8_t select, const char* name) {
    Drive cand;
    cand.select = select;
    cand.lba48 = false;
    cand.sectors = 0;

    uint16_t words[256];
    if (!identify(&cand, words)) return false;

    // Word 49 bit 9: LBA addressing. A CHS-only drive is refused rather than
    // driven wrongly; nothing this century needs it.
    if (!(words[49] & 0x0200)) {
        kprint("[ATA] ");
        kprint(name);
        kprint(": no LBA support\n");
        return false;
    }

    uint64_t sectors = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    bool lba48 = (words[83] & 0x0400) != 0;
    if (lba48) {
        uint64_t hi = (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
                      ((uint64_t)words[102] << 32) | ((uint64_t)words[103] << 48);
        if (hi != 0) sectors = hi;
    }
    if (sectors == 0) return false;

    Drive* d = &drives[ndrives];
    *d = cand;
    d->sectors = sectors;
    d->lba48 = lba48;

    Block::Device* dev = &devs[ndrives];
    dev->name = name;
    dev->sectors = sectors;
    dev->read = ata_read;
    dev->write = ata_write;
    dev->flush = ata_flush;
    dev->priv = d;

    if (!Block::add(dev)) return false;

    kprint("[ATA] ");
    kprint(name);
    kprint(": ");
    kprint_uint(sectors);
    kprint(" sectors, ");
    kprint(lba48 ? "LBA48" : "LBA28");
    kprint("\n");
    ndrives++;
    return true;
}

Block::Device* init() {
    // Master and slave: two drives is what the channel has, and the caller
    // decides what each one is.
    probe(SEL_MASTER, "ata0");
    probe(SEL_SLAVE, "ata1");
    return ndrives > 0 ? &devs[0] : nullptr;
}

int count() { return ndrives; }

Block::Device* device(int i) {
    return (i >= 0 && i < ndrives) ? &devs[i] : nullptr;
}

} // namespace Ata
