/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for platform/libretro/libretro-common/memmap/memmap.c's
 * mmap()/munmap()/mprotect() POSIX shim.
 *
 * platform/libretro/libretro.c's plat_mmap()/plat_mremap()/plat_munmap()
 * (the generic, non-_3DS/VITA/PS3 branch) call real mmap()/munmap() and
 * check `errno` on failure; libretro-common/include/memmap.h declares
 * those under its "_WIN32" branch expecting libretro-common's own
 * memmap.c to provide them. That file's Windows implementation goes
 * through CreateFileMapping()/MapViewOfFile() via _get_osfhandle(), which
 * does not exist in this toolchain (Windows CE has no POSIX-style CRT
 * file-descriptor layer) - so it can't be used as-is.
 *
 * None of this is ever actually reached by this build: plat_mmap() and
 * friends exist only to hand memory to a dynamic recompiler's RWX code
 * cache (cpu/drc/cmn.c's plat_mem_set_exec()), and this port excludes
 * both of PicoDrive's true runtime recompilers - the SH2 compiler
 * (cpu/sh2/compiler.c, excluded via -DNO_32X) and the SVP compiler
 * (pico/carthw/svp/compiler.c, excluded via use_svpdrc=0) - and with them
 * cpu/drc/cmn.c itself, since nothing else calls it. (Cyclone and DrZ80,
 * the 68000/Z80 cores this port also doesn't use, are NOT dynamic
 * recompilers despite what an earlier version of this comment claimed -
 * they're static hand-written ARM asm and never touch this path; they're
 * unused simply because their calling convention hasn't been verified on
 * this toolchain, not for JIT-safety reasons - see CE/Makefile's
 * architecture notes.) This file exists purely so
 * platform/libretro/libretro.c links;
 * it implements the same signatures using WinCE's native VirtualAlloc()/
 * VirtualFree()/VirtualProtect() instead (which this port's other code
 * already depends on being present), ignoring the file-mapping arguments
 * since plat_mmap() only ever calls this with an anonymous mapping
 * (fildes = -1) in the first place.
 */
#include <windows.h>
#include "platform/libretro/libretro-common/include/memmap.h"

int errno;

void *mmap(void *addr, size_t len, int prot, int flags, int fildes, size_t offset)
{
	(void)addr;
	(void)prot;
	(void)flags;
	(void)fildes;
	(void)offset;
	void *ret = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (ret == NULL) {
		errno = 12; /* ENOMEM */
		return MAP_FAILED;
	}
	return ret;
}

int munmap(void *addr, size_t len)
{
	(void)len;
	if (!VirtualFree(addr, 0, MEM_RELEASE)) {
		errno = 22; /* EINVAL */
		return -1;
	}
	return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
	DWORD newProtect = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))
		? PAGE_EXECUTE_READWRITE : PAGE_NOACCESS;
	DWORD oldProtect = 0;
	if (!VirtualProtect(addr, len, newProtect, &oldProtect)) {
		errno = 22; /* EINVAL */
		return -1;
	}
	return 0;
}
