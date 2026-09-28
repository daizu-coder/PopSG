/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <stdio.h> on this cegcc toolchain - like compat/errno.h,
 * this exists purely to intercept one libc entry point without touching
 * the untouchable core (pico/, cpu/, platform/libretro/libretro.c) or any
 * frontend call site directly. Unlike errno.h though, a working toolchain
 * <stdio.h> genuinely exists further down the include chain, so this uses
 * #include_next to pull it in unchanged and only overrides fopen().
 *
 * Round 54 added IsAsciiPathW()/RejectNonAsciiPathAndExit() to ce_main.c
 * because "the core opens ROM/BIOS files with a narrow fopen() that can't
 * reach a path holding characters outside 0x20-0x7E on this toolchain" -
 * true as far as it went, but the actual cause is narrower than "can't
 * reach a non-ASCII path" at all: the CRT's narrow fopen() converts its
 * char* argument to wchar_t* internally to call CreateFileW (the only file
 * API this OS has), and it does that using CP_ACP. This port already
 * builds every narrow path it hands the core (game.path/content_path in
 * LoadRomFlow, g_sysDirUtf8 via SetSysDirFromW, the persisted
 * "MegaCDBiosDir" config value) as CP_UTF8, not CP_ACP - so the round-trip
 * through the CRT's own fopen() was never going to work, the same
 * CP_ACP-vs-CP_UTF8 mismatch the sister PopGB port hit and fixed in
 * its own dev notes (session 13: a real device log caught a Japanese
 * folder name mangled to literal "???" by WideCharToMultiByte(CP_ACP,
 * ...), and CP_ACP on the way back into fopen() can't undo that damage -
 * "???" isn't the real folder name). Rejecting the pick up front instead
 * of hitting that mismatch (round 54's fix) sidesteps the corruption but
 * throws away the ability to use a Japanese folder at all.
 *
 * Fixing the round-trip instead: every narrow path in this codebase is
 * UTF-8 (see above), so redirecting fopen() to a wrapper (ce_fopen_utf8(),
 * ce_fopen_utf8.c) that decodes UTF-8 to wide with
 * MultiByteToWideChar(CP_UTF8, ...) and calls the real _wfopen() sidesteps
 * the CRT's lossy CP_ACP conversion entirely - for every fopen() call in
 * the program, core included, since -Icompat applies to the whole build
 * (see this port's own Makefile) and this header shadows the toolchain's
 * unconditionally. ASCII paths (the common case) are valid UTF-8
 * unchanged, so this is a strict improvement with no behavior change for
 * paths that already worked. ce_main.c's IsAsciiPathW()/
 * RejectNonAsciiPathAndExit() and their three call sites (ROM pick, Mega
 * CD BIOS pick, "MegaCDBiosDir" config read) are removed now that the
 * cause they were guarding against is actually fixed.
 */
#ifndef CE_COMPAT_STDIO_H
#define CE_COMPAT_STDIO_H

#include_next <stdio.h>

FILE *ce_fopen_utf8(const char *utf8path, const char *mode);
#define fopen ce_fopen_utf8

#endif
