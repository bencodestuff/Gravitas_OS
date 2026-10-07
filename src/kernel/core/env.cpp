#include "kernel/core/env.h"

// See env.h. The storage is one static buffer and an array of pointers into it,
// so nothing is allocated and the pointers stay valid (the buffer is filled
// front to back, never reallocated).

namespace Env {

constexpr int MAX_VARS  = 16;
constexpr int MAX_BYTES = 1024;

static char storage[MAX_BYTES];
static const char* vars[MAX_VARS];
static int nvars = 0;

static bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

void set(const char* blob, uint64_t len) {
    nvars = 0;
    if (blob == nullptr || len == 0) return;

    uint64_t i = 0;
    uint64_t w = 0;
    while (i < len && nvars < MAX_VARS && w + 1 < MAX_BYTES) {
        // One line, without its newline.
        uint64_t start = i;
        while (i < len && blob[i] != '\n') i++;
        uint64_t end = i;
        if (i < len) i++;

        while (start < end && space(blob[start])) start++;
        while (end > start && space(blob[end - 1])) end--;
        if (start >= end || blob[start] == '#') continue;

        // NAME=VALUE, with a name of at least one character.
        uint64_t eq = start;
        while (eq < end && blob[eq] != '=') eq++;
        if (eq == start || eq >= end) continue;

        vars[nvars++] = &storage[w];
        for (uint64_t k = start; k < end && w + 1 < MAX_BYTES; k++)
            storage[w++] = blob[k];
        storage[w++] = '\0';
    }
}

int count() { return nvars; }

const char* at(int i) {
    return (i >= 0 && i < nvars) ? vars[i] : nullptr;
}

} // namespace Env
