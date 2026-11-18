// Address-space teardown is a tree walk, and the only place the kernel
// returns a whole user space's frames at once. The bug this test pins was a
// double free of every intermediate table page: the recursion freed the table
// it had been handed, and the caller freed the same page again on return.
// PMM::free_page() ignores a frame that is already free, so the guest never
// noticed; here the frame manager is a stub that records every deliberate
// free, so a second free is visible.
//
// It runs on the host, so it is part of the fast half (`ninja check`): the
// walk needs no CPU state, only tables in ordinary memory.
//
// The stub also stands in for the HHDM: vmm.cpp's offset starts at zero and
// nothing here calls init(), so a "physical" address is the pointer itself.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <limine.h>

#include "kernel/core/vmm.h"

// --- the frame manager as a test double ------------------------------------
//
// alloc_page() hands out 4 KiB pages from a fixed pool; free_page() records
// the call and clears the page's used bit. A call for a page that is not used
// (never allocated, or already freed) is exactly the double free this test is
// looking for, so it is counted separately from the legitimate frees.
namespace {
constexpr int POOL_PAGES = 32;
constexpr uint64_t PAGE_MASK = 0x000FFFFFFFFFF000ull;

uint64_t pool[POOL_PAGES];
bool used[POOL_PAGES];
std::vector<uint64_t> frees; // every deliberate free_page() call, in order
int double_frees = 0;
int unknown_frees = 0;
int checks = 0;
int failures = 0;
} // namespace

static int page_index(uint64_t phys) {
    for (int i = 0; i < POOL_PAGES; i++)
        if (pool[i] == phys) return i;
    return -1;
}

namespace PMM {
uint64_t alloc_page() {
    for (int i = 0; i < POOL_PAGES; i++) {
        if (!used[i]) {
            used[i] = true;
            std::memset((void*)pool[i], 0, 4096);
            return pool[i];
        }
    }
    return 0;
}

uint64_t alloc_page_zeroed() { return alloc_page(); }

void free_page(uint64_t address) {
    frees.push_back(address);
    int i = page_index(address);
    if (i < 0) {
        unknown_frees++;
        std::printf("FAIL - free_page(%llX) is not a page from this pool\n",
                    (unsigned long long)address);
        return;
    }
    if (!used[i]) {
        double_frees++;
        std::printf("FAIL - free_page(%llX) frees a page that is already free\n",
                    (unsigned long long)address);
        return;
    }
    used[i] = false;
}

void init(struct limine_memmap_response*) {}
void reserve_range(uint64_t, uint64_t) {}
uint64_t free_frames() { return 0; }
uint64_t total_frames() { return POOL_PAGES; }
} // namespace PMM

extern "C" void kprint(const char*) {}

volatile struct limine_hhdm_request hhdm_request;
volatile struct limine_kernel_address_request kernel_address_request;

// --- table building --------------------------------------------------------

static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        std::printf("FAIL - %s\n", what);
        failures++;
    }
}

static uint64_t* table(uint64_t phys) { return (uint64_t*)phys; }

// A present entry pointing at 'next'. The level-4 entries are leaves; the
// walk never looks at flags other than PRESENT and SHARED.
static void point(uint64_t* t, int i, uint64_t next, uint64_t flags) {
    t[i] = (next & PAGE_MASK) | flags;
}

// Every page the test allocated and still owns. The pool is a closed set, so
// "how many pages did this space own" is exact: live pages minus the shared
// ones a caller mapped but never owned.
static int live_pages() {
    int n = 0;
    for (int i = 0; i < POOL_PAGES; i++)
        if (used[i]) n++;
    return n;
}

// --- checks ----------------------------------------------------------------

