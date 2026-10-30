// The path rule is one rule, and its mechanics are pure. This test drives
// FsPath directly: normalisation ('.', '..', repeated and trailing slashes),
// the relative join that mkdir/unlink/rename use, component splitting, the name
// and path limits, and mount-prefix matching.
//
// It runs on the host, so it is part of the fast half (`ninja check`). The guest
// test (pathtest.elf) checks the call sites: that mkdir, unlink and rename
// really pass the CWD to the join this test pins.
#include <cstdio>
#include <cstring>

#include "fs/path.h"

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        std::printf("FAIL - %s\n", what);
        failures++;
    }
}

static void expect_normalize(const char* in, const char* want) {
    char out[FsPath::PATH_MAX];
    bool ok = FsPath::normalize(in, out, sizeof(out));
    if (!ok || std::strcmp(out, want) != 0) {
        std::printf("FAIL - normalize(\"%s\") = %s\"%s\", want \"%s\"\n",
                    in, ok ? "" : "(refused) ", out, want);
        failures++;
    }
    checks++;
}

static void expect_join(const char* dir, const char* name, const char* want) {
    char out[FsPath::PATH_MAX];
    bool ok = FsPath::join(dir, name, out, sizeof(out));
    if (!ok || std::strcmp(out, want) != 0) {
        std::printf("FAIL - join(\"%s\", \"%s\") = %s\"%s\", want \"%s\"\n",
                    dir, name, ok ? "" : "(refused) ", out, want);
        failures++;
    }
    checks++;
}

static void expect_absolute(const char* path, const char* cwd, const char* want) {
    char out[FsPath::PATH_MAX];
    bool ok = FsPath::absolute(path, cwd, out, sizeof(out));
    if (!ok || std::strcmp(out, want) != 0) {
        std::printf("FAIL - absolute(\"%s\", cwd \"%s\") = %s\"%s\", want \"%s\"\n",
                    path, cwd, ok ? "" : "(refused) ", out, want);
        failures++;
    }
    checks++;
}

static void expect_split(const char* rel, bool want_ok, const char* want_parent,
                         const char* want_name) {
    char parent[FsPath::PATH_MAX];
    char name[FsPath::NAME_MAX];
    FsPath::SplitResult r = FsPath::split(rel, parent, sizeof(parent),
                                          name, sizeof(name));
    bool ok = (r == FsPath::SPLIT_OK) == want_ok;
    if (ok && want_ok)
        ok = std::strcmp(parent, want_parent) == 0 &&
             std::strcmp(name, want_name) == 0;
    if (!ok) {
        std::printf("FAIL - split(\"%s\") = %d \"%s\" \"%s\", want %d \"%s\" \"%s\"\n",
                    rel, (int)r, parent, name, (int)want_ok,
                    want_parent, want_name);
        failures++;
    }
    checks++;
}

