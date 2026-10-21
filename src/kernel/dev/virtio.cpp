#include "kernel/dev/virtio.h"
#include "kernel/dev/pci.h"
#include "kernel/core/block.h"
#include "kernel/core/heap.h"
#include "kernel/core/vmm.h"
#include "kernel/io.h"
#include "kernel/klib/klib.h"

// --- virtio-blk, legacy interface ---
//
// The legacy device is one I/O BAR with a fixed register file and a single
// virtqueue; see virtio.h for why the suite asks for it. Everything here is
// polled, so the device needs no interrupt and the driver is deterministic in a
// test, exactly like the ATA one.
//
// Two pieces of hardware behaviour are easy to get wrong and are called out
// where they happen: the queue address is a *page frame number*, not an
// address, and the device reads the queue only after the notify write, so the
// descriptor and ring stores must be ordered before it.

namespace Virtio {

// Register file, offsets from the I/O BAR.
constexpr uint16_t R_HOST_FEATURES  = 0x00;
constexpr uint16_t R_GUEST_FEATURES = 0x04;
constexpr uint16_t R_QUEUE_ADDRESS  = 0x08;   // page frame number of the queue
constexpr uint16_t R_QUEUE_SIZE     = 0x0C;   // read-only: the device's max
constexpr uint16_t R_QUEUE_SELECT   = 0x0E;
constexpr uint16_t R_QUEUE_NOTIFY   = 0x10;
constexpr uint16_t R_STATUS         = 0x12;
constexpr uint16_t R_ISR            = 0x13;
constexpr uint16_t R_CAPACITY       = 0x14;   // u64, in 512-byte sectors

constexpr uint8_t S_ACKNOWLEDGE = 1;
constexpr uint8_t S_DRIVER      = 2;
constexpr uint8_t S_DRIVER_OK   = 4;

constexpr uint16_t VENDOR     = 0x1AF4;
constexpr uint16_t DEVICE_BLK = 0x1001;   // legacy virtio-blk

// --- the split virtqueue ---

struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
constexpr uint16_t DESC_NEXT  = 1;
constexpr uint16_t DESC_WRITE = 2;   // device-writable

// The queue size is the device's to choose; QEMU's legacy virtio-blk reports
// 256. The headroom is so a device that reports a little more still works.
constexpr uint32_t MAX_QUEUE = 256;

// One request in flight at a time, staged through a fixed buffer. The chain is
// header, data, status.
constexpr uint32_t BOUNCE_SECTORS = 32;   // 16 KB

struct Request {
    uint32_t type;        // 0 = read (device to guest), 1 = write
    uint32_t reserved;
    uint64_t sector;
    uint8_t  status;      // written by the device: 0 is ok
    uint8_t  pad[7];
    uint8_t  data[BOUNCE_SECTORS * Block::SECTOR];
};

struct Dev {
    uint16_t io;          // I/O BAR base
    uint64_t sectors;
    uint16_t qsize;
    uint8_t* desc;        // descriptor table  (page-aligned, contiguous)
    uint8_t* avail;       // available ring
    uint8_t* used;        // used ring, on its own page
    uint16_t last_used;
    Request* req;         // header/status plus the staged data
    bool failed;          // re-arm failed: the device must not be driven again
};

static Dev devs[Block::MAX_DEVICES];
static Block::Device bdevs[Block::MAX_DEVICES];
static int ndevs = 0;

static const char* const dev_names[Block::MAX_DEVICES] = {
    "virtio0", "virtio1", "virtio2", "virtio3",
};

// A request must not be interleaved with another. The timer can switch tasks
// mid-spin, and the next task could reach the disk through the same queue, so
// the whole transfer runs with interrupts off.
static inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1ull << 9)) asm volatile("sti"); else asm volatile("cli");
}

