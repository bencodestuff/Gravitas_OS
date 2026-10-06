# Third-party sources

These are fetched at configure time and are **not committed** (see `.gitignore`).
`configure.py` downloads, checksum-verifies and unpacks them, the same way it
does for the Limine bootloader.

## Lua

- Version: **5.4.9** (the last of the 5.4 line)
- Source: <https://www.lua.org/ftp/lua-5.4.9.tar.gz>
- sha256: `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`
- License: MIT (see `lua-5.4.9/README`)

It lands in `third_party/lua-5.4.9/`, is built into `bin/lua.elf` by
`configure.py` (33 C files: the core, the standard libraries, and the standalone
interpreter `lua.c`), and ships as `/bin/lua.elf`.

The build leaves the tree pristine and configures it entirely with `-D` flags
(no source patches):

| define | why |
|---|---|
| `LUA_PATH_DEFAULT` | Lua modules come from `/lua/` in the image |
| `LUA_CPATH_DEFAULT` | there are no C modules; kept harmless |

`LUA_USE_POSIX`, `LUA_USE_DLOPEN` and `LUA_USE_READLINE` are deliberately **not**
defined: we have no `popen`, no dynamic loader and no readline, so Lua builds in
its ANSI mode and `package.loadlib` reports that dynamic libraries are disabled.

To bump the version, change `LUA_VERSION`/`LUA_SHA256` in `configure.py`, delete
`third_party/lua-*/`, and re-run `configure.py`.
