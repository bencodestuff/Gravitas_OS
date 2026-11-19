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
uint32_t refs[POOL_PAGES];
std::vector<uint64_t> frees; // every deliberate free_page() call, in order
int double_frees = 0;
int unknown_frees = 0;
int allocs_until_failure = -1;
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
    if (allocs_until_failure == 0) return 0;
    if (allocs_until_failure > 0) allocs_until_failure--;
    for (int i = 0; i < POOL_PAGES; i++) {
        if (!used[i]) {
            used[i] = true;
            refs[i] = 1;
            std::memset((void*)pool[i], 0, 4096);
            return pool[i];
        }
    }
    return 0;
}

uint64_t alloc_page_zeroed() { return alloc_page(); }
bool retain_page(uint64_t address) {
    int i = page_index(address);
    if (i < 0 || !used[i] || refs[i] == UINT32_MAX) return false;
    refs[i]++;
    return true;
}
uint32_t page_references(uint64_t address) {
    int i = page_index(address);
    return i < 0 ? 0 : refs[i];
}

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
    if (refs[i] > 1) { refs[i]--; return; }
    refs[i] = 0;
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

// A reservation must cover every page it hands out, not just the first.
// range_free() tests pages one at a time, so a range guarded only at its base
// has a tail that reads as free and a second allocation can start inside it.
// That overlap is not cosmetic: whoever maps the second range clears the
// first one's PTEs on the way out and the first task faults.
static void a_reserved_range_covers_its_whole_length() {
    int live_before = live_pages();
    uint64_t pml4 = PMM::alloc_page();
    check(pml4 != 0, "space for the allocator test");

    uint64_t pages = 4;
    uint64_t first = VMM::alloc_user_range(pml4, pages);
    check(first != 0, "the allocator returns a range");
    check(first == VMM::USER_VA_BASE, "it starts at the bottom of the arena");

    check(!VMM::user_range_free(pml4, first, pages),
          "the whole reserved range reads as taken");
    check(!VMM::user_range_free(pml4, first + 4096, pages - 1),
          "the tail of a reservation is not free");

    uint64_t second = VMM::alloc_user_range(pml4, pages);
    check(second != 0, "a second range can still be allocated");
    check(second >= first + pages * 4096,
          "the second range starts after the first, not inside it");

    // Neither map_page_in() nor unmap_user_range() can run here: both finish
    // by touching CR3 or the TLB, which a host process may not do. The
    // release they perform is unchanged by this fix, so what is pinned below
    // is the two things the fix introduces: the overlap, and that a space
    // whose tables hold nothing but markers is still torn down cleanly.
    int owned = live_pages() - live_before;
    frees.clear();
    VMM::destroy_user_space(pml4);
    check(double_frees == 0, "reservation tables are not freed twice");
    check((int)frees.size() == owned,
          "teardown returns every page the reserved space owned");
    check(live_pages() == live_before, "nothing is left allocated");
}

// A reservation that runs out of table pages part way through must take its
// markers back out, or every ENOMEM leaks the pages it had claimed.
static void a_failed_reservation_leaves_nothing_behind() {
    int live_before = live_pages();
    uint64_t pml4 = PMM::alloc_page();
    uint64_t first = VMM::alloc_user_range(pml4, 1);
    check(first != 0, "the range that will be left standing allocates");

    std::vector<uint64_t> held;
    for (uint64_t p = PMM::alloc_page(); p != 0; p = PMM::alloc_page())
        held.push_back(p); // drain the pool so a second table cannot exist

    // 600 pages need two page tables and only one exists, so this fails after
    // it has marked the 511 pages that fit in the first.
    uint64_t big = VMM::alloc_user_range(pml4, 600);
    check(big == 0, "a reservation that cannot get a table fails");
    check(!VMM::user_range_free(pml4, first, 1),
          "the range that did allocate is still reserved");
    check(VMM::user_range_free(pml4, first + 4096, 511),
          "the guards installed before the failure were released");

    for (uint64_t p : held) PMM::free_page(p);
    frees.clear();
    VMM::destroy_user_space(pml4);
    check(double_frees == 0, "failed reservation: no page freed twice");
    check(live_pages() == live_before, "failed reservation leaves no leak");
}