// A page-aligned, physically contiguous block, which is what the queue address
// register needs. The kernel heap sits in the direct map over a reserved,
// contiguous physical range, so aligning a heap pointer up gives both: the
// slack is under a page, freed never, and this runs once per device.
static uint8_t* alloc_page_aligned(size_t bytes) {
    uint8_t* raw = (uint8_t*)Heap::malloc(bytes + 4096);
    if (raw == nullptr) return nullptr;
    uintptr_t a = ((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u;
    return (uint8_t*)a;
}

// Put the device back in a state where no completion from an earlier request
// can ever be read as the answer to a later one. A full reset is required
// rather than resyncing last_used from the used index: only one chain is ever
// in flight, so a stale used entry is indistinguishable from the one the next
// request will produce, and there is no way to tell them apart by inspection.
// The reset aborts the request; emptying the rings then removes any trace of
// it. The queue memory, its size and the descriptors are unchanged, so only
// the legacy handshake and the ring indices have to be redone.
static bool rearm(Dev* d) {
    outb(d->io + R_STATUS, 0);

    outb(d->io + R_STATUS, S_ACKNOWLEDGE);
    outb(d->io + R_STATUS, S_ACKNOWLEDGE | S_DRIVER);
    (void)inl(d->io + R_HOST_FEATURES);
    outl(d->io + R_GUEST_FEATURES, 0);

    outw(d->io + R_QUEUE_SELECT, 0);
    // The device picks the queue size, so a different one after the reset would
    // mean the ring layout below no longer matches what is in guest memory.
    if (inw(d->io + R_QUEUE_SIZE) != d->qsize) {
        outb(d->io + R_STATUS, 0);
        return false;
    }

    // Same queue page as before: the register wants its page frame number.
    outl(d->io + R_QUEUE_ADDRESS, (uint32_t)(VMM::phys_of(d->desc) >> 12));

    // Only the rings carry device-visible state, so emptying them is enough to
    // forget the abandoned request. The barrier keeps those stores (and the
    // last_used reset) before the device is told it may look at the rings.
    memset(d->avail, 0, 4u + 2u * d->qsize);
    memset(d->used, 0, 4u + 8u * d->qsize);
    d->last_used = 0;
    d->req->status = 0xFF;
    __sync_synchronize();

    outb(d->io + R_STATUS, S_ACKNOWLEDGE | S_DRIVER | S_DRIVER_OK);
    return true;
}

// Issue one request of at most BOUNCE_SECTORS sectors. False on a device error
// or a timeout; prints the reason, because a disk that stops answering should
// say so rather than look like a disk that ran out of room.
static bool submit(Dev* d, bool write, uint64_t lba, uint32_t nsec, void* buf) {
    if (d->failed) return false;

    Request* r = d->req;
    r->type = write ? 1u : 0u;
    r->reserved = 0;
    r->sector = lba;
    r->status = 0xFF;
    uint32_t len = nsec * (uint32_t)Block::SECTOR;
    if (write) memcpy(r->data, buf, len);

    Desc* desc = (Desc*)d->desc;
    desc[0].addr  = VMM::phys_of(&r->type);
    desc[0].len   = 16;
    desc[0].flags = DESC_NEXT;
    desc[0].next  = 1;
    desc[1].addr  = VMM::phys_of(r->data);
    desc[1].len   = len;
    desc[1].flags = DESC_NEXT | (write ? 0 : DESC_WRITE);
    desc[1].next  = 2;
    desc[2].addr  = VMM::phys_of(&r->status);
    desc[2].len   = 1;
    desc[2].flags = DESC_WRITE;
    desc[2].next  = 0;

    // Hand the chain to the device. Descriptor 0 is always its head and only
    // one request is ever in flight, so the available ring always says 0. The
    // stores must be visible before the notify, hence the barriers. The rings
    // are volatile because the device, not this CPU, is what moves the used
    // index, and the poll below must re-read it every turn.
    volatile uint16_t* avail_idx = (volatile uint16_t*)(d->avail + 2);
    volatile uint16_t* avail_ring = (volatile uint16_t*)(d->avail + 4);
    uint16_t slot = *avail_idx;
    avail_ring[slot % d->qsize] = 0;
    __sync_synchronize();
    *avail_idx = (uint16_t)(slot + 1);
    __sync_synchronize();
    outw(d->io + R_QUEUE_NOTIFY, 0);

    volatile uint16_t* used_idx = (volatile uint16_t*)(d->used + 2);
    bool done = false;
    for (uint64_t spin = 0; spin < 20000000; spin++) {
        if (*used_idx != d->last_used) { done = true; break; }
    }
    (void)inb(d->io + R_ISR);   // clear the completion bit (unused, but deasserts)
    if (!done) {
        kprint("[Virtio] request timed out\n");
        // The chain is still in the device's hands. Leaving it there would let
        // its late completion satisfy the next request, so re-arm now; if that
        // fails there is no safe way to keep using this device.
        if (!rearm(d)) {
            d->failed = true;
            kprint("[Virtio] re-arm failed, device disabled\n");
        }
        return false;
    }
    d->last_used = *used_idx;

    if (r->status != 0) {
        kprint("[Virtio] request failed, status ");
        kprint_uint(r->status);
        kprint("\n");
        return false;
    }
    if (!write) memcpy(buf, r->data, len);
    return true;
}

// The Block ops. A request that runs past the end moves none, matching the
// contract the other devices keep.
static uint64_t blk_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Dev* d = (Dev*)priv;
    if (lba >= d->sectors) return 0;
    if (count > d->sectors - lba) return 0;

    uint8_t* p = (uint8_t*)buf;
    uint64_t done = 0;
    uint64_t f = irq_save();
    while (done < count) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > BOUNCE_SECTORS) chunk = BOUNCE_SECTORS;
        if (!submit(d, false, lba + done, chunk, p)) break;
        p += (uint64_t)chunk * Block::SECTOR;
        done += chunk;
    }
    irq_restore(f);
    return done;
}

