// The kernel and the libc describe the same syscall records in two headers that
// cannot include each other: the kernel headers are the kernel's view,
// grv/syscall.h is the ABI's. They have to agree byte for byte, because several
// calls fill the caller's record in place rather than converting it.
//
// Nothing else checks this. The static_asserts in the dispatcher pin the
// kernel's *sizes*, which a field reorder would pass straight through — and that
// is not hypothetical: with the type and the inode swapped, every directory
// reported as a file, and the symptom was a missing '/' in a listing.
//
// Every record that crosses the boundary is pinned here, field for field, plus
// the ABI version both sides report. This runs on the host, so it is part of the
// fast half (`ninja check`).
#include <cstddef>
#include <cstdio>

#include "fs/vfs.h"
#include "kernel/core/fault.h"
#include "kernel/core/ipc.h"
#include "kernel/core/memmap.h"
#include "kernel/core/sched.h"
#include "kernel/core/sysinfo.h"
#include "kernel/dev/mouse.h"
#include "kernel/gfx/compositor.h"
#include "kernel/gfx/framebuffer.h"
// Quoted, and reached through -iquote, so the libc's include directory does not
// shadow the host's <stdio.h> and friends for the standard library this test
// itself uses.
#include "grv/syscall.h"

static int failures = 0;

static void same(const char* what, size_t kernel, size_t abi) {
    if (kernel != abi) {
        std::printf("FAIL: %s: kernel says %zu, the ABI says %zu\n",
                    what, kernel, abi);
        failures++;
    }
}

