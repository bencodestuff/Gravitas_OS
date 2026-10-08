#include <stdio.h>
#include <grv/syscall.h>

// ps — what is running. The kernel reports the tasks; deciding what to show, and
// in what order, is here.

static const char* state_name(int s) {
    switch (s) {
        case 0: return "ready";
        case 1: return "sleeping";
        case 2: return "running";
        default: return "?";
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    static struct grv_taskinfo tasks[64];
    int n = grv_task_list(tasks, 64);
    if (n < 0) n = 0;

    printf("  PID STATE     WAITING  NAME\n");
    for (int i = 0; i < n; i++) {
        char wait[16];
        if (tasks[i].waiting_on < 0) {
            wait[0] = '-'; wait[1] = '\0';
        } else {
            snprintf(wait, sizeof(wait), "%ld", tasks[i].waiting_on);
        }
        printf("%5d %-9s %8s  %s\n", tasks[i].id, state_name(tasks[i].state),
               wait, tasks[i].name);
    }
    return 0;
}