static uint64_t blk_write(void* priv, uint64_t lba, uint64_t count,
                          const void* buf) {
    Dev* d = (Dev*)priv;
    if (lba >= d->sectors) return 0;
    if (count > d->sectors - lba) return 0;

    const uint8_t* p = (const uint8_t*)buf;
    uint64_t done = 0;
    uint64_t f = irq_save();
    while (done < count) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > BOUNCE_SECTORS) chunk = BOUNCE_SECTORS;
        if (!submit(d, true, lba + done, chunk, (void*)p)) break;
        p += (uint64_t)chunk * Block::SECTOR;
        done += chunk;
    }
    irq_restore(f);
    return done;
}

// --- probe ---

static bool init_one(const Pci::Address& a) {
    if (ndevs >= Block::MAX_DEVICES) return false;

    // The legacy interface is an I/O BAR. A device whose BAR0 is memory is a
    // modern (1.0) one, which this driver does not speak: refuse it rather than
    // drive the wrong registers.
    uint32_t bar0 = Pci::bar(a, 0);
    if ((bar0 & 0x1) == 0) return false;
    uint16_t io = (uint16_t)(bar0 & 0xFFFC);
    if (io == 0) return false;   // firmware never assigned the BAR

    // Reset, then the handshake: acknowledge, then driver, then features.
    outb(io + R_STATUS, 0);
    outb(io + R_STATUS, S_ACKNOWLEDGE);
    outb(io + R_STATUS, S_ACKNOWLEDGE | S_DRIVER);
    (void)inl(io + R_HOST_FEATURES);
    outl(io + R_GUEST_FEATURES, 0);   // no optional feature is used

    outw(io + R_QUEUE_SELECT, 0);
    uint16_t qsize = inw(io + R_QUEUE_SIZE);
    if (qsize < 3 || qsize > MAX_QUEUE) {
        outb(io + R_STATUS, 0);
        return false;
    }

    // Layout: descriptor table, available ring, then the used ring on the next
    // 4K boundary. The device computes the same offsets from the queue size.
    // EVENT_IDX is not negotiated, so neither ring carries its trailing event
    // field: the available ring is flags, idx and the ring alone.
    uint32_t avail_bytes = 4u + 2u * qsize;
    uint32_t used_off = (16u * qsize + avail_bytes + 4095u) & ~4095u;
    uint32_t qbytes = used_off + 4u + 8u * qsize;   // flags, idx, ring[]

    uint8_t* q = alloc_page_aligned(qbytes);
    if (q == nullptr) { outb(io + R_STATUS, 0); return false; }
    memset(q, 0, qbytes);

    Dev* d = &devs[ndevs];
    d->io = io;
    d->qsize = qsize;
    d->desc = q;
    d->avail = q + 16u * qsize;
    d->used = q + used_off;
    d->last_used = 0;
    d->failed = false;

    d->req = (Request*)Heap::malloc(sizeof(Request));
    if (d->req == nullptr) { outb(io + R_STATUS, 0); return false; }
    memset(d->req, 0, sizeof(Request));

    // The queue address register is a page frame number.
    outl(io + R_QUEUE_ADDRESS, (uint32_t)(VMM::phys_of(q) >> 12));
    outb(io + R_STATUS, S_ACKNOWLEDGE | S_DRIVER | S_DRIVER_OK);

    uint64_t sectors = (uint64_t)inl(io + R_CAPACITY) |
                       ((uint64_t)inl(io + R_CAPACITY + 4) << 32);
    if (sectors == 0) { outb(io + R_STATUS, 0); return false; }
    d->sectors = sectors;

    Block::Device* dev = &bdevs[ndevs];
    dev->name = dev_names[ndevs];
    dev->sectors = sectors;
    dev->read = blk_read;
    dev->write = blk_write;
    dev->priv = d;
    if (!Block::add(dev)) { outb(io + R_STATUS, 0); return false; }

    kprint("[Virtio] ");
    kprint(dev->name);
    kprint(": ");
    kprint_uint(sectors);
    kprint(" sectors, queue ");
    kprint_uint(qsize);
    kprint("\n");
    ndevs++;
    return true;
}

Block::Device* init() {
    Pci::Address addrs[Block::MAX_DEVICES];
    int n = Pci::find_all(VENDOR, DEVICE_BLK, addrs, Block::MAX_DEVICES);
    for (int i = 0; i < n; i++) init_one(addrs[i]);
    return ndevs > 0 ? &bdevs[0] : nullptr;
}

int count() { return ndevs; }

Block::Device* device(int i) {
    return (i >= 0 && i < ndevs) ? &bdevs[i] : nullptr;
}

} // namespace Virtio
