#pragma once
#include <stdint.h>

// memmap — mappings of memory the task does not own.
//
// Two things live here, and they share one table because they share a whole
// lifecycle: a *file mapping* (a slice of the immutable image, read-only,
// zero-copy) and a *shared region* (anonymous pages one task creates and others
// are granted). Both map frames into a task's address space that the task does
// not own, so both mark their leaves PTE_SHARED — the address-space teardown
// clears the mapping and leaves the frames alone — and both need a record so a
// mapping can be dropped by address and a region's last reference released when
// a task dies.

namespace MemMap {

// --- shared regions ---
// Create a region of 'size' bytes (rounded up to pages), zeroed. Returns its id
// (the caller's handle), or -1.
int create(uint64_t size);

// Let 'task' attach the region (owner only). 0, or -1.
int grant(int id, int task);

// Map the region into the CALLER's address space and fill the address of its
// first byte plus its size. 0, or -1 (unknown id, or not granted to us).
int attach(int id, uint64_t* out_addr, uint64_t* out_size);

// Drop the caller's handle. The frames go when the last handle and the last
// mapping are gone.
int close(int id);

// --- file mappings ---
// Map a file read-only and zero-copy: the bytes are the image's own frames.
// Fills the address of its first byte and its size. 0, or -1.
int map_file(const char* path, uint64_t* out_addr, uint64_t* out_size);

// --- both ---
// Drop the mapping that begins at the address attach()/map_file() returned.
int unmap(uint64_t addr);

// Release everything a dying task held: its mappings, and its handles.
void task_gone(int task_id);

} // namespace MemMap
