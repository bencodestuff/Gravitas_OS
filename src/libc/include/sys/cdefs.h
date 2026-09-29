#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H 1

#define __gravitas_libc 1

/* C/C++ linkage boundary: C sources must export unmangled symbols */
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

/* Marks a symbol as belonging to the libc itself (vs the user program) */
#define __libc_decl __BEGIN_DECLS

#endif
