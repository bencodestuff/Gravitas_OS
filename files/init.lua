-- Gravitas boot manifest.
--
-- init.elf (C++) runs this file at boot; services start in the order they
-- appear here. A program is a name in /bin — write it with or without the
-- ".elf" ("term" and "term.elf" both work) — and any extra arguments become
-- that program's argv[1..].
--
-- This file IS the system's boot policy, and it is ordinary Lua: add
-- conditions, loops, your own programs. To add something at boot, append an
-- init.spawn(...) below. Because "/" is writable, a copy at /init.lua in the
-- overlay shadows this one, so you can change what starts without rebuilding
-- the image.
--
-- The API init.elf provides:
--   init.spawn(prog [, arg...]) -> pid | nil, err   start in the background
--   init.run(prog [, arg...])   -> code | nil, err  start and wait for it
--   init.wait(pid) -> code
--   init.kill(pid [, code]) -> true | false
--   init.sleep(ms)
--   init.cwd() -> string
--   init.log(msg)                a "[init] ..." line on the console

init.log("starting userland")

-- The console shell. It is the interactive login and must exist before the
-- window manager looks for it, so it goes first.
local shell = init.spawn("shell.elf")
if shell then
    init.log("shell as task " .. shell)
end

-- The window manager owns the display. If it will not start, stay
-- console-only: a windowed terminal needs a window manager to place it.
local wm = init.spawn("wm.elf")
if wm then
    init.log("window manager as task " .. wm)

    -- The terminal hosts a second shell in a window.
    local term = init.spawn("term.elf")
    if term then
        init.log("terminal as task " .. term)
    end
else
    init.log("no window manager; staying console-only")
end

-- Your programs go here, for example:
--   init.spawn("plasma.elf")