// One space, PDPT -> PD -> two PTs, four leaves of which one is PTE_SHARED.
// Owned: PML4, PDPT, PD, PT, PT, three data frames. The shared leaf belongs to
// whoever mapped it and must survive teardown.
static void teardown_of_a_full_space() {
    uint64_t pml4 = PMM::alloc_page();
    uint64_t pdpt = PMM::alloc_page();
    uint64_t pd = PMM::alloc_page();
    uint64_t pt0 = PMM::alloc_page();
    uint64_t pt1 = PMM::alloc_page();
    uint64_t data0 = PMM::alloc_page();
    uint64_t data1 = PMM::alloc_page();
    uint64_t data2 = PMM::alloc_page();
    uint64_t shared = PMM::alloc_page();

    point(table(pml4), 1, pdpt, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pdpt), 0, pd, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pd), 0, pt0, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pd), 1, pt1, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);

    point(table(pt0), 0, data0, VMM::PTE_PRESENT | VMM::PTE_USER);
    point(table(pt0), 1, data1, VMM::PTE_PRESENT | VMM::PTE_USER);
    point(table(pt0), 2, shared,
          VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_SHARED);
    point(table(pt1), 5, data2, VMM::PTE_PRESENT | VMM::PTE_USER);

    int owned_before = live_pages();
    check(owned_before == 9, "fixture allocated nine pages");

    frees.clear();
    VMM::destroy_user_space(pml4);

    check(double_frees == 0, "no table page is freed twice");
    check(unknown_frees == 0, "every free names a real page");
    check((int)frees.size() == 8,
          "eight pages freed: PML4, two tables deep, and three frames");
    check(live_pages() == 1, "only the shared frame is still in use");
    check(used[page_index(shared)], "the PTE_SHARED frame was not freed");

    // Each owned page appears once. A count alone would also pass if one page
    // were freed twice and another not at all, so this is the real check.
    uint64_t owned[] = {pml4, pdpt, pd, pt0, pt1, data0, data1, data2};
    for (uint64_t p : owned) {
        int n = 0;
        for (uint64_t f : frees)
            if (f == p) n++;
        if (n != 1) {
            std::printf("FAIL - page %llX freed %d times, want 1\n",
                        (unsigned long long)p, n);
            failures++;
        }
        checks++;
    }
}

// A second space destroyed after the first: the walk must not depend on the
// space being the only thing in the pool, and the top user-half entry (255)
// must be included while entries above it are not.
static void teardown_uses_the_user_half_only() {
    uint64_t pml4 = PMM::alloc_page();
    uint64_t pdpt = PMM::alloc_page();
    uint64_t pd = PMM::alloc_page();
    uint64_t pt = PMM::alloc_page();
    uint64_t data = PMM::alloc_page();
    uint64_t kernel_side = PMM::alloc_page();

    point(table(pml4), 255, pdpt, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    // The kernel half is shared, not owned: teardown must leave it alone.
    point(table(pml4), 256, kernel_side,
          VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pdpt), 0, pd, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pd), 0, pt, VMM::PTE_PRESENT | VMM::PTE_WRITABLE);
    point(table(pt), 0, data, VMM::PTE_PRESENT | VMM::PTE_USER);

    int live_before = live_pages();
    frees.clear();
    VMM::destroy_user_space(pml4);

    check(double_frees == 0, "second space: no page is freed twice");
    check((int)frees.size() == 5, "second space: five pages freed");
    check(live_before - live_pages() == 5,
          "pages returned match the pages the space owned");
    check(used[page_index(kernel_side)], "the kernel half is untouched");
}

int main() {
    for (int i = 0; i < POOL_PAGES; i++) {
        void* p = nullptr;
        if (posix_memalign(&p, 4096, 4096) != 0) {
            std::printf("FAIL - cannot allocate the page pool\n");
            return 1;
        }
        std::memset(p, 0, 4096);
        pool[i] = (uint64_t)p;
    }

    teardown_of_a_full_space();
    teardown_uses_the_user_half_only();

    if (failures) {
        std::printf("test_vmm: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_vmm: %d checks passed\n", checks);
    return 0;
}
