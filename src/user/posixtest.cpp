#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// posixtest — exercises the POSIX facade (fcntl/unistd/sys-stat/dirent) against
// the filesystem and the console descriptors 0/1/2. Prints PASS/FAIL for the
// test harness.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("posixtest: FAIL: %s\n", what); fails++; }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* path = "/tmp/posix.txt";
    unlink(path);   // clean slate if a previous run left it

    // --- open / write / lseek / close ---------------------------------
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open O_CREAT|O_TRUNC");
    if (fd < 0) { printf("posixtest: %d check(s) failed\n", fails); return 1; }
    check(write(fd, "hello posix", 11) == 11, "write");
    check(lseek(fd, 0, SEEK_SET) == 0, "lseek");
    check(close(fd) == 0, "close");

    // --- stat ----------------------------------------------------------
    struct stat st;
    check(stat(path, &st) == 0, "stat");
    check(st.st_size == 11, "stat size");
    check(S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode), "stat mode is regular");

    fd = open(path, O_RDONLY);
    check(fd >= 0, "open O_RDONLY");
    char buf[32];
    check(read(fd, buf, 11) == 11 && memcmp(buf, "hello posix", 11) == 0, "read");
    check(close(fd) == 0, "close read");

    // --- the console descriptors --------------------------------------
    check(write(STDOUT_FILENO, "posixtest: via write(1)\n", 24) == 24, "write(1)");
    check(access(path, F_OK) == 0, "access existing");
    check(access("/tmp/no-such-file", F_OK) == -1, "access missing");

    // --- getcwd / chdir -------------------------------------------------
    char cwd[64];
    check(getcwd(cwd, sizeof cwd) != 0 && cwd[0] == '/', "getcwd");
    check(chdir("/tmp") == 0, "chdir /tmp");
    check(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/tmp") == 0,
          "getcwd after chdir");
    check(chdir("/") == 0, "chdir back");

    // --- dirent ---------------------------------------------------------
    DIR* d = opendir("/bin");
    check(d != 0, "opendir /bin");
    if (d != 0) {
        int count = 0, saw_tests = 0;
        struct dirent* e;
        while ((e = readdir(d)) != 0) {
            count++;
            if (strcmp(e->d_name, "tests") == 0 && e->d_type == DT_DIR)
                saw_tests = 1;
        }
        check(count > 0, "readdir found entries");
        check(saw_tests, "readdir: tests is a directory");
        check(closedir(d) == 0, "closedir");
    }

    check(stat("/bin", &st) == 0 && S_ISDIR(st.st_mode), "stat a directory");

    // --- errors ---------------------------------------------------------
    errno = 0;
    check(open("/tmp/no-such-file", O_RDONLY) == -1 && errno == ENOENT,
          "open missing -> ENOENT");
    errno = 0;
    // The image tree is writable through the overlay: opening an image file for
    // writing copies it into the upper layer (O_TRUNC would shadow it instead).
    int wfd = open("/hello.txt", O_WRONLY | O_CREAT);
    check(wfd >= 0, "open an image file for writing (overlay)");
    if (wfd >= 0) close(wfd);
    errno = 0;
    check(opendir("/hello.txt") == 0, "opendir a file fails");

    // --- mkdir / rmdir --------------------------------------------------
    unlink("/tmp/pd");
    check(mkdir("/tmp/pd", 0755) == 0, "mkdir");
    check(stat("/tmp/pd", &st) == 0 && S_ISDIR(st.st_mode), "mkdir then stat");
    check(rmdir("/tmp/pd") == 0, "rmdir");

    check(unlink(path) == 0, "unlink");
    check(access(path, F_OK) == -1, "gone after unlink");

    if (fails == 0) { printf("posixtest: all checks pass\n"); return 0; }
    printf("posixtest: %d check(s) failed\n", fails);
    return 1;
}