int main() {
    // --- normalisation: absolute, '.' and '..', repeated/trailing slashes ---
    expect_normalize("/", "/");
    expect_normalize("", "/");
    expect_normalize("///", "/");
    expect_normalize(".", "/");
    expect_normalize("..", "/");
    expect_normalize("/.", "/");
    expect_normalize("/..", "/");
    expect_normalize("/a", "/a");
    expect_normalize("a", "/a");                    // relative, rooted for shape
    expect_normalize("a/b", "/a/b");
    expect_normalize("//a///b//", "/a/b");
    expect_normalize("/a/b/", "/a/b");
    expect_normalize("/a/./b", "/a/b");
    expect_normalize("/a/../b", "/b");
    expect_normalize("/a/b/../..", "/");
    expect_normalize("/a/../../b", "/b");
    expect_normalize("/a//../b", "/b");
    expect_normalize("/a/b/..", "/a");

    // A canonical path never keeps a trailing slash, even when it was created
    // by ".." popping back to a directory.
    expect_normalize("/a/b/../", "/a");

    // --- the CWD join (what mkdir, unlink and rename resolve through) -------
    // A relative path is joined to the CWD and to nothing else. There is no
    // /bin search on this side of the path rule.
    expect_absolute("d", "/tmp", "/tmp/d");
    expect_absolute("d", "/", "/d");
    expect_absolute("a/b", "/tmp", "/tmp/a/b");
    expect_absolute("/d", "/tmp", "/d");            // absolute is used as it is
    expect_absolute("..", "/tmp", "/tmp/..");
    expect_absolute(".", "/tmp", "/tmp/.");
    expect_absolute("", "/tmp", "/tmp/");
    expect_absolute("d", nullptr, "/d");            // no CWD means the root

    // The old bug in one line: a relative create from /tmp must not name /d.
    {
        char out[FsPath::PATH_MAX];
        check(FsPath::absolute("d", "/tmp", out, sizeof(out)) &&
                  std::strcmp(out, "/d") != 0,
              "a relative path from /tmp does not resolve to /d");
    }

    // --- join: one slash, no truncation ------------------------------------
    expect_join("/tmp", "d", "/tmp/d");
    expect_join("/", "d", "/d");
    expect_join("/tmp/fc", "a.txt", "/tmp/fc/a.txt");
    expect_join("/tmp", "", "/tmp/");

    // --- name limit ---------------------------------------------------------
    check(FsPath::component_fits(0), "an empty component fits");
    check(FsPath::component_fits(1), "a one-byte name fits");
    check(FsPath::component_fits(FsPath::NAME_MAX - 1), "a NAME_MAX-1 name fits");
    check(!FsPath::component_fits(FsPath::NAME_MAX), "a NAME_MAX name is refused");
    check(!FsPath::component_fits(FsPath::NAME_MAX + 1),
          "a name past NAME_MAX is refused");

    // --- next_component: splitting, slashes, the same name limit -----------
    {
        const char* p = "//a//b//";
        char comp[FsPath::NAME_MAX];
        check(FsPath::next_component(&p, comp, sizeof(comp)) == FsPath::COMPONENT_OK &&
                  std::strcmp(comp, "a") == 0,
              "next_component reads the first component past slashes");
        check(FsPath::next_component(&p, comp, sizeof(comp)) == FsPath::COMPONENT_OK &&
                  std::strcmp(comp, "b") == 0,
              "and the second");
        check(FsPath::next_component(&p, comp, sizeof(comp)) == FsPath::COMPONENT_END,
              "and then ends on trailing slashes");

        const char* empty = "";
        check(FsPath::next_component(&empty, comp, sizeof(comp)) == FsPath::COMPONENT_END,
              "an empty path has no components");
    }
    {
        char name[FsPath::NAME_MAX + 2];
        for (int i = 0; i < FsPath::NAME_MAX - 1; i++) name[i] = 'n';
        name[FsPath::NAME_MAX - 1] = '\0';
        const char* p = name;
        char comp[FsPath::NAME_MAX];
        check(FsPath::next_component(&p, comp, sizeof(comp)) == FsPath::COMPONENT_OK &&
                  std::strcmp(comp, name) == 0,
              "next_component accepts a NAME_MAX-1 component");

        name[FsPath::NAME_MAX - 1] = 'n';
        name[FsPath::NAME_MAX] = '\0';
        p = name;
        check(FsPath::next_component(&p, comp, sizeof(comp)) == FsPath::COMPONENT_TOO_LONG,
              "next_component refuses a NAME_MAX component");
    }

    // --- split: parent and final name --------------------------------------
    expect_split("a", true, "", "a");
    expect_split("a/b", true, "a", "b");
    expect_split("a/b/", true, "a", "b");
    expect_split("a/b/c", true, "a/b", "c");
    expect_split("", false, "", "");
    expect_split("/", false, "", "");
    expect_split("a/", true, "", "a");

    {
        char rel[FsPath::NAME_MAX + 2];
        for (int i = 0; i < FsPath::NAME_MAX - 1; i++) rel[i] = 'x';
        rel[FsPath::NAME_MAX - 1] = '\0';
        expect_split(rel, true, "", rel);

        rel[FsPath::NAME_MAX - 1] = 'x';
        rel[FsPath::NAME_MAX] = '\0';
        expect_split(rel, false, "", "");
    }
    {
        // The parent buffer has its own boundary: "ab/c" needs three bytes for
        // "ab" and its NUL. Anything smaller is refused rather than cut down.
        char parent[3];
        char name[FsPath::NAME_MAX];
        check(FsPath::split("ab/c", parent, sizeof(parent), name, sizeof(name)) ==
                  FsPath::SPLIT_OK,
              "split accepts a parent at the buffer boundary");
        check(FsPath::split("ab/c", parent, 2, name, sizeof(name)) ==
                  FsPath::SPLIT_TOO_LONG,
              "split refuses a parent past the buffer boundary");
    }

    // --- PATH_MAX: a path that fits is accepted, one past it is refused -----
    {
        // "/" + (PATH_MAX-2) chars = PATH_MAX-1 bytes, plus the NUL.
        char path[FsPath::PATH_MAX + 1];
        char out[FsPath::PATH_MAX];
        int n = 0;
        path[n++] = '/';
        for (int i = 1; i < FsPath::PATH_MAX - 1; i++) path[n++] = 'p';
        path[n] = '\0';
        check((int)std::strlen(path) == FsPath::PATH_MAX - 1,
              "the longest path is PATH_MAX-1 bytes");
        check(FsPath::normalize(path, out, sizeof(out)),
              "a PATH_MAX-1 path is accepted");

        path[n++] = 'p';                 // one byte past what a 512-byte buffer holds
        path[n] = '\0';
        check((int)std::strlen(path) == FsPath::PATH_MAX,
              "one more byte is PATH_MAX");
        out[0] = '\0';
        check(!FsPath::normalize(path, out, sizeof(out)),
              "a PATH_MAX path is refused, not truncated");
        check(out[0] == '\0', "and a refusal leaves nothing that could be used");
    }

    // --- component depth ---------------------------------------------------
    {
        char path[2 * FsPath::MAX_COMPONENTS + 4];
        int n = 0;
        for (int i = 0; i < FsPath::MAX_COMPONENTS; i++) { path[n++] = '/'; path[n++] = 'd'; }
        path[n] = '\0';
        char out[FsPath::PATH_MAX];
        check(FsPath::normalize(path, out, sizeof(out)),
              "MAX_COMPONENTS components are accepted");

        path[n++] = '/'; path[n++] = 'd';
        path[n] = '\0';
        check(!FsPath::normalize(path, out, sizeof(out)),
              "one component past MAX_COMPONENTS is refused");
    }

    // --- mount prefixes: longest match, boundary, and a lookalike ----------
    const char* mounts[] = { "/", "/bin", "/bin/tools" };
    const int count = (int)(sizeof(mounts) / sizeof(mounts[0]));

    check(FsPath::longest_mount("/bin/tools/x.elf", mounts, count) == 2,
          "the longest mount wins under /bin/tools");
    check(FsPath::longest_mount("/bin/tools", mounts, count) == 2,
          "the mount point itself resolves to its own mount");
    check(FsPath::longest_mount("/bin/ls.elf", mounts, count) == 1,
          "/bin is chosen for a file directly under it");
    check(FsPath::longest_mount("/bin", mounts, count) == 1,
          "the /bin mount point resolves to /bin");
    check(FsPath::longest_mount("/tmp", mounts, count) == 0,
          "an unmounted path under the root goes to the root mount");
    check(FsPath::longest_mount("/", mounts, count) == 0,
          "the root path goes to the root mount");

    // The same-looking prefix that is not a mount. "/binary" must not resolve
    // into the /bin filesystem just because it starts with those characters.
    check(FsPath::longest_mount("/binary", mounts, count) == 0,
          "/binary is not inside /bin");
    check(FsPath::longest_mount("/bin2/x", mounts, count) == 0,
          "/bin2 is not inside /bin");
    check(FsPath::longest_mount("/bin/toolshed", mounts, count) == 1,
          "/bin/toolshed is inside /bin, not /bin/tools");

    check(FsPath::mount_covers("/bin", "/bin"), "a mount covers itself");
    check(FsPath::mount_covers("/bin", "/bin/x"), "a mount covers its children");
    check(!FsPath::mount_covers("/bin", "/binary"), "a mount does not cover a lookalike");
    check(!FsPath::mount_covers("/bin", "/bin2"), "a mount stops at a component boundary");
    check(!FsPath::mount_covers("/bin", "/bi"), "a shorter path is not covered");
    check(FsPath::mount_covers("/", "/anything"), "the root mount covers everything");

    const char* only_bin[] = { "/bin" };
    check(FsPath::longest_mount("/tmp", only_bin, 1) == -1,
          "no mount at all is reported as no mount");

    if (failures == 0) {
        std::printf("test_path: all %d path checks pass\n", checks);
        return 0;
    }
    std::printf("test_path: %d of %d path checks failed\n", failures, checks);
    return 1;
}
