#pragma once
#include <stdint.h>

// env — the environment every program starts with.
//
// The kernel reads it once at boot from /env ("NAME=VALUE" per line) and copies
// it onto each new task's stack, right after argv, where crt0 finds it (System V
// layout). It is a file rather than a kernel constant so that "what does a
// program start with" is something you can read and change — Lua's LUA_PATH is
// the first thing to use it.

namespace Env {
    // How many variables a task is handed (the same cap env.cpp parses to).
    constexpr int MAX_HANDOFF = 16;

    // Parse a block of "NAME=VALUE" lines. Blank lines and '#' comments are
    // skipped; a line without '=' is ignored.
    void set(const char* blob, uint64_t len);

    int count();
    const char* at(int i);   // the i-th "NAME=VALUE", or nullptr
}