int main() {
    // The version both sides report. A layout change below must bump it.
    same("ABI_VERSION", ABI_VERSION, GRV_ABI_VERSION);

    // The directory entry, field for field.
    same("sizeof(DirEntry)", sizeof(VFS::DirEntry), sizeof(struct grv_dirent));
    same("offsetof(name)", offsetof(VFS::DirEntry, name),
         offsetof(struct grv_dirent, name));
    same("offsetof(type)", offsetof(VFS::DirEntry, type),
         offsetof(struct grv_dirent, type));
    same("offsetof(ino)", offsetof(VFS::DirEntry, ino),
         offsetof(struct grv_dirent, ino));
    same("NAME_MAX vs name[]", VFS::NAME_MAX,
         sizeof(((struct grv_dirent*)0)->name));

    // grv_stat: the dispatcher writes it as five plain 64-bit fields in this
    // order. StatPkt in src/arch/x86_64/idt.cpp asserts the same numbers from
    // the kernel's side, so the two halves are pinned to one set of values.
    same("sizeof(grv_stat)", 40, sizeof(struct grv_stat));
    same("grv_stat.size", 0, offsetof(struct grv_stat, size));
    same("grv_stat.is_dir", 8, offsetof(struct grv_stat, is_dir));
    same("grv_stat.ino", 16, offsetof(struct grv_stat, ino));
    same("grv_stat.mode", 24, offsetof(struct grv_stat, mode));
    same("grv_stat.mtime", 32, offsetof(struct grv_stat, mtime));

    // grv_statfs: the dispatcher fills it from VFS::StatFs, so the two must
    // line up field for field. The ABI carries the type as a fixed char[16]
    // where the kernel carries a pointer, so only the sizes below the type are
    // pinned; the type's own offset still has to agree.
    same("grv_statfs.total", offsetof(VFS::StatFs, total),
         offsetof(struct grv_statfs, total));
    same("grv_statfs.free", offsetof(VFS::StatFs, free),
         offsetof(struct grv_statfs, free));
    same("grv_statfs.inodes_total", offsetof(VFS::StatFs, inodes_total),
         offsetof(struct grv_statfs, inodes_total));
    same("grv_statfs.inodes_free", offsetof(VFS::StatFs, inodes_free),
         offsetof(struct grv_statfs, inodes_free));
    same("grv_statfs.writable", offsetof(VFS::StatFs, writable),
         offsetof(struct grv_statfs, writable));
    same("grv_statfs.type", offsetof(VFS::StatFs, type),
         offsetof(struct grv_statfs, type));
    same("sizeof(grv_statfs)", 56, sizeof(struct grv_statfs));

    // IPCs: the kernel moves these four words verbatim and stamps 'from'.
    same("sizeof(grv_msg)", sizeof(Ipc::Msg), sizeof(struct grv_msg));
    same("grv_msg.from", offsetof(Ipc::Msg, from),
         offsetof(struct grv_msg, from));
    same("grv_msg.type", offsetof(Ipc::Msg, type),
         offsetof(struct grv_msg, type));
    same("grv_msg.a", offsetof(Ipc::Msg, a), offsetof(struct grv_msg, a));
    same("grv_msg.b", offsetof(Ipc::Msg, b), offsetof(struct grv_msg, b));

    // MOUSERD drains one of these straight into the caller's record.
    same("sizeof(grv_mouse_packet)", sizeof(Mouse::Packet),
         sizeof(struct grv_mouse_packet));
    same("grv_mouse_packet.dx", offsetof(Mouse::Packet, dx),
         offsetof(struct grv_mouse_packet, dx));
    same("grv_mouse_packet.dy", offsetof(Mouse::Packet, dy),
         offsetof(struct grv_mouse_packet, dy));
    same("grv_mouse_packet.buttons", offsetof(Mouse::Packet, buttons),
         offsetof(struct grv_mouse_packet, buttons));
    same("grv_mouse_packet.dz", offsetof(Mouse::Packet, dz),
         offsetof(struct grv_mouse_packet, dz));

    // FBINFO copies the mapped framebuffer's geometry into this record.
    same("sizeof(grv_fb_info)", sizeof(Framebuffer::Info),
         sizeof(struct grv_fb_info));
    same("grv_fb_info.width", offsetof(Framebuffer::Info, width),
         offsetof(struct grv_fb_info, width));
    same("grv_fb_info.height", offsetof(Framebuffer::Info, height),
         offsetof(struct grv_fb_info, height));
    same("grv_fb_info.pitch", offsetof(Framebuffer::Info, pitch),
         offsetof(struct grv_fb_info, pitch));
    same("grv_fb_info.bpp", offsetof(Framebuffer::Info, bpp),
         offsetof(struct grv_fb_info, bpp));
    same("grv_fb_info.address", offsetof(Framebuffer::Info, address),
         offsetof(struct grv_fb_info, address));
    same("grv_fb_info.size", offsetof(Framebuffer::Info, size),
         offsetof(struct grv_fb_info, size));

    // SURFACE_CREATE/SURFACE_LIST/SURFACE_RESIZE write this record whole.
    same("sizeof(grv_surface_info)", sizeof(Compositor::Info),
         sizeof(struct grv_surface_info));
    same("grv_surface_info.address", offsetof(Compositor::Info, address),
         offsetof(struct grv_surface_info, address));
    same("grv_surface_info.width", offsetof(Compositor::Info, width),
         offsetof(struct grv_surface_info, width));
    same("grv_surface_info.height", offsetof(Compositor::Info, height),
         offsetof(struct grv_surface_info, height));
    same("grv_surface_info.id", offsetof(Compositor::Info, id),
         offsetof(struct grv_surface_info, id));
    same("grv_surface_info.owner", offsetof(Compositor::Info, owner),
         offsetof(struct grv_surface_info, owner));
    same("grv_surface_info.x", offsetof(Compositor::Info, x),
         offsetof(struct grv_surface_info, x));
    same("grv_surface_info.y", offsetof(Compositor::Info, y),
         offsetof(struct grv_surface_info, y));
    same("grv_surface_info.z", offsetof(Compositor::Info, z),
         offsetof(struct grv_surface_info, z));

    // MAP_FILE and SHM_MAP return a mapping's address and length.
    same("sizeof(grv_map)", sizeof(MemMap::Mapping), sizeof(struct grv_map));
    same("grv_map.address", offsetof(MemMap::Mapping, address),
         offsetof(struct grv_map, address));
    same("grv_map.size", offsetof(MemMap::Mapping, size),
         offsetof(struct grv_map, size));

    // SYSINFO is the snapshot the monitor prints, and now carries the ABI
    // version at the end so new fields never move an old offset.
    same("sizeof(grv_sysinfo)", sizeof(SysInfo), sizeof(struct grv_sysinfo));
    same("grv_sysinfo.ticks", offsetof(SysInfo, ticks),
         offsetof(struct grv_sysinfo, ticks));
    same("grv_sysinfo.tasks", offsetof(SysInfo, tasks),
         offsetof(struct grv_sysinfo, tasks));
    same("grv_sysinfo.mem_free", offsetof(SysInfo, mem_free),
         offsetof(struct grv_sysinfo, mem_free));
    same("grv_sysinfo.mem_total", offsetof(SysInfo, mem_total),
         offsetof(struct grv_sysinfo, mem_total));
    same("grv_sysinfo.heap_used", offsetof(SysInfo, heap_used),
         offsetof(struct grv_sysinfo, heap_used));
    same("grv_sysinfo.heap_peak", offsetof(SysInfo, heap_peak),
         offsetof(struct grv_sysinfo, heap_peak));
    same("grv_sysinfo.heap_total", offsetof(SysInfo, heap_total),
         offsetof(struct grv_sysinfo, heap_total));
    same("grv_sysinfo.surfaces", offsetof(SysInfo, surfaces),
         offsetof(struct grv_sysinfo, surfaces));
    same("grv_sysinfo.comp_presents", offsetof(SysInfo, comp_presents),
         offsetof(struct grv_sysinfo, comp_presents));
    same("grv_sysinfo.comp_full", offsetof(SysInfo, comp_full),
         offsetof(struct grv_sysinfo, comp_full));
    same("grv_sysinfo.comp_rects", offsetof(SysInfo, comp_rects),
         offsetof(struct grv_sysinfo, comp_rects));
    same("grv_sysinfo.comp_pixels", offsetof(SysInfo, comp_pixels),
         offsetof(struct grv_sysinfo, comp_pixels));
    same("grv_sysinfo.screen_w", offsetof(SysInfo, screen_w),
         offsetof(struct grv_sysinfo, screen_w));
    same("grv_sysinfo.screen_h", offsetof(SysInfo, screen_h),
         offsetof(struct grv_sysinfo, screen_h));
    same("grv_sysinfo.cursor_x", offsetof(SysInfo, cursor_x),
         offsetof(struct grv_sysinfo, cursor_x));
    same("grv_sysinfo.cursor_y", offsetof(SysInfo, cursor_y),
         offsetof(struct grv_sysinfo, cursor_y));
    same("grv_sysinfo.abi_version", offsetof(SysInfo, abi_version),
         offsetof(struct grv_sysinfo, abi_version));

    // TASK_LIST copies an array of these.
    same("sizeof(grv_taskinfo)", sizeof(Sched::TaskInfo),
         sizeof(struct grv_taskinfo));
    same("grv_taskinfo.id", offsetof(Sched::TaskInfo, id),
         offsetof(struct grv_taskinfo, id));
    same("grv_taskinfo.state", offsetof(Sched::TaskInfo, state),
         offsetof(struct grv_taskinfo, state));
    same("grv_taskinfo.cr3", offsetof(Sched::TaskInfo, cr3),
         offsetof(struct grv_taskinfo, cr3));
    same("grv_taskinfo.brk", offsetof(Sched::TaskInfo, brk),
         offsetof(struct grv_taskinfo, brk));
    same("grv_taskinfo.waiting_on", offsetof(Sched::TaskInfo, waiting_on),
         offsetof(struct grv_taskinfo, waiting_on));
    same("grv_taskinfo.name", offsetof(Sched::TaskInfo, name),
         offsetof(struct grv_taskinfo, name));

    // FAULT_LAST copies the kernel's crash record whole.
    same("sizeof(grv_fault_record)", sizeof(Fault::Record),
         sizeof(struct grv_fault_record));
    same("grv_fault_record.valid", offsetof(Fault::Record, valid),
         offsetof(struct grv_fault_record, valid));
    same("grv_fault_record.task_id", offsetof(Fault::Record, task_id),
         offsetof(struct grv_fault_record, task_id));
    same("grv_fault_record.task_name", offsetof(Fault::Record, task_name),
         offsetof(struct grv_fault_record, task_name));
    same("grv_fault_record.vector", offsetof(Fault::Record, vector),
         offsetof(struct grv_fault_record, vector));
    same("grv_fault_record.error_code", offsetof(Fault::Record, error_code),
         offsetof(struct grv_fault_record, error_code));
    same("grv_fault_record.cr2", offsetof(Fault::Record, cr2),
         offsetof(struct grv_fault_record, cr2));
    same("grv_fault_record.rip", offsetof(Fault::Record, rip),
         offsetof(struct grv_fault_record, rip));
    same("grv_fault_record.rsp", offsetof(Fault::Record, rsp),
         offsetof(struct grv_fault_record, rsp));
    same("grv_fault_record.rflags", offsetof(Fault::Record, rflags),
         offsetof(struct grv_fault_record, rflags));
    same("grv_fault_record.cs", offsetof(Fault::Record, cs),
         offsetof(struct grv_fault_record, cs));
    same("grv_fault_record.rax", offsetof(Fault::Record, rax),
         offsetof(struct grv_fault_record, rax));
    same("grv_fault_record.rbx", offsetof(Fault::Record, rbx),
         offsetof(struct grv_fault_record, rbx));
    same("grv_fault_record.rcx", offsetof(Fault::Record, rcx),
         offsetof(struct grv_fault_record, rcx));
    same("grv_fault_record.rdx", offsetof(Fault::Record, rdx),
         offsetof(struct grv_fault_record, rdx));
    same("grv_fault_record.rsi", offsetof(Fault::Record, rsi),
         offsetof(struct grv_fault_record, rsi));
    same("grv_fault_record.rdi", offsetof(Fault::Record, rdi),
         offsetof(struct grv_fault_record, rdi));
    same("grv_fault_record.rbp", offsetof(Fault::Record, rbp),
         offsetof(struct grv_fault_record, rbp));
    same("grv_fault_record.r8", offsetof(Fault::Record, r8),
         offsetof(struct grv_fault_record, r8));
    same("grv_fault_record.r9", offsetof(Fault::Record, r9),
         offsetof(struct grv_fault_record, r9));
    same("grv_fault_record.r10", offsetof(Fault::Record, r10),
         offsetof(struct grv_fault_record, r10));
    same("grv_fault_record.r11", offsetof(Fault::Record, r11),
         offsetof(struct grv_fault_record, r11));
    same("grv_fault_record.r12", offsetof(Fault::Record, r12),
         offsetof(struct grv_fault_record, r12));
    same("grv_fault_record.r13", offsetof(Fault::Record, r13),
         offsetof(struct grv_fault_record, r13));
    same("grv_fault_record.r14", offsetof(Fault::Record, r14),
         offsetof(struct grv_fault_record, r14));
    same("grv_fault_record.r15", offsetof(Fault::Record, r15),
         offsetof(struct grv_fault_record, r15));

    if (failures == 0) {
        std::printf("test_abi: the kernel and the ABI agree (version %d)\n",
                    GRV_ABI_VERSION);
        return 0;
    }
    std::printf("test_abi: %d mismatch(es)\n", failures);
    return 1;
}
