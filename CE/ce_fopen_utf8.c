/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * fopen() -> ce_fopen_utf8() redirect target - see compat/stdio.h for why
 * this exists (the CRT's narrow fopen() converts its char* path to
 * wchar_t* via CP_ACP internally, which mangles the CP_UTF8-encoded
 * narrow paths this port builds for the core; this wrapper decodes them
 * correctly instead and calls the real _wfopen()).
 *
 * mode is always a short plain-ASCII literal ("rb", "wb", "r", ...) from
 * every call site in this codebase (core included), so CP_ACP is fine for
 * that half - it only matters for the path half, which can hold a
 * Japanese folder/file name.
 *
 * Also the single place the core's fixed-name Mega CD BIOS fopen() calls
 * (platform/libretro/libretro.c find_bios(), via
 * RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) get redirected to whatever BIOS
 * file ce_main.c's ResolveMegaCdBiosDir()/InstallMegaCdBiosFromPicked()
 * actually found by content - see CeMegaCdBiosRedirect()'s own comment in
 * ce_main.c for why the name doesn't have to match.
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int CeMegaCdBiosRedirect(const wchar_t *requestedPath, wchar_t *out, size_t count);

FILE *ce_fopen_utf8(const char *utf8path, const char *mode)
{
    wchar_t wpath[MAX_PATH];
    wchar_t wmode[8];
    wchar_t redirected[MAX_PATH];

    MultiByteToWideChar(CP_UTF8, 0, utf8path, -1, wpath, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 8);

    if (CeMegaCdBiosRedirect(wpath, redirected, MAX_PATH))
        return _wfopen(redirected, wmode);

    return _wfopen(wpath, wmode);
}
