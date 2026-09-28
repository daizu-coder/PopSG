/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <errno.h> on this cegcc toolchain.
 *
 * /opt/cegcc/arm-mingw32ce/include/errno.h does `#include_next <errno.h>`
 * to hand off to coredll's own errno.h, but there is no further directory
 * in the search chain that provides one, so the include_next fails ("no
 * include path in which to search for errno.h").
 *
 * This used to be an empty stub, because nothing in the codebase read or
 * wrote `errno` at the time (the sister CE ports' dev notes mention this).
 * platform/libretro/libretro.c's plat_mmap()/plat_mremap() do now (they
 * log the POSIX mmap()/mremap() shim's error via `errno` on failure) -
 * that whole code path is dead here (nothing in this build ever calls
 * plat_mmap; see CE/ce_mmap_stub.c), but it still needs to compile.
 * zlib does *not* need this: it's built with -DNO_ERRNO_H -D_WIN32_WCE=...
 * (see CE/Makefile), which makes it use its own internal z_errno instead
 * of touching this header at all (see zlib/zutil.h).
 *
 * Real storage for this `errno` lives in CE/ce_mmap_stub.c, next to the
 * mmap()/munmap()/mprotect() stand-ins that are its only actual writers.
 */
#ifndef CE_COMPAT_ERRNO_H
#define CE_COMPAT_ERRNO_H
extern int errno;
#endif
