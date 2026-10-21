#include <stdio.h>
#include <unistd.h>

// sync — empty every write cache the system is holding. Like the Unix command,
// it says nothing when it worked: the point is the state of the disk, not
// output. A failure is worth a line and a non-zero exit.
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (sync() != 0) {
        printf("sync: a filesystem flush failed\n");
        return 1;
    }
    return 0;
}
