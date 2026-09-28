/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN) - a directory-browsing
 * listbox dialog that replaces GetOpenFileNameW(). Ported from the
 * an earlier prototype's own IDD_FILEOPEN/DLGFileOpen (CE.c there): the
 * standard common file-open dialog has no way to render Japanese folder/
 * file names on this device (its own font is glyph-less for them,
 * confirmed by the earlier prototype's dev notes, and unfixable via OFN_* flags), so
 * the earlier prototype replaced it outright with a self-drawn listbox using the bundled
 * Japanese font (see ce_lang.h) instead.
 */
#ifndef CE_FILEOPEN_H
#define CE_FILEOPEN_H

#include <windows.h>

/* Loads the "Open Last Folder" preference from the config file - call
 * once from WinMain, after CeConfigLoad(). */
void CeFileOpenInit(void);

/* Which set of files the picker lists (see IsRomExtension /
 * FileMatchesPickMode in ce_fileopen.c):
 *  - CE_FILEOPEN_ROM : every supported ROM/CD extension EXCEPT .bin.
 *    A .bin Mega Drive ROM is loaded through its .cue instead, and a
 *    bare-.bin CD image isn't supported anyway (ce_main.c IsCdImagePath),
 *    so a loose .bin in a ROM folder is only ever a Mega CD BIOS - just
 *    clutter here. User request 2026-09-01.
 *  - CE_FILEOPEN_BIOS: *.bin only, for PromptAndSetupMegaCdBios's
 *    one-shot BIOS pick. */
enum {
    CE_FILEOPEN_ROM  = 0,
    CE_FILEOPEN_BIOS = 1
};

/* Shows the picker. mode is one of the CE_FILEOPEN_* values above.
 * Starting folder depends on CeFileOpenGetRememberLast() below: off (the
 * default) always starts at "\Storage Card" (falling back to "\" if that
 * doesn't exist), same as before; on, and a folder was remembered from a
 * previous successful pick, starts there instead (falling back the same
 * way if it no longer exists) - see Video Config's "Open Last Folder"
 * checkbox (ce_video.c), ported from the earlier prototype's Misc dialog. On a
 * successful pick, fills outPath with the chosen file's full path,
 * remembers its folder for next time, and returns 1; returns 0 if the
 * user backed out at the root without picking anything. */
int CeShowFileOpenDialog(HWND owner, wchar_t *outPath, size_t outPathCount, int mode);

/* Persisted "Open Last Folder" preference (ce_config.c, key
 * "VideoOpenLastFolder") - read/written by Video Config's checkbox;
 * CeShowFileOpenDialog() above just consumes the current value. Default
 * off, same as the earlier prototype. */
int  CeFileOpenGetRememberLast(void);
void CeFileOpenSetRememberLast(int enable);

#endif
