#include "fs/path.h"

namespace FsPath {

static uint64_t str_len(const char* s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

bool is_absolute(const char* path) {
    return path != nullptr && path[0] == '/';
}

bool component_fits(uint64_t len) {
    return len < (uint64_t)NAME_MAX;
}

bool normalize(const char* path, char* out, uint64_t out_max) {
    // A canonical path is at least "/" plus its NUL.
    if (out == nullptr || out_max < 2) return false;

    // Component starts, as offsets into out. On the stack, like every other
    // buffer on this path: a syscall can be preempted by the timer, so a static
    // would be shared with whatever task runs next.
    uint64_t starts[MAX_COMPONENTS];
    int depth = 0;
    uint64_t len = 0;

    out[0] = '\0';
    if (path == nullptr) return false;

    const char* p = path;
    while (*p) {
        while (*p == '/') p++;              // collapse slashes
        if (*p == '\0') break;

        // component = [p, end)
        const char* end = p;
        while (*end && *end != '/') end++;
        uint64_t clen = (uint64_t)(end - p);

        if (clen == 1 && p[0] == '.') {
            // skip
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) { len = starts[--depth]; out[len] = '\0'; }
        } else {
            if (depth >= MAX_COMPONENTS) return false;
            // One slash, the component and the NUL must all fit. Refusing one
            // byte early would refuse a path a caller can represent.
            if (len + clen + 2 > out_max) return false;
            starts[depth++] = len;
            out[len++] = '/';
            for (uint64_t i = 0; i < clen; i++) out[len++] = p[i];
            out[len] = '\0';
        }
        p = end;
    }

    if (len == 0) { out[0] = '/'; out[1] = '\0'; }
    return true;
}

bool join(const char* dir, const char* name, char* out, uint64_t out_max) {
    if (out == nullptr || out_max == 0) return false;
    out[0] = '\0';
    if (dir == nullptr || name == nullptr) return false;

    uint64_t dl = str_len(dir);
    uint64_t nl = str_len(name);
    // The root is already a separator: "/" + "d" is "/d", not "//d". Any other
    // directory gets one slash between itself and the name.
    bool sep = !(dl > 0 && dir[dl - 1] == '/');
    if (dl + nl + (sep ? 1 : 0) + 1 > out_max) return false;   // both + NUL

    uint64_t i = 0;
    for (uint64_t j = 0; j < dl; j++) out[i++] = dir[j];
    if (sep) out[i++] = '/';
    for (uint64_t j = 0; j < nl; j++) out[i++] = name[j];
    out[i] = '\0';
    return true;
}

bool absolute(const char* path, const char* cwd, char* out, uint64_t out_max) {
    if (out == nullptr || out_max == 0) return false;
    out[0] = '\0';
    if (path == nullptr) return false;

    if (is_absolute(path)) {
        uint64_t n = str_len(path);
        if (n + 1 > out_max) return false;
        for (uint64_t i = 0; i <= n; i++) out[i] = path[i];
        return true;
    }
    return join(cwd != nullptr ? cwd : "/", path, out, out_max);
}

ComponentResult next_component(const char** cursor, char* out, uint64_t out_max) {
    const char* s = *cursor;
    while (*s == '/') s++;
    if (*s == '\0') { *cursor = s; return COMPONENT_END; }

    uint64_t i = 0;
    while (*s && *s != '/') {
        // The same ceiling as everywhere else: one byte for the NUL.
        if (i + 1 >= out_max) return COMPONENT_TOO_LONG;
        out[i++] = *s++;
    }
    out[i] = '\0';
    *cursor = s;
    return COMPONENT_OK;
}

SplitResult split(const char* rel, char* parent, uint64_t parent_max,
                  char* name, uint64_t name_max) {
    if (rel == nullptr || parent == nullptr || name == nullptr ||
        parent_max == 0 || name_max == 0) return SPLIT_EMPTY;

    uint64_t len = str_len(rel);
    while (len > 0 && rel[len - 1] == '/') len--;

    uint64_t slash = 0;                    // position just past the last '/'
    for (uint64_t i = 0; i < len; i++)
        if (rel[i] == '/') slash = i + 1;

    uint64_t n = 0;
    for (uint64_t i = slash; i < len; i++) {
        if (n + 1 >= name_max) return SPLIT_TOO_LONG;   // a name too long to be one
        name[n++] = rel[i];
    }
    name[n] = '\0';
    if (n == 0) return SPLIT_EMPTY;

    uint64_t plen = (slash > 0) ? slash - 1 : 0;   // drop the trailing '/'
    if (plen + 1 > parent_max) return SPLIT_TOO_LONG;
    for (uint64_t i = 0; i < plen; i++) parent[i] = rel[i];
    parent[plen] = '\0';
    return SPLIT_OK;
}

bool mount_covers(const char* mount, const char* canon) {
    if (mount == nullptr || canon == nullptr) return false;

    uint64_t ml = str_len(mount);
    if (ml == 0) return false;
    if (ml == 1 && mount[0] == '/') return canon[0] == '/';   // root

    uint64_t cl = str_len(canon);
    if (ml > cl) return false;
    for (uint64_t j = 0; j < ml; j++)
        if (canon[j] != mount[j]) return false;
    // The prefix must end at a boundary: "/bin" matches "/bin", "/bin/x",
    // but not "/binary".
    return canon[ml] == '\0' || canon[ml] == '/';
}

int longest_mount(const char* canon, const char* const* mount_paths, int count) {
    int best = -1;
    uint64_t best_len = 0;
    for (int i = 0; i < count; i++) {
        const char* mp = mount_paths[i];
        if (!mount_covers(mp, canon)) continue;
        uint64_t ml = str_len(mp);
        if (best < 0 || ml >= best_len) {
            best = i;
            best_len = ml;
        }
    }
    return best;
}

} // namespace FsPath