static void cow_fault_and_copy_to_user() {
    uint64_t pml4 = PMM::alloc_page();
    uint64_t pdpt = PMM::alloc_page();
    uint64_t pd = PMM::alloc_page();
    uint64_t pt = PMM::alloc_page();
    uint64_t data = PMM::alloc_page();
    constexpr uint64_t va = 0x400000;
    auto* l4 = (uint64_t*)pml4;
    auto* l3 = (uint64_t*)pdpt;
    auto* l2 = (uint64_t*)pd;
    auto* l1 = (uint64_t*)pt;
    l4[(va >> 39) & 511] = pdpt | 7;
    l3[(va >> 30) & 511] = pd | 7;
    l2[(va >> 21) & 511] = pt | 7;
    l1[(va >> 12) & 511] = data | VMM::PTE_PRESENT | VMM::PTE_USER |
                              VMM::PTE_COW;
    std::memset((void*)data, 0x11, 4096);
    check(PMM::retain_page(data), "a CoW mapping retains its source frame");
    check(VMM::resolve_cow_fault(pml4, va + 17),
          "a CoW write fault resolves");
    uint64_t resolved = l1[(va >> 12) & 511];
    check((resolved & VMM::PTE_WRITABLE) && !(resolved & VMM::PTE_COW),
          "resolved mapping is writable and no longer CoW");
    check((resolved & 0x000FFFFFFFFFF000ull) != data,
          "a multiply referenced frame is copied");
    check(((uint8_t*)VMM::phys_to_kernel(
              resolved & 0x000FFFFFFFFFF000ull))[0] == 0x11,
          "new mapping contains a private copy of the source");
    check(((uint8_t*)data)[0] == 0x11, "the shared source remains unchanged");
    const char payload[] = "safe";
    check(VMM::copy_to_user(pml4, va + 100, payload, sizeof(payload)),
          "copy_to_user writes through the kernel mapping");
    uint8_t* copied = (uint8_t*)VMM::phys_to_kernel(
        resolved & 0x000FFFFFFFFFF000ull);
    check(std::memcmp(copied + 100, payload, sizeof(payload)) == 0,
          "copy_to_user copied the expected bytes");
    check(!VMM::copy_to_user(pml4, VMM::USER_VA_LIMIT, payload, 1),
          "copy_to_user rejects kernel-half addresses");
    VMM::destroy_user_space(pml4);
}

static uint64_t make_clone_source(uint64_t va, uint64_t data, uint64_t flags) {
    uint64_t pml4 = PMM::alloc_page();
    uint64_t pdpt = PMM::alloc_page();
    uint64_t pd = PMM::alloc_page();
    uint64_t pt = PMM::alloc_page();
    if (!pml4 || !pdpt || !pd || !pt) return 0;
    table(pml4)[(va >> 39) & 511] = pdpt | 7;
    table(pdpt)[(va >> 30) & 511] = pd | 7;
    table(pd)[(va >> 21) & 511] = pt | 7;
    table(pt)[(va >> 12) & 511] = data | flags;
    return pml4;
}

