#include <stdio.h>
#include <grv/syscall.h>

// mon — system monitor (the M7 debugger's userland half).
//
// Prints a snapshot of kernel state: counters, the task table, the surface
// registry, and the last recorded crash. Read-only — it only uses the
// introspection syscalls (SYSINFO / TASK_LIST / FAULT_LAST and the M4
// surface list). Run it again for a fresh snapshot; press F12 to bring the
// console (and this output) above any window.

static const char* state_name(int st) {
    if (st == 2) return "running";
    if (st == 1) return "sleeping";
    return "ready";
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_sysinfo si;
    grv_sysinfo(&si);
    printf("mon: ticks %lu, tasks %lu, surfaces %lu\n",
           si.ticks, si.tasks, si.surfaces);
    printf("mon: mem %lu/%lu frames free (%lu MB), heap %lu/%lu B used, peak %lu\n",
           si.mem_free, si.mem_total, (si.mem_free * 4096) / (1024 * 1024),
           si.heap_used, si.heap_total, si.heap_peak);
    printf("mon: compositor %lu presents, %lu full, %lu rects, %lu px\n",
           si.comp_presents, si.comp_full, si.comp_rects, si.comp_pixels);

    struct grv_taskinfo tasks[32];
    int n = grv_task_list(tasks, 32);
    printf("mon: tasks (%d):\n", n);
    for (int i = 0; i < n; i++) {
        printf("mon:   #%d %s %s cr3=%lx\n",
               tasks[i].id, tasks[i].name, state_name(tasks[i].state),
               tasks[i].cr3);
    }

    struct grv_surface_info surfs[32];
    int ns = grv_surface_list(surfs, 32);
    printf("mon: surfaces (%d):\n", ns);
    for (int i = 0; i < ns; i++) {
        printf("mon:   #%d owner=%d %lux%lu at (%ld,%ld) z=%lu\n",
               (int)surfs[i].id, (int)surfs[i].owner,
               surfs[i].width, surfs[i].height,
               surfs[i].x, surfs[i].y, surfs[i].z);
    }

    struct grv_fault_record fr;
    if (grv_fault_last(&fr) == 1) {
        printf("mon: last fault: task '%s' #%ld vector %lu RIP=%lx\n",
               fr.task_name, fr.task_id, fr.vector, fr.rip);
    } else {
        printf("mon: last fault: none\n");
    }
    return 0;
}
