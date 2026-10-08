// init — the userland boot supervisor.
//
// The kernel starts exactly one userland program (this one) and then idles.
// Everything about "what runs at boot" lives here, in userland, which is the
// project's rule: the kernel owns mechanisms, userland owns policy. init's
// job is three small things:
//
//   1. Run the boot manifest — a Lua script, /init.lua by default. It decides
//      what starts, in what order, and with what arguments. It is an ordinary
//      file in the writable root, so editing it (or shadowing it in the
//      overlay) is how a user adds a program to boot. No kernel change, no
//      rebuild.
//   2. Give the manifest a small, documented API — the `init` table — over the
//      spawn/wait/kill/sleep syscalls. The mechanisms stay here in C++; the
//      policy is all in the script.
//   3. Recover if the manifest is missing or fails to load: fall back to a
//      built-in default so the machine always comes up.
//
// Lua is linked in rather than spawned: the manifest is a *script*, not a
// separate program, and there is no dlopen (and no shell to hand a command
// string to), so the interpreter has to be part of init. It is the same Lua
// 5.4 that ships as /bin/lua.elf.

#include <stdio.h>
#include <string.h>

#include <grv/syscall.h>

// The Lua headers are C, the C library they compile against is not.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// The manifest used when /init.lua cannot be read at all. It starts the same
// three services the shipped manifest does, so a broken manifest still leaves
// a usable machine (a console, a window manager and a terminal).
static const char kDefaultManifest[] =
    "init.log('no manifest; using the built-in default')\n"
    "local shell = init.spawn('shell.elf')\n"
    "if shell then init.log('shell as task ' .. shell) end\n"
    "local wm = init.spawn('wm.elf')\n"
    "if wm then\n"
    "  init.log('window manager as task ' .. wm)\n"
    "  local term = init.spawn('term.elf')\n"
    "  if term then init.log('terminal as task ' .. term) end\n"
    "else\n"
    "  init.log('no window manager; staying console-only')\n"
    "end\n";

// A bare program word ("term") means the program in /bin, exactly as the
// shell's `run` treats it: append ".elf". A name with a dot or a slash is a
// program already and is passed through untouched. The result is either the
// original pointer or the caller's scratch buffer.
static const char* resolve_prog(const char* name, char* buf, size_t n) {
    if (strchr(name, '/') || strchr(name, '.')) return name;
    snprintf(buf, n, "%s.elf", name);
    return buf;
}

// Collect the Lua call's arguments into the argv the spawn ABI takes: the
// program name plus up to seven more words (the kernel stages at most eight).
// Returns the count.
static int collect_argv(lua_State* L, const char** argv, int max) {
    int n = lua_gettop(L);
    if (n > max) n = max;
    for (int i = 1; i <= n; i++) argv[i - 1] = luaL_checkstring(L, i);
    return n;
}

// Push nil + a message: the idiomatic Lua way to report "I could not do it".
static int fail(lua_State* L, const char* what, const char* name) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: cannot start '%s'", what, name);
    return 2;
}

// init.spawn(prog [, arg...]) -> pid | nil, err
// Start a program in the background and return its task id. 'prog' is a name
// in /bin ("term.elf", or a path); the extra arguments become argv[1..].
static int l_spawn(lua_State* L) {
    const char* argv[8];
    int argc = collect_argv(L, argv, 8);
    if (argc < 1) {
        lua_pushnil(L);
        lua_pushliteral(L, "spawn: no program name");
        return 2;
    }
    char buf[64];
    argv[0] = resolve_prog(argv[0], buf, sizeof(buf));
    long pid = grv_spawn_bg(argv[0], argv, argc);
    if (pid < 0) return fail(L, "spawn", argv[0]);
    lua_pushinteger(L, pid);
    return 1;
}

// init.run(prog [, arg...]) -> exit code | nil, err
// Start a program and wait for it. Blocks, so it belongs in a manifest only
// for a service that must finish before the next one starts.
static int l_run(lua_State* L) {
    const char* argv[8];
    int argc = collect_argv(L, argv, 8);
    if (argc < 1) {
        lua_pushnil(L);
        lua_pushliteral(L, "run: no program name");
        return 2;
    }
    char buf[64];
    argv[0] = resolve_prog(argv[0], buf, sizeof(buf));
    long pid = grv_spawn(argv[0], argv, argc);
    if (pid < 0) return fail(L, "run", argv[0]);
    lua_pushinteger(L, grv_wait(pid));
    return 1;
}

// init.wait(pid) -> exit code
static int l_wait(lua_State* L) {
    lua_pushinteger(L, grv_wait(luaL_checkinteger(L, 1)));
    return 1;
}

// init.kill(pid [, code]) -> true | false
static int l_kill(lua_State* L) {
    long pid  = luaL_checkinteger(L, 1);
    long code = luaL_optinteger(L, 2, 0);
    lua_pushboolean(L, grv_kill(pid, code) == 0);
    return 1;
}

// init.sleep(ms)
static int l_sleep(lua_State* L) {
    grv_sleep((unsigned long)luaL_checkinteger(L, 1));
    return 0;
}

// init.cwd() -> the working directory as a string
static int l_cwd(lua_State* L) {
    char buf[64];
    grv_getcwd(buf, sizeof(buf));
    lua_pushstring(L, buf);
    return 1;
}

// init.log(msg) — one line on init's console, prefixed so boot output is easy
// to read (and to test).
static int l_log(lua_State* L) {
    printf("[init] %s\n", luaL_checkstring(L, 1));
    return 0;
}

static const luaL_Reg kInitLib[] = {
    { "log",   l_log },
    { "spawn", l_spawn },
    { "run",   l_run },
    { "wait",  l_wait },
    { "kill",  l_kill },
    { "sleep", l_sleep },
    { "cwd",   l_cwd },
    { nullptr, nullptr },
};

// Run a chunk held in a C string. Reports failure and returns false.
static bool run_string(lua_State* L, const char* chunk, const char* what) {
    if (luaL_loadstring(L, chunk) != LUA_OK) {
        printf("[init] %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        printf("[init] %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const char* manifest = (argc > 1 && argv[1] && argv[1][0]) ? argv[1]
                                                               : "/init.lua";

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        printf("[init] cannot create a Lua state; using the built-in default\n");
        // Without an interpreter there is still one sensible fallback: the
        // console shell, so the machine is not dead.
        const char* a[1] = { "shell.elf" };
        grv_spawn_bg(a[0], a, 1);
    } else {
        luaL_openlibs(L);
        luaL_newlib(L, kInitLib);
        lua_pushliteral(L, "0.1");
        lua_setfield(L, -2, "version");
        lua_setglobal(L, "init");

        printf("[init] loading %s\n", manifest);
        // Load and run separately: if the file exists but fails part-way
        // through, some services may already be running, and re-running the
        // default would start them twice. Only a manifest that cannot be read
        // at all falls back.
        if (luaL_loadfile(L, manifest) != LUA_OK) {
            printf("[init] cannot load %s: %s\n", manifest, lua_tostring(L, -1));
            lua_pop(L, 1);
            run_string(L, kDefaultManifest, "default manifest");
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[init] manifest error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    // Stay resident: init is the process that outlives the services it starts
    // (and the place a restart/monitor policy will live). Nothing to do yet,
    // so it sleeps — it blocks, so it never competes for the console.
    printf("[init] userland up\n");
    while (true) grv_sleep(60 * 1000);
}