static void clone_retains_and_preserves_owned_leaves(bool child_first) {
    constexpr uint64_t writable_va = 0x400000;
    constexpr uint64_t readonly_va = 0x401000;
    uint64_t writable = PMM::alloc_page();
    uint64_t readonly = PMM::alloc_page();
    std::memset((void*)writable, 0x5a, 4096);
    std::memset((void*)readonly, 0xa5, 4096);
    uint64_t source = make_clone_source(
        writable_va, writable, VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_WRITABLE);
    // Same page-table branch, second leaf, read-only and owned by the source.
    uint64_t* source_pt = table(table(table(table(source)[0] & PAGE_MASK)[0] & PAGE_MASK)[2] & PAGE_MASK);
    source_pt[(readonly_va >> 12) & 511] =
        readonly | VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_NO_CACHE |
        (1ull << 63);
    uint64_t child = VMM::clone_user_space(source);
    check(child != 0, "clone of writable and read-only leaves succeeds");
    uint64_t parent_pte = VMM::leaf_pte_in(source, writable_va);
    uint64_t child_pte = VMM::leaf_pte_in(child, writable_va);
    check((parent_pte & PAGE_MASK) == writable,
          "parent CoW entry retains its physical frame");
    check((child_pte & PAGE_MASK) == writable,
          "child CoW entry maps the same physical frame");
    check(!(parent_pte & VMM::PTE_WRITABLE) && (parent_pte & VMM::PTE_COW),
          "parent writable mapping becomes CoW");
    check(((uint8_t*)writable)[0] == 0x5a &&
          ((uint8_t*)VMM::phys_to_kernel(child_pte & PAGE_MASK))[0] == 0x5a,
          "clone preserves the source frame bytes");
    uint64_t ro_child = VMM::leaf_pte_in(child, readonly_va);
    check((ro_child & PAGE_MASK) == readonly &&
          (ro_child & VMM::PTE_NO_CACHE) &&
          (ro_child & (1ull << 63)) &&
          !(ro_child & VMM::PTE_WRITABLE),
          "read-only owned mapping keeps low and high PTE flags");
    check(PMM::page_references(readonly) == 2,
          "read-only owned frame is retained for the child");
    if (child_first) {
        VMM::destroy_user_space(child);
        check(PMM::page_references(readonly) == 1 && PMM::page_references(writable) == 1,
              "child teardown releases only its frame references");
        VMM::destroy_user_space(source);
    } else {
        VMM::destroy_user_space(source);
        check(PMM::page_references(readonly) == 1 && PMM::page_references(writable) == 1,
              "parent teardown leaves child frame references alive");
        VMM::destroy_user_space(child);
    }
    check(!used[page_index(writable)] && !used[page_index(readonly)],
          "both teardown orders release owned frames exactly once");
}

static void failed_clone_rolls_back_retained_frames() {
    constexpr uint64_t va0 = 0x400000;
    constexpr uint64_t va1 = 0x800000;
    uint64_t data = PMM::alloc_page();
    uint64_t source = make_clone_source(
        va0, data, VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_WRITABLE);
    // A second leaf in a separate top-level branch forces an allocation after
    // the first leaf has already been retained and mapped into the child.
    uint64_t d1 = PMM::alloc_page();
    uint64_t second_pt = PMM::alloc_page();
    uint64_t* source_pd = table(table(table(source)[0] & PAGE_MASK)[0] & PAGE_MASK);
    source_pd[(va1 >> 21) & 511] = second_pt | 7;
    table(second_pt)[(va1 >> 12) & 511] =
        d1 | VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_WRITABLE;
    allocs_until_failure = 4;
    uint64_t child = VMM::clone_user_space(source);
    allocs_until_failure = -1;
    check(child == 0, "clone fails when a later page-table allocation fails");
    check(PMM::page_references(data) == 1 && PMM::page_references(d1) == 1,
          "failed clone rolls back every retained leaf");
    check((VMM::leaf_pte_in(source, va0) & PAGE_MASK) == data,
          "failed clone leaves parent physical mapping intact");
    VMM::destroy_user_space(source);
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
    a_reserved_range_covers_its_whole_length();
    a_failed_reservation_leaves_nothing_behind();
    cow_fault_and_copy_to_user();
    clone_retains_and_preserves_owned_leaves(true);
    clone_retains_and_preserves_owned_leaves(false);
    failed_clone_rolls_back_retained_frames();

    if (failures) {
        std::printf("test_vmm: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_vmm: %d checks passed\n", checks);
    return 0;
}
