/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Windows CE frontend for PicoDrive (SHARP Brain PW-G5300, ARM).
 *
 * This is a *libretro frontend*, not a port that reaches into the
 * PicoDrive core directly: ../platform/libretro/libretro.c already
 * implements every port hook the core needs in terms of the five
 * retro_set_* callbacks below (same "core is not patched" architecture
 * the sister PopSNES port uses - see the dev notes). All we do here
 * is drive retro_init/retro_load_game/retro_run and turn those callbacks
 * into real GDI/waveOut/key I/O.
 *
 * UI shape, window/shutdown handling, ROM/SRAM/save-state file I/O and
 * the touch-to-reveal menu dialog are all ported near-verbatim from the
 * sister PopSNES port's own ce_main.c, which is the more recently
 * hardware-validated of the two prior CE ports on this exact device (see
 * that project's dev notes for the iteration history that shaped this
 * shape) - ce_display.c/ce_audio.c/ce_config.c/ce_lang.c/ce_fileopen.c are
 * likewise carried over with only the PicoDrive-specific bits (core
 * option keys, ROM extensions, joypad button set) adjusted; see each
 * file's own header comment for what changed.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <ctype.h>

#include <libretro.h>

#include "ce_log.h"
#include "ce_display.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_fileopen.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

/* pico/memory.c (declared in pico/pico_int.h, which this file doesn't
 * otherwise need) - see LoadRomFlow() for why it's called there. */
void io_ports_reset(void);

static const wchar_t kWndClassName[] = L"PopSGWnd";
static const wchar_t kMutexName[]    = L"PopSG_SingleInstance";
static const wchar_t kAppTitle[]     = L"PopSG";

static HWND   g_hwnd     = NULL;
static HANDLE g_mutex    = NULL;
static volatile int g_running = 0;

static wchar_t g_romPath[MAX_PATH] = L""; /* last successfully-loaded ROM's path, for save-state/SRAM file naming */

/* UTF-8 directory handed to the core for RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
 * i.e. where PicoDrive's find_bios() (platform/libretro/libretro.c) looks for
 * the Mega CD BIOS. Empty until ResolveMegaCdBiosDir() locates one - the
 * folder the user picked the BIOS from, or the exe folder. Can hold a
 * Japanese folder name like any other narrow path this port builds: fopen()
 * itself is redirected (compat/stdio.h/ce_fopen_utf8.c) to decode CP_UTF8
 * correctly instead of the CRT's own lossy CP_ACP, so the core's narrow
 * fopen() calls reach it fine. The picked folder's path is persisted
 * verbatim in the config key "MegaCDBiosDir"; there is no copy into a
 * private data dir. */
static char g_sysDirUtf8[MAX_PATH * 3] = "";

/* The actual Mega CD BIOS file ProbeMegaCdBiosFileW() validated (by
 * content, not name) and FindBestMegaCdBiosInDirW()/InstallMegaCdBiosFromPicked()
 * picked, plus its region bits (1=Japan, 4=US, 8=Europe - same encoding
 * platform/libretro/libretro.c's find_bios() takes as its region argument).
 * ce_fopen_utf8.c's CeMegaCdBiosRedirect() uses these to transparently hand
 * the core this file whenever it fopen()s one of the fixed names in
 * kMcdBiosNames[] for a matching region - no rename/copy on disk, the
 * redirect only exists in memory for the life of the process. Empty/0
 * until a BIOS is found. */
static wchar_t g_mcdBiosActualPathW[MAX_PATH] = L"";
static int     g_mcdBiosActualRegion = 0;

static int  PromptAndSetupMegaCdBios(HWND hwnd);    /* defined after CeConfirm, used by LoadRomFlow */
static int  CeSaveState(void);                      /* defined below; used by SaveStateDlgProc above it */

/* retro_get_system_av_info().timing.fps for the loaded ROM (NTSC
 * ~59.92, PAL ~49.70). The main-loop frame limiter's GetTickCount
 * fallback paces to this; 0 until the first ROM loads. */
static double g_coreFps = 0.0;

/* Frame limiter high-water mark: hold a frame back while the audio ring
 * has more than this many ms of playback queued ahead of real time.
 * Enough cushion above the fixed waveOut pipeline (~90ms) to ride out
 * this device's bursty retro_run() timing without letting a light core
 * free-run. Starting value - wants a real-hardware latency/underrun
 * tune (see the dev notes). */
#define CE_FRAME_PACING_HIGH_MS 64

/* g_romLoaded: a game has been successfully retro_load_game()'d at
 * least once (stays true across File>Open reloads until exit).
 * g_paused: the touch-to-reveal menu is up right now - retro_run() is
 * not called while this is true, and CeDisplaySuspend() has released the
 * cached window DC so the menu dialog and WM_PAINT own the screen. */
static int  g_romLoaded = 0;
static int  g_paused    = 0;

static void CeShutdown(int exitCode); /* used by MainMenuDlgProc, below */
static void CeShowShellChrome(HWND hwnd); /* used by CeShutdown, defined further below */
static void CeHideShellChrome(HWND hwnd); /* used by ShowMainMenuDialog and WinMain, defined further below */

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* Set by the core via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * whenever Video Config's Frame Skip is above 0 (ce_video.c) - NULL
 * otherwise (Frame Skip Off, or before retro_load_game()'s first
 * check_variables() call). Invoked once per frame from WinMain's main
 * loop, right before retro_run(), per that environment call's contract -
 * see the call site below. */
static retro_audio_buffer_status_callback_t g_audioBuffStatusCb = NULL;

static bool ce_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
        /* ce_display.c's off-screen DIB section is RGB565 (BI_BITFIELDS
         * 5-6-5); refuse anything else so the core doesn't silently
         * assume a format we can't display. PicoDrive's retro_load_game()
         * hard-requires this to succeed (bails out with "RGB565 support
         * required" otherwise), so this must return true for RGB565. */
        return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Video Config's sprite-limit/frame-skip settings (ce_video.c)
         * ride the core's own existing core-options protocol
         * (picodrive_sprlim / picodrive_frameskip* - see
         * check_variables() in libretro/libretro.c) instead of a new
         * side channel - CE just needs to answer these two queries. */
        struct retro_variable *var = (struct retro_variable *)data;
        return CeVideoEnvGetVariable(var->key, &var->value) ? true : false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Lets a Video Config change made mid-session (ROM already
         * loaded, retro_load_game() - which would otherwise be the only
         * point check_variables() re-reads these - not called again)
         * take effect on the very next retro_run() instead of needing a
         * File>Open reload. */
        *(bool *)data = CeVideoConsumeDirty() ? true : false;
        return true;

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        /* The core only asks for this when frame skip is "auto" (see
         * check_variables()/init_frameskip() in libretro/libretro.c) -
         * without answering it, that mode silently never skips anything
         * (retro_audio_buff_active stays false forever). data is NULL
         * when the core wants to unregister. */
        const struct retro_audio_buffer_status_callback *cb =
            (const struct retro_audio_buffer_status_callback *)data;
        g_audioBuffStatusCb = cb ? cb->callback : NULL;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        /* Only answered once a Mega CD BIOS has been located (see
         * ResolveMegaCdBiosDir()); until then we return false so the core keeps
         * its built-in default, exactly like every other optional call
         * below. find_bios() in platform/libretro/libretro.c builds
         * "<this dir>\<known bios name>.bin" from what we return here. */
        if (g_sysDirUtf8[0] == '\0')
            return false;
        *(const char **)data = g_sysDirUtf8;
        return true;

    default:
        /* Everything else (GET_LOG_INTERFACE handled separately by the
         * core itself calling environ_cb before this is even wired up,
         * GET_SYSTEM_DIRECTORY for BIOS lookup, SET_CORE_OPTIONS*,
         * GET_INPUT_BITMASKS, ...) is optional per the libretro API
         * contract - returning false tells the core to use its built-in
         * defaults, which is exactly what we want until there is a
         * settings UI for each of those too. */
        return false;
    }
}

/* Screenshot guard: set by ce_video_refresh once the current ROM has
 * actually drawn a frame, cleared on every ROM load. ce_display.c's DIB
 * isn't cleared on ROM switch, so without this a freshly loaded game
 * could save the previous game's image. */
static int g_frameDrawnSinceLoad = 0;

static void ce_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    /* Keeps Video Config's Frame Skip consecutive-skip counter (see
     * ce_video.h) in sync with what the core actually did this frame:
     * data is NULL exactly when the core skipped rendering. */
    CeVideoFrameSkipNotifyRendered(data != NULL);

    if (!data)
        return; /* duplicate/skipped frame - nothing new to draw */

    g_frameDrawnSinceLoad = 1; /* Screenshot: the DIB now holds this ROM's image */

    /* Self-contained per call (width/height/pitch given fresh every
     * time, and always RGB565 per ce_environment()'s SET_PIXEL_FORMAT
     * handling above). width/height vary at runtime - MD H32 256 /
     * H40 320 / SMS 256 / GG 160 - and ce_display.c reacts to that. */
    CeDisplayBlitRGB565(data, width, height, (unsigned)pitch);
}

static void ce_audio_sample_noop(int16_t left, int16_t right)
{
    /* The core only ever uses retro_set_audio_sample_batch (see
     * retro_set_audio_sample() in libretro.c - it's an intentional
     * no-op setter), but we still register a real callback rather than
     * NULL to avoid relying on that being true forever. */
    (void)left;
    (void)right;
}

/* Per-frame timing for the debug log (same pattern as ce_display.c's
 * "perf: blit" logging - see CeDisplayBlitRGB565()): platform/libretro/
 * libretro.c calls audio_batch_cb() exactly once per retro_run(), so
 * this is directly comparable, per-frame, to retro_run's own timing
 * below and blit's. Lets retro_run_avg - blit_avg - audio_push_avg
 * stand in for "core CPU/PPU/sound-chip emulation alone", without
 * touching platform/libretro/libretro.c or anything under pico/. */
static size_t ce_audio_sample_batch(const int16_t *data, size_t frames)
{
    static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
    DWORD t0 = GetTickCount();
    size_t ret = CeAudioPushSamples(data, frames);
    unsigned elapsed = (unsigned)(GetTickCount() - t0);

    s_accumMs += elapsed;
    if (elapsed > s_maxMs)
        s_maxMs = elapsed;
    if (++s_count >= 60)
    {
        CeLog("perf: audio_push avg=%ums max=%ums over %u frames",
              s_accumMs / s_count, s_maxMs, s_count);
        s_accumMs = 0;
        s_maxMs = 0;
        s_count = 0;
    }
    return ret;
}

static void ce_input_poll(void)
{
    CeInputPoll();
}

static int16_t ce_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return CeInputState(port, device, index, id);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

/* PicoDrive's retro_get_system_info() sets need_fullpath = true (see
 * libretro/libretro.c), i.e. the core wants to open/read the ROM itself
 * (through PicoLoadMedia(), which also handles .zip via unzip/unzip.c -
 * linked into CE/Makefile's SOURCES_CORE) rather than have the frontend
 * preload the whole file into memory first - unlike the sister
 * PopSNES port's own PickAndLoadRom(), which does preload (that
 * core does not set need_fullpath). This is simpler and, for the larger
 * ROMs this console uses (up to 4MB, vs. SNES's typical <=4MB too, but
 * PicoDrive additionally has to cope with much larger Sega CD images),
 * avoids an extra malloc()+fread() of the entire file for no benefit. */
static int PickRom(HWND owner, wchar_t *outPath, size_t outPathCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    memset(outPath, 0, outPathCount * sizeof(wchar_t));

    /* Custom listbox-based picker (ce_fileopen.c), not GetOpenFileNameW()
     * - the standard common dialog has no way to render Japanese folder/
     * file names on this device (see ce_fileopen.c's header comment). */
    if (!CeShowFileOpenDialog(owner, outPath, outPathCount, CE_FILEOPEN_ROM))
    {
        CeLog("PickRom: file picker cancelled");
        return 0;
    }

    hFind = FindFirstFileW(outPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        CeLog("PickRom: selected file no longer exists");
        return 0;
    }
    FindClose(hFind);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Battery-backed cartridge save (SRAM)                                */
/* ------------------------------------------------------------------ */

/* Loads "<romPath>.srm" into the core's SRAM, if this game has any
 * (RETRO_MEMORY_SAVE_RAM) and a save file already exists. This is what
 * makes a game's own in-cartridge save feature survive across app
 * restarts, same as a real battery-backed cartridge would - ported from
 * the sister PopSNES port's own CeLoadSram/CeSaveSram, which added
 * this after a real power-off test showed relying only on graceful
 * app-exit/ROM-switch checkpoints lost saves (see that project's
 * dev notes round 8/9). No .srm file yet is the normal case for a new
 * game (or one with no SRAM at all) and isn't logged as an error. */
static void CeLoadSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t got;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"rb");
    if (!f)
    {
        CeLog("CeLoadSram: no .srm file yet (new game, or none saved)");
        return;
    }

    /* Read at most `size` bytes - a mismatched-size .srm (shouldn't
     * happen for a given ROM, but don't overrun the core's buffer if it
     * somehow does) is truncated, not rejected outright. */
    got = fread(sram, 1, size, f);
    fclose(f);
    CeLog("CeLoadSram: loaded %lu of %lu bytes", (unsigned long)got, (unsigned long)size);
}

/* Writes the core's current SRAM out to "<romPath>.srm" - the other half
 * of CeLoadSram(). Called whenever a loaded game's SRAM is about to stop
 * being the live one (File>Open loading a different ROM, or app exit),
 * plus a pause-time checkpoint (ShowMainMenuDialog) and a periodic
 * autosave (WinMain's loop), same three checkpoints the sister
 * PopSNES port settled on. */
static void CeSaveSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM - nothing to save */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"wb");
    if (!f)
    {
        CeLog("CeSaveSram: failed to open .srm file for write");
        return;
    }

    fwrite(sram, 1, size, f);
    fclose(f);
    CeLog("CeSaveSram: saved %lu bytes", (unsigned long)size);
}

/* ------------------------------------------------------------------ */
/* Mega CD BIOS                                                        */
/* ------------------------------------------------------------------ */

/* True for the CD-image extensions PicoDrive treats as Mega CD content
 * (.cue/.iso/.chd). ".bin" is deliberately left out: a bare .bin is far
 * more often a plain cartridge dump, and a real Mega CD rip always ships
 * its .cue alongside. Used only to decide whether a load failure is
 * worth offering the BIOS picker for. */
static int IsCdImagePath(const wchar_t *path)
{
    const wchar_t *ext = wcsrchr(path, L'.');

    if (!ext)
        return 0;
    return (_wcsicmp(ext, L".cue") == 0 ||
            _wcsicmp(ext, L".iso") == 0 ||
            _wcsicmp(ext, L".chd") == 0);
}

/* Every BIOS file name find_bios() (platform/libretro/libretro.c) will
 * try, across all three regions, paired with the region bits (1=Japan,
 * 4=US, 8=Europe - find_bios()'s own region argument encoding) that name
 * is only ever requested under. CeMegaCdBiosRedirect() uses the pairing
 * to refuse a redirect when the BIOS content actually found is the wrong
 * region for the name the core asked for (e.g. don't hand a Japanese BIOS
 * to a US-region fopen() call just because it's the only one around). */
static const struct { const wchar_t *name; int region; } kMcdBiosNames[] = {
    { L"bios_CD_J.bin",      1 }, { L"bios_CD_U.bin",       4 }, { L"bios_CD_E.bin",      8 },
    { L"jp_mcd2_921222.bin", 1 }, { L"jp_mcd1_9112.bin",    1 }, { L"jp_mcd1_9111.bin",   1 },
    { L"us_scd2_9306.bin",   4 }, { L"SegaCDBIOS9303.bin",  4 }, { L"us_scd1_9210.bin",   4 },
    { L"eu_mcd2_9306.bin",   8 }, { L"eu_mcd2_9303.bin",    8 }, { L"eu_mcd1_9210.bin",   8 }
};
#define MCD_BIOS_NAME_COUNT ((int)(sizeof(kMcdBiosNames) / sizeof(kMcdBiosNames[0])))

/* The exe's own directory (module path minus the trailing "\AppMain.exe"). */
static void GetExeDirW(wchar_t *out, size_t count)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    if (slash)
        *slash = L'\0';
    wcsncpy(out, path, count - 1);
    out[count - 1] = L'\0';
}

/* file's directory - everything up to, but not including, the last
 * backslash. "\a\b\c.bin" -> "\a\b". */
static void PathDirOnlyW(const wchar_t *file, wchar_t *out, size_t count)
{
    wchar_t *slash;

    wcsncpy(out, file, count - 1);
    out[count - 1] = L'\0';
    slash = wcsrchr(out, L'\\');
    if (slash)
        *slash = L'\0';
}

/* Does path look like a Mega CD BIOS dump, judged purely by content (the
 * file name is never consulted)? A BIOS dump shares the fixed Genesis/Mega
 * Drive cartridge header this port's own core already parses for region
 * detection (pico/pico.c PicoDetectRegion()):
 *   - 0x100: 16-byte console name starting "SEGA" (shared by MD carts,
 *     32X and the BIOS itself - the first, cheap filter);
 *   - 0x1f0: 4-byte region code, made of 'J'/'U'/'E' letters (same
 *     encoding PicoDetectRegion() decodes) - absence of all three means
 *     this isn't a Sega header at all;
 *   - 0x110: 16-byte copyright/date string, e.g. "(C)SEGA 1993.DEC" -
 *     best-effort parsed into a YYYYMM sort key so several candidates in
 *     one folder can be ranked by how new they are.
 * Returns the OR of region bits found (1/4/8, matching find_bios()'s
 * region argument), or 0 if the file isn't Mega-CD-BIOS-shaped (wrong
 * size, no "SEGA" signature, or no region letters). *outDateKey is set to
 * the parsed date (0 if unparseable - sorts as oldest). */
static int ProbeMegaCdBiosFileW(const wchar_t *path, long *outDateKey)
{
    static const char * const kMonths[12] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    FILE *f;
    long sz;
    unsigned char hdr[0x200];
    size_t n;
    int region = 0;
    int i;

    *outDateKey = 0;

    f = _wfopen(path, L"rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    /* A Mega CD BIOS dump is 128 KB; accept a little slack, but reject an
     * obvious mistake like a .cue (a few hundred bytes) or a whole disc
     * image. */
    if (sz < 0x8000 || sz > 0x80000)
    {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);
    n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < sizeof(hdr))
        return 0;

    if (memcmp(hdr + 0x100, "SEGA", 4) != 0)
        return 0;

    for (i = 0; i < 4; i++)
    {
        unsigned char c = hdr[0x1f0 + i];
        if (c == 'J' || c == 'j') region |= 1;
        else if (c == 'U' || c == 'u') region |= 4;
        else if (c == 'E' || c == 'e') region |= 8;
    }
    if (region == 0)
        return 0;

    for (i = 0; i <= (int)sizeof(hdr) - 0x110 - 9 && i < 8; i++)
    {
        const char *s = (const char *)hdr + 0x110 + i;
        if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
            isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
            s[4] == '.')
        {
            int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 +
                       (s[2] - '0') * 10 + (s[3] - '0');
            int m;

            for (m = 0; m < 12; m++)
            {
                if (_strnicmp(s + 5, kMonths[m], 3) == 0)
                {
                    *outDateKey = year * 100 + (m + 1);
                    break;
                }
            }
            break;
        }
    }

    return region;
}

/* Scans dir (its direct contents only - no recursion into subfolders) for
 * the Mega CD BIOS best suited to the current UI language, judged purely
 * by file content via ProbeMegaCdBiosFileW() (names are never checked).
 * When several files qualify, picks by region first - Japanese UI prefers
 * J, then E, then U; English UI prefers E, then U, then J - and, among
 * same-priority candidates, the newest copyright date. Returns 1 and
 * fills outPath/outRegion if a candidate was found, 0 otherwise. */
static int FindBestMegaCdBiosInDirW(const wchar_t *dir, wchar_t *outPath,
    size_t outCount, int *outRegion)
{
    static const int kOrderJp[3] = { 1, 8, 4 }; /* J, E, U */
    static const int kOrderEn[3] = { 8, 4, 1 }; /* E, U, J */
    const int *order = CeLangIsJapanese() ? kOrderJp : kOrderEn;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];
    wchar_t bestPath[MAX_PATH] = L"";
    int bestRegion = 0;
    int bestRank = 99;
    long bestDateKey = -1;

    _snwprintf(pattern, MAX_PATH, L"%s\\*", dir);
    pattern[MAX_PATH - 1] = L'\0';

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do
    {
        wchar_t full[MAX_PATH];
        long dateKey;
        int region, rank, i;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        _snwprintf(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH - 1] = L'\0';

        region = ProbeMegaCdBiosFileW(full, &dateKey);
        if (region == 0)
            continue;

        rank = 99;
        for (i = 0; i < 3; i++)
        {
            if (region & order[i]) { rank = i; break; }
        }
        if (rank == 99)
            continue; /* can't happen - ProbeMegaCdBiosFileW() only returns J/U/E bits */

        if (rank < bestRank || (rank == bestRank && dateKey > bestDateKey))
        {
            bestRank = rank;
            bestDateKey = dateKey;
            bestRegion = region;
            wcsncpy(bestPath, full, MAX_PATH - 1);
            bestPath[MAX_PATH - 1] = L'\0';
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    if (bestPath[0] == L'\0')
        return 0;

    wcsncpy(outPath, bestPath, outCount - 1);
    outPath[outCount - 1] = L'\0';
    *outRegion = bestRegion;
    return 1;
}

/* Called from ce_fopen_utf8.c for every fopen() the core makes (compat/
 * stdio.h redirects fopen -> ce_fopen_utf8 for every source file,
 * including the core itself). If the requested path's file name is one of
 * kMcdBiosNames[] *and* that name's region matches the BIOS content
 * ResolveMegaCdBiosDir()/InstallMegaCdBiosFromPicked() actually found,
 * points the real fopen() at the found file instead - no rename/copy on
 * disk, this redirect exists only in memory for the life of the process.
 * The region check matters when the folder holds a BIOS for a different
 * region than the one requested: without it a Japanese BIOS could get
 * handed to a US-region fopen() call just because it's the only file
 * around, silently mixing regions. Returns 1 and fills out if redirected,
 * 0 to let the caller's own path through unchanged. */
int CeMegaCdBiosRedirect(const wchar_t *requestedPath, wchar_t *out, size_t count)
{
    const wchar_t *slash;
    const wchar_t *name;
    int i;

    if (g_mcdBiosActualPathW[0] == L'\0')
        return 0;

    slash = wcsrchr(requestedPath, L'\\');
    name = slash ? slash + 1 : requestedPath;

    for (i = 0; i < MCD_BIOS_NAME_COUNT; i++)
    {
        if (_wcsicmp(name, kMcdBiosNames[i].name) == 0)
        {
            if (!(kMcdBiosNames[i].region & g_mcdBiosActualRegion))
                return 0; /* wrong region for this name - don't redirect */
            wcsncpy(out, g_mcdBiosActualPathW, count - 1);
            out[count - 1] = L'\0';
            return 1;
        }
    }
    return 0;
}

static void SetSysDirFromW(const wchar_t *dir)
{
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, g_sysDirUtf8, sizeof(g_sysDirUtf8), NULL, NULL);
    CeLog("MegaCD BIOS: system dir configured");
}

/* Point g_sysDirUtf8/g_mcdBiosActualPathW at a Mega CD BIOS found purely
 * by content (file name doesn't matter - ProbeMegaCdBiosFileW()), without
 * storing an absolute path that would break if the app is moved:
 *   (a) the folder remembered from a previous BIOS pick (config key
 *       "MegaCDBiosDir") still holds one -> use it;
 *   (b) otherwise, a BIOS sitting next to the exe (that folder only - no
 *       recursion into subfolders).
 * Either can be a Japanese folder name - see g_sysDirUtf8's comment above.
 * Returns 1 if a BIOS is now configured, 0 if none was found (caller then
 * falls back to the first-run picker). No copy into a private data dir -
 * the user keeps the BIOS where they put it, and CeMegaCdBiosRedirect()
 * (ce_fopen_utf8.c) makes the core see it under whichever fixed name it
 * asks for. */
static int ResolveMegaCdBiosDir(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t found[MAX_PATH];
    char dirUtf8[MAX_PATH * 3];
    int region;

    dirUtf8[0] = '\0';
    CeConfigGetString("MegaCDBiosDir", dirUtf8, sizeof(dirUtf8));
    if (dirUtf8[0])
    {
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8, -1, dir, MAX_PATH);
        dir[MAX_PATH - 1] = L'\0';
        if (FindBestMegaCdBiosInDirW(dir, found, MAX_PATH, &region))
        {
            wcsncpy(g_mcdBiosActualPathW, found, MAX_PATH - 1);
            g_mcdBiosActualPathW[MAX_PATH - 1] = L'\0';
            g_mcdBiosActualRegion = region;
            SetSysDirFromW(dir);
            return 1;
        }
    }

    GetExeDirW(dir, MAX_PATH);
    if (FindBestMegaCdBiosInDirW(dir, found, MAX_PATH, &region))
    {
        wcsncpy(g_mcdBiosActualPathW, found, MAX_PATH - 1);
        g_mcdBiosActualPathW[MAX_PATH - 1] = L'\0';
        g_mcdBiosActualRegion = region;
        SetSysDirFromW(dir);
        return 1;
    }

    return 0;
}

/* First-run picker result: validate the picked file is a Mega CD BIOS by
 * content (ProbeMegaCdBiosFileW() - its name is never checked), then
 * point the core's system dir at the folder it sits in, remember that
 * file (g_mcdBiosActualPathW/g_mcdBiosActualRegion, for
 * CeMegaCdBiosRedirect()) and that folder (config key "MegaCDBiosDir").
 * No copy/rename on disk - the user keeps the BIOS exactly where they put
 * it, under whatever name it already has. Returns 1 on success, 0 if the
 * file doesn't look like a Mega CD BIOS. */
static int InstallMegaCdBiosFromPicked(const wchar_t *biosPath)
{
    wchar_t dir[MAX_PATH];
    char dirUtf8[MAX_PATH * 3];
    long dateKey;
    int region;

    region = ProbeMegaCdBiosFileW(biosPath, &dateKey);
    if (region == 0)
    {
        CeLog("MegaCD BIOS: picked file doesn't look like a Mega CD BIOS");
        return 0;
    }

    PathDirOnlyW(biosPath, dir, MAX_PATH);

    wcsncpy(g_mcdBiosActualPathW, biosPath, MAX_PATH - 1);
    g_mcdBiosActualPathW[MAX_PATH - 1] = L'\0';
    g_mcdBiosActualRegion = region;

    SetSysDirFromW(dir);

    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirUtf8, sizeof(dirUtf8), NULL, NULL);
    CeConfigSetString("MegaCDBiosDir", dirUtf8);
    return 1;
}

/* Picks a ROM (via PickRom) and hands its path to the core. Safe to call
 * both for the first load and for File>Open while a game is already
 * running (unloads the previous game first). Returns 1 on success, 0 if
 * the user cancelled the picker or loading failed - in both failure
 * cases whatever was running before is left untouched. */
static int LoadRomFlow(HWND hwnd)
{
    wchar_t romPath[MAX_PATH];
    struct retro_game_info game;
    struct retro_system_av_info avInfo;
    static char pathUtf8[MAX_PATH * 3]; /* worst case UTF-8 expansion */
    wchar_t title[MAX_PATH + 32];
    wchar_t *base;

    if (!PickRom(hwnd, romPath, MAX_PATH))
        return 0; /* cancelled/failed - PickRom already logged why */

    if (g_romLoaded)
    {
        CeSaveSram(); /* g_romPath/the core's SRAM still refer to the *previous* game here - new one isn't loaded yet */
        retro_unload_game();
    }

    WideCharToMultiByte(CP_UTF8, 0, romPath, -1, pathUtf8, sizeof(pathUtf8), NULL, NULL);
    memset(&game, 0, sizeof(game));
    game.path = pathUtf8;
    game.data = NULL; /* PicoLoadMedia() reads the file itself via game.path - see PickRom's comment */
    game.size = 0;

    g_frameDrawnSinceLoad = 0;
    if (!retro_load_game(&game))
    {
        /* A Mega CD image fails here with the core's "Missing BIOS" when
         * find_bios() found nothing. Try once more to locate a BIOS dir
         * (covers a BIOS dropped in since startup), then fall back to the
         * first-run picker (like the PCE-CD / GBA cores); retry once if
         * either wires up a system directory. */
        int retried = 0;

        if (IsCdImagePath(romPath) &&
            (ResolveMegaCdBiosDir() || PromptAndSetupMegaCdBios(hwnd)))
            retried = retro_load_game(&game);

        if (!retried)
        {
            CeLog("LoadRomFlow: retro_load_game failed");
            MessageBoxW(hwnd, L"Failed to load ROM.", kAppTitle, MB_OK);
            g_romLoaded = 0;
            return 0;
        }
    }

    /* Re-sync the core's pad-port timing state to the freshly reset
     * 68k cycle counter. PicoPower() (inside retro_load_game()) resets
     * Pico.t.m68c_cnt to 0 but not pico/memory.c's padTHLatency/
     * padTLLatency/padTHTimeout, which still hold the previous game's
     * (much larger) cycle counts after a mid-game ROM switch. Until the
     * new game's counter caught up with them, port_read() inverted TL
     * and held TH low, so the new game saw a pad button held from its
     * very first read (on-device debug log: 0xA10003 read 0xa3 instead
     * of 0xff after playing another game first) - SGDK titles such as
     * Mai Nurse skipped straight past their title screen. io_ports_reset()
     * is the core's own reset for exactly that state (pico/state.c
     * calls it when a save state carries none); on a first load it
     * changes nothing (those values are already 0). Called here, after
     * both the first try and the Mega CD retry above, and before the
     * first retro_run(). Only the Mega Drive / Mega CD pad ports read
     * these values - harmless for Master System, Game Gear and Pico. */
    io_ports_reset();

    g_romLoaded = 1;
    wcsncpy(g_romPath, romPath, MAX_PATH - 1);
    g_romPath[MAX_PATH - 1] = L'\0';

    CeLoadSram();

    retro_get_system_av_info(&avInfo);
    CeLog("LoadRomFlow: loaded, geometry=%ux%u fps=%.3f sample_rate=%.0f",
          avInfo.geometry.base_width, avInfo.geometry.base_height,
          avInfo.timing.fps, avInfo.timing.sample_rate);
    g_coreFps = avInfo.timing.fps; /* main-loop frame limiter's fallback target */
    CeAudioStart(avInfo.timing.sample_rate);

    base = wcsrchr(romPath, L'\\');
    _snwprintf(title, MAX_PATH + 32, L"PopSG - %s", base ? base + 1 : romPath);
    SetWindowTextW(g_hwnd, title); /* always the main window, even when
                                     * called with a dialog as `hwnd` */

    return 1;
}

/* ------------------------------------------------------------------ */
/* Generic message box (IDD_MSGBOX)                                    */
/* ------------------------------------------------------------------ */

/* MessageBoxW() draws with whatever system font Windows CE finds, and
 * this device has no CJK-capable one any more (jptahoma.ttc dropped for
 * licensing reasons - see ce_lang.h) - Japanese text through it comes
 * back as tofu. This dialog instead paints its own text with the
 * Shinonome bitmap font (ce_bmpfont.c), the same way every other piece
 * of Japanese UI text in this port already does. Use it for any result
 * message that can carry Japanese text (Save/Load State below); a
 * message that's always English-only (ROM load failures, fatal startup
 * errors) can stay a plain MessageBoxW(). */
#define CE_MSGBOX_MAX_TEXT  256
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

#define CE_MSGBOX_MAX_LINE  64
#define CE_MSGBOX_MAX_LINES 4

/* Word-wraps text into up to maxLines lines of at most maxWidth real
 * pixels each, filling lines[i][0..CE_MSGBOX_MAX_LINE-1]. Greedy: each
 * line takes as many characters as fit, breaking at the last ASCII space
 * on the line when there is one (so English breaks between words;
 * Japanese has no spaces and just hard-breaks). At least one character
 * always goes on a line even if it alone overflows. Any remainder past
 * the final line is dropped. Returns the number of lines used (>=1). */
static int WrapMsgBoxLines(const wchar_t *text, int maxWidth,
                           wchar_t lines[][CE_MSGBOX_MAX_LINE], int maxLines)
{
    int count = 0;
    const wchar_t *p = text;

    while (*p && count < maxLines)
    {
        int fit = 0, lastSpace = -1, i;
        wchar_t probe[CE_MSGBOX_MAX_LINE];

        for (i = 0; p[i] && i < CE_MSGBOX_MAX_LINE - 1; i++)
        {
            probe[i] = p[i];
            probe[i + 1] = L'\0';
            if (CeBmpFontGetTextWidth(probe) > maxWidth)
                break;
            fit = i + 1;
            if (p[i] == L' ')
                lastSpace = i + 1;
        }

        if (p[fit] == L'\0')            /* rest fits on this line */
            ;
        else if (lastSpace > 0)         /* break after the last space */
            fit = lastSpace;
        else if (fit == 0)              /* one over-wide char - force it */
            fit = 1;

        wcsncpy(lines[count], p, fit);
        lines[count][fit] = L'\0';
        count++;

        p += fit;
        while (*p == L' ')              /* swallow the wrapped space */
            p++;
    }

    if (count == 0)
    {
        lines[0][0] = L'\0';
        count = 1;
    }
    return count;
}

/* Draws up to `count` pre-wrapped lines centered both ways in rc. */
static void PaintMsgBoxLines(HDC hdc, const RECT *rc,
                             wchar_t lines[][CE_MSGBOX_MAX_LINE], int count)
{
    COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    int lineH  = CE_BMPFONT_HEIGHT + 2;
    int rectW  = rc->right - rc->left;
    int rectH  = rc->bottom - rc->top;
    int blockH = count * CE_BMPFONT_HEIGHT + (count - 1) * (lineH - CE_BMPFONT_HEIGHT);
    int y = rc->top + (rectH - blockH) / 2;
    int i;

    if (y < rc->top)   /* a block taller than rc grows downward, not up under the title bar */
        y = rc->top;

    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < count; i++)
    {
        int x = rc->left + (rectW - CeBmpFontGetTextWidth(lines[i])) / 2;
        CeBmpFontDrawTextW(hdc, x, y, lines[i], fg);
        y += lineH;
    }
}

static WNDPROC s_pMsgBoxOkOrigProc = NULL;

/* Same DLGC_WANTALLKEYS/VK_RETURN/VK_SPACE/VK_ESCAPE subclass pattern as
 * every other BS_OWNERDRAW OK button in this port (MainMenuBtnCtrlProc
 * below, SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc) - BS_OWNERDRAW
 * breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter routing and
 * Escape-to-Cancel handling alike, so both have to be reimplemented by
 * hand here too. */
static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        /* IDC_MB_TEXT stays a plain hidden LTEXT, same as IDC_MM_HINT
         * elsewhere in this port - repainted by WM_PAINT below instead
         * of drawn by the control itself. */
        ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
        s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
        return TRUE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
        int count;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_MB_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

        count = WrapMsgBoxLines(s_msgBoxText, rc.right - rc.left, lines, CE_MSGBOX_MAX_LINES);
        PaintMsgBoxLines(hdc, &rc, lines, count);

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
               MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Yes/No confirmation (IDD_CONFIRM) - user request: Save State asks     */
/* first. Copied verbatim from the PopGB port's ConfirmDlgProc/     */
/* ConfirmBtnCtrlProc/CeConfirm. Shares WrapMsgBoxLines()/s_msgBoxText  */
/* with CeShowMsgBox above (the two are never on screen at the same     */
/* time), and the same BS_OWNERDRAW-button key handling; just two       */
/* buttons instead of one. */
/* ------------------------------------------------------------------ */

static WNDPROC s_pConfirmBtnOrigProc = NULL;

static LRESULT CALLBACK ConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CF_NO, 0), (LPARAM)hWnd);
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
            return 0;
        }
    }

    return CallWindowProc(s_pConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK ConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
        SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
        int count;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_CF_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

        count = WrapMsgBoxLines(s_msgBoxText, rc.right - rc.left, lines, CE_MSGBOX_MAX_LINES);
        PaintMsgBoxLines(hdc, &rc, lines, count);

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CF_YES:
            EndDialog(hDlg, 1);
            return TRUE;
        case IDC_CF_NO:
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Returns 1 for Yes, 0 for No/Back. */
static int CeConfirm(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    return (int)DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
                           MAKEINTRESOURCEW(IDD_CONFIRM), owner, ConfirmDlgProc) == 1;
}

/* ------------------------------------------------------------------ */
/* Save State: ask + run + acknowledge, all in one self-drawn dialog  */
/* ------------------------------------------------------------------ */

/* The old Save State path opened IDD_CONFIRM, closed it, then opened
 * IDD_MSGBOX - two modal dialogs back-to-back under the main menu. On
 * this device's window manager that hand-off exposes the main menu for a
 * frame and its owner-draw "ステートセーブ" button repaints to the front.
 * This reuses the IDD_CONFIRM template but never closes/reopens: on Yes
 * it runs the save in place and morphs its own text/buttons into the
 * result. IDD_CONFIRM stays generic for PromptAndSetupMegaCdBios. */
static WNDPROC s_pSsBtnOrigProc = NULL;
static int     s_ssPhase        = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK SsCfBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int  id   = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;
        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (s_ssPhase == 0) /* the result phase has only the OK button */
                SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
            return 0;
        }
    }
    return CallWindowProc(s_pSsBtnOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK SaveStateDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_ssPhase = 0;
        ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
        SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pSsBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)SsCfBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)SsCfBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
        int count;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_CF_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

        count = WrapMsgBoxLines(s_msgBoxText, rc.right - rc.left, lines, CE_MSGBOX_MAX_LINES);
        PaintMsgBoxLines(hdc, &rc, lines, count);

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CF_YES:
            if (s_ssPhase == 0)
            {
                int ok = CeSaveState();
                wcsncpy(s_msgBoxText, CeLangIsJapanese()
                        ? (ok ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002"                     /* セーブしました。 */
                              : L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002")  /* セーブに失敗しました。 */
                        : (ok ? L"State saved." : L"Save failed."),
                        CE_MSGBOX_MAX_TEXT - 1);
                s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
                s_ssPhase = 1;
                ShowWindow(GetDlgItem(hDlg, IDC_CF_NO), SW_HIDE);
                SetDlgItemTextW(hDlg, IDC_CF_YES, L"OK");
                {   /* recentre the lone OK button */
                    RECT rc, rb;
                    GetClientRect(hDlg, &rc);
                    GetWindowRect(GetDlgItem(hDlg, IDC_CF_YES), &rb);
                    MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
                    SetWindowPos(GetDlgItem(hDlg, IDC_CF_YES), NULL,
                                 (rc.right - (rb.right - rb.left)) / 2, rb.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                InvalidateRect(hDlg, NULL, TRUE);
                SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
            }
            else
            {
                EndDialog(hDlg, 1);
            }
            return TRUE;
        case IDC_CF_NO:
        case IDCANCEL:
            EndDialog(hDlg, s_ssPhase ? 1 : 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static void CeConfirmAndSaveState(HWND owner)
{
    wcsncpy(s_msgBoxText, CeLangIsJapanese()
            ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
            : L"Save state?",
            CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
               MAKEINTRESOURCEW(IDD_CONFIRM), owner, SaveStateDlgProc);
}

/* Asks whether to pick a Mega CD BIOS, runs the file picker, points the
 * core's system dir at the folder the picked BIOS lives in and persists
 * that folder (InstallMegaCdBiosFromPicked). Returns 1 only when a usable
 * BIOS is now configured, so the caller can retry the load. */
static int PromptAndSetupMegaCdBios(HWND hwnd)
{
    wchar_t biosPath[MAX_PATH];

    if (!CeConfirm(hwnd, CeLangIsJapanese()
            ? L"\x30e1\x30ac" L"CD" L"\x306e" L"BIOS" L"\x304c\x5fc5\x8981\x3067\x3059\x3002\x9078\x629e\x3057\x307e\x3059\x304b\xff1f"
              /* メガCDのBIOSが必要です。選択しますか？ */
            : L"Mega CD BIOS required. Select the BIOS file now?"))
        return 0;

    if (!CeShowFileOpenDialog(hwnd, biosPath, MAX_PATH, CE_FILEOPEN_BIOS))
        return 0;

    if (!InstallMegaCdBiosFromPicked(biosPath))
    {
        MessageBoxW(hwnd, L"That file does not look like a Mega CD BIOS.",
                    kAppTitle, MB_OK);
        return 0;
    }

    CeConfigSave(); /* InstallMegaCdBiosFromPicked wrote "MegaCDBiosDir" into the table */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Save state (single slot per ROM: "<romPath>.state")                */
/* ------------------------------------------------------------------ */

/* Returns 1 on success, 0 on failure. Shows no UI itself - the caller
 * (SaveStateDlgProc) folds the result into its own single dialog so no
 * second modal is ever nested. */
static int CeSaveState(void)
{
    size_t size;
    void *buffer;
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;

    size = retro_serialize_size();
    if (size == 0)
    {
        CeLog("CeSaveState: retro_serialize_size returned 0");
        return 0;
    }

    /* calloc, not malloc: retro_serialize_size() is a worst case (it
     * counts the 32X chunks for MD/MCD and the FM unit for SMS), the core
     * writes only the chunks the game really has, and the whole buffer
     * goes to the file. pico/state.c reads past the real chunks too, so
     * the tail must be zeros (chunk 0, length 0 - skipped) rather than
     * leftover heap bytes it could take for a chunk header. */
    buffer = calloc(1, size);
    if (!buffer)
    {
        CeLog("CeSaveState: calloc(%lu) failed", (unsigned long)size);
        return 0;
    }

    if (!retro_serialize(buffer, size))
    {
        free(buffer);
        CeLog("CeSaveState: retro_serialize failed");
        return 0;
    }

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"wb");
    if (!f)
    {
        free(buffer);
        CeLog("CeSaveState: failed to open state file for write");
        return 0;
    }

    fwrite(buffer, 1, size, f);
    fclose(f);
    free(buffer);
    CeLog("CeSaveState: saved %lu bytes", (unsigned long)size);
    return 1;
}

static int CeLoadState(HWND owner)
{
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;
    long size;
    void *buffer;

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"rb");
    if (!f)
    {
        CeLog("CeLoadState: no state file found");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093\x3002" /* セーブデータが見つかりません。 */
                                                : L"No save state found.");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(f);
        CeLog("CeLoadState: empty/unreadable state file");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    buffer = malloc((size_t)size);
    if (!buffer)
    {
        fclose(f);
        CeLog("CeLoadState: malloc(%ld) failed", size);
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(buffer);
        CeLog("CeLoadState: short read");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }
    fclose(f);

    if (!retro_unserialize(buffer, (size_t)size))
    {
        free(buffer);
        CeLog("CeLoadState: retro_unserialize failed");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f(\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x306e\x5f62\x5f0f\x304c\x7570\x306a\x308b\x53ef\x80fd\x6027\x304c\x3042\x308a\x307e\x3059)\x3002" /* ロードに失敗しました(セーブデータの形式が異なる可能性があります)。 */
                                                : L"Load failed (incompatible save?).");
        return 0;
    }

    free(buffer);
    CeLog("CeLoadState: loaded %ld bytes", size);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Menu (touch-to-reveal - hidden during gameplay, shown at startup     */
/* before any ROM is loaded and whenever the screen is tapped mid-game) */
/*                                                                      */
/* Implemented as a modal DialogBoxW (CE/ce_res.rc's IDD_MAINMENU) with */
/* plain PUSHBUTTON controls, not a real HMENU - see ce_resource.h for  */
/* why (this coredll doesn't export SetMenu).                          */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInstance = NULL;

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the native pixel format of both sources below, so no conversion and no
 * compression: one fwrite per row). Uses the on-screen image at the
 * current Scale (x1/x1.5/Wide/Full, from ce_display.c's DIB) so the file
 * matches what the user sees. Written to "<exe-dir>\Screenshots\<ROM name>_NNN.bmp",
 * creating the folder on first use and taking the first unused number.
 * The header is built byte by byte rather than from BITMAPFILEHEADER so
 * struct packing can't shift it. Returns 1 on success. */
static int CeSaveScreenshot(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t romName[MAX_PATH];
    wchar_t path[MAX_PATH + 16];
    wchar_t *p;
    const wchar_t *base;
    unsigned n, y, rowBytes, padBytes;
    unsigned long imageBytes;
    unsigned char hdr[66];
    static const unsigned char pad[4] = { 0, 0, 0, 0 };
    const void *img;
    unsigned imgW, imgH, imgPitch;
    FILE *f;

    /* Only the display's DIB is used (no fallback to the core's own
     * frame buffer as PopSNES has: PicoDrive's libretro.c may realloc
     * vout_buf on H32/H40 changes, and the DIB always exists once a
     * frame was drawn anyway). */
    if (!g_frameDrawnSinceLoad
        || !CeDisplayGetLastImage(&img, &imgW, &imgH, &imgPitch))
    {
        CeLog("CeSaveScreenshot: no rendered frame yet");
        return 0;
    }

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 0;
    p = wcsrchr(dir, L'\\');
    if (!p)
        return 0;
    p[1] = L'\0';
    if (wcslen(dir) + 12 >= MAX_PATH)
        return 0;
    wcscat(dir, L"Screenshots");
    CreateDirectoryW(dir, NULL); /* already existing is fine */
    if (GetFileAttributesW(dir) == 0xFFFFFFFF)
    {
        CeLog("CeSaveScreenshot: can't create Screenshots folder");
        return 0;
    }

    base = wcsrchr(g_romPath, L'\\');
    wcsncpy(romName, base ? base + 1 : g_romPath, MAX_PATH - 1);
    romName[MAX_PATH - 1] = L'\0';
    p = wcsrchr(romName, L'.');
    if (p)
        *p = L'\0';

    for (n = 1; n <= 999; n++)
    {
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
        path[MAX_PATH + 15] = L'\0';
        if (GetFileAttributesW(path) == 0xFFFFFFFF)
            break;
    }
    if (n > 999)
    {
        CeLog("CeSaveScreenshot: all 999 numbers used");
        return 0;
    }

    rowBytes = imgW * 2;
    padBytes = (4 - (rowBytes & 3)) & 3;
    imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
    PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
    PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
    PutLE32(hdr + 18, imgW);
    PutLE32(hdr + 22, imgH);             /* positive = bottom-up */
    PutLE16(hdr + 26, 1);                        /* planes */
    PutLE16(hdr + 28, 16);                       /* bits per pixel */
    PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
    PutLE32(hdr + 34, imageBytes);
    PutLE32(hdr + 38, 2835);                     /* 72 dpi */
    PutLE32(hdr + 42, 2835);
    PutLE32(hdr + 54, 0xF800);                   /* R mask */
    PutLE32(hdr + 58, 0x07E0);                   /* G mask */
    PutLE32(hdr + 62, 0x001F);                   /* B mask */

    f = _wfopen(path, L"wb");
    if (!f)
    {
        CeLog("CeSaveScreenshot: can't open output file");
        return 0;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = imgH; y-- > 0; )
    {
        fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
        if (padBytes)
            fwrite(pad, 1, padBytes, f);
    }
    if (ferror(f))
    {
        fclose(f);
        DeleteFileW(path);
        CeLog("CeSaveScreenshot: write failed");
        return 0;
    }
    fclose(f);

    CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
    return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
    if (CeSaveScreenshot())
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
            : L"Screenshot saved.");
    else
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
            : L"Screenshot failed.");
}

/* Swaps every IDD_MAINMENU button caption between English (the .rc
 * template's own text) and Japanese, gated by CeLangIsJapanese() - see
 * ce_lang.h. Called once from WM_INITDIALOG, and again right after the
 * IDC_MM_VIDEO case returns (Video Config is where the toggle itself
 * lives), so flipping it and returning to this still-open dialog updates
 * it immediately instead of only the next time the menu happens to be
 * recreated. The title bar is deliberately never touched - it's
 * non-client area the OS paints with its own caption font, unreachable
 * via WM_SETFONT (confirmed to render as tofu boxes on this device by
 * an earlier prototype). The PUSHBUTTONs above are BS_OWNERDRAW
 * (ce_res.rc) and repaint themselves from the text just set (WM_DRAWITEM
 * below, via CeBmpFontDrawOwnerButtonTheme() with this dialog's own
 * pastel colors - see kMainMenuTheme below and ce_bmpfont.c). IDC_MM_HINT
 * is a plain LTEXT - STATIC controls have no ownerdraw style - so it's
 * hidden here instead and repainted by this dialog's own WM_PAINT via
 * CeBmpFontPaintLabel(), which still reads the text set above with
 * GetWindowTextW() even though the control itself is hidden.
 *
 * CeBmpFontPaintLabel() only SetPixel()s the "on" bits of each glyph
 * with a transparent background - it never clears the label's rect
 * first, so a stale glyph from the previous language can survive
 * underneath the new one unless the whole dialog gets a full
 * erase+redraw first. Forcing that unconditionally here (every time
 * this text can change) avoids depending on whatever incidental repaint
 * a sub-dialog closing over part of this one happens to trigger. */
static void ApplyMainMenuLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                             /* ROMを開く... */
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");            /* ステートセーブ */
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");            /* ステートロード */
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                        /* ボタン設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");                  /* サウンド設定 */
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                              /* 画面設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b");                /* 画面保存する */
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                          /* 終了 */
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
    }

    ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);
    InvalidateRect(hDlg, NULL, TRUE);
}

static const int kMainMenuButtonIds[] = {
    IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
    IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while g_romLoaded is still 0) -
 * EnableWindow() alone only blocks activation, not this dialog's own
 * custom arrow-key cycling below. Bounded to CE_MAINMENU_BUTTON_COUNT
 * steps so it can't spin forever if every button were ever disabled at
 * once (never happens in practice - Open/Exit are always enabled). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
    int idx, step;
    for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
        if (kMainMenuButtonIds[idx] == id)
            break;
    if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
        return id;

    for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
    {
        int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
        int nextId = kMainMenuButtonIds[nextIdx];
        if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
            return nextId;
    }
    return id;
}

/* Pastel/rounded main-menu skin (from the menu redesign notes - a
 * mockup screenshot showing SD
 * mascot art next to a cream-background, rounded-button menu). Applies
 * only to this dialog's own buttons via WM_DRAWITEM below and to its
 * own client background via WM_ERASEBKGND - every other dialog in this
 * port keeps the plain gray CeBmpFontDrawOwnerButton() look untouched.
 * Text color is a dark charcoal rather than pure black to match the
 * mascot artwork's own outline color (popsg_mascot.bmp, IDB_MAINMENU
 * below).
 *
 * Button *fills* went pastel again in a third pass (further user
 * feedback after an on-device photo of the deepened-color cut: "the
 * colors should be pastel-toned" - but "leave the window background
 * color as is", so CE_MENU_BG_CREAM below is intentionally still the
 * deepened ivory from that same prior cut, not the design notes'
 * original pale cream). Borders are kept at that prior cut's deeper,
 * more saturated tone rather than reverting those too - a soft pastel
 * fill with a crisp dark outline is what the very first mockup image
 * this skin was built from already showed, and it also keeps each
 * button's icon (drawn with the border color, see
 * CeBmpFontDrawOwnerButtonTheme() in ce_bmpfont.c) legible against the
 * lighter fill. The corner-radius/focus-fill changes from that same
 * feedback round live in CeBmpFontDrawOwnerButtonTheme() itself.
 *
 * Save/Load State's fill went back to that prior cut's deeper amber
 * in a later round - on-device it read too pale/washed out next to the
 * other five, still-pastel buttons (an on-device photo alongside them
 * made the difference obvious in a way the earlier per-button mockups
 * hadn't) - then split into two distinct colors (yellow/blue, user
 * request) in the round after that, replacing the one shared amber
 * pair both buttons had used since the very first cut of this skin -
 * then Save State's yellow was tuned a shade greener/brighter into
 * "lemon" (also user request), then pulled back to a pastel version of
 * that same lemon two rounds later - unlike every other button, this
 * one's fill went pastel->deep amber->bold yellow->bold lemon->pastel
 * lemon across five separate feedback rounds.
 *
 * Open ROM and Load State swapped their bg/border pairs whole (mint<->
 * blue, user request) in a later round still - Open ROM is blue now,
 * Load State mint; every other button (including Save State's own
 * lemon, above) kept its own color through that swap. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
    { IDC_MM_OPEN,      RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,  0 }, /* blue   */
    { IDC_MM_SAVESTATE, RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,  0 }, /* pastel lemon */
    { IDC_MM_LOADSTATE, RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,  0 }, /* mint   */
    { IDC_MM_VIDEO,     RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO, 1 }, /* green */
    { IDC_MM_SOUND,     RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND, 1 }, /* blue  */
    { IDC_MM_INPUT,     RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT, 1 }, /* pink  */
    { IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender (same as PopSNES) */
    { IDC_MM_EXIT,      RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,  0 }, /* coral */
};

static WNDPROC s_pMainMenuOrigProc = NULL;

/* CE/icon/popsg_mascot.bmp, embedded as IDB_MAINMENU (ce_res.rc). Loaded
 * lazily in WM_INITDIALOG, blitted by WM_PAINT into the blank strip
 * right of the IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;

/* PUSHBUTTON's built-in WM_KEYDOWN -> BN_CLICKED conversion for
 * VK_RETURN/VK_SPACE (and IsDialogMessage()'s own Up/Down/Left/Right
 * focus-cycling) stops working once these buttons become BS_OWNERDRAW
 * (ce_res.rc): touch/stylus taps keep working (WM_LBUTTONUP is
 * unaffected) but the physical decide key does nothing on a focused
 * button unless a control claims DLGC_WANTARROWS | DLGC_WANTALLKEYS,
 * the same claim SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc already
 * make for their own "-/value/+" buttons - so arrow-key navigation is
 * reimplemented here too via MainMenuNeighbor() above instead of
 * leaning on the standard dialog navigation that claim steals control
 * of. Claiming DLGC_WANTALLKEYS also steals the physical Back key
 * (Escape) away from IsDialogMessage()'s normal Cancel-key handling, so
 * it's forwarded to IDCANCEL by hand below, same as those three
 * dialogs already do for their own OK button. */
static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
            return 0;

        case VK_DOWN:
        case VK_RIGHT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
            return 0;

        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
        ApplyMainMenuLanguage(hDlg);

        s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
        for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

        if (!s_hMainMenuBmp)
            s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        unsigned i;
        for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
        {
            if (kMainMenuTheme[i].id == (int)dis->CtlID)
            {
                CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
                                                kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
                return TRUE;
            }
        }
        CeBmpFontDrawOwnerButton(dis); /* fallback, shouldn't hit any control here */
        return TRUE;
    }

    /* Cream client background for the pastel menu skin (see
     * kMainMenuTheme above) - painted here instead of a class background
     * brush so it's scoped to just this dialog. Created once and kept
     * for the process lifetime rather than per-message (this fires on
     * every erase, e.g. each time a sub-dialog closes over part of this
     * one) - deliberately never DeleteObject()'d, same tradeoff this
     * port already makes for other process-lifetime GDI objects. */
    case WM_ERASEBKGND:
    {
        static HBRUSH s_hCreamBrush = NULL;
        RECT rc;
        if (!s_hCreamBrush)
            s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, s_hCreamBrush);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hMainMenuBmp)
        {
            DeleteObject(s_hMainMenuBmp);
            s_hMainMenuBmp = NULL;
        }
        return FALSE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

        /* popsg_mascot.bmp (IDB_MAINMENU) in the blank strip to the right of
         * the "戻るキーでゲーム再開" hint. Measured off the hint
         * control's own rect (same NULL->hDlg mapping CeBmpFontPaintLabel
         * uses) plus the Shinonome text width, so it starts just past the
         * text and runs to the dialog's right/bottom edge with the source
         * aspect ratio preserved, bottom-right aligned. */
        if (s_hMainMenuBmp)
        {
            HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
            RECT rcHint, rcClient;
            wchar_t hintText[128];
            BITMAP bm;

            hintText[0] = 0;
            GetWindowTextW(hHint, hintText, 128);
            GetWindowRect(hHint, &rcHint);
            MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
            GetClientRect(hDlg, &rcClient);

            if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
                long boxR = rcClient.right - 4;
                long boxT = rcHint.top;
                long boxB = rcClient.bottom - 2;
                long boxW = boxR - boxL;
                long boxH = boxB - boxT;

                if (boxW > 8 && boxH > 8)
                {
                    long drawW = boxW;
                    long drawH = drawW * bm.bmHeight / bm.bmWidth;
                    HDC memDC;
                    HGDIOBJ oldBmp;

                    if (drawH > boxH)
                    {
                        drawH = boxH;
                        drawW = drawH * bm.bmWidth / bm.bmHeight;
                    }

                    /* No SetStretchBltMode() - this coredll doesn't export
                     * it; CE's default (COLORONCOLOR) already drops whole
                     * rows/cols on shrink, which is what this decorative
                     * blit wants anyway. */
                    memDC = CreateCompatibleDC(hdc);
                    oldBmp = SelectObject(memDC, s_hMainMenuBmp);
                    StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
                               (int)drawW, (int)drawH,
                               memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteDC(memDC);
                }
            }
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MM_OPEN:
            /* Stays open on cancel/failure (LoadRomFlow already showed
             * a MessageBox explaining why); closes only on success, so
             * the caller knows to resume gameplay. */
            if (LoadRomFlow(hDlg))
                EndDialog(hDlg, IDC_MM_OPEN);
            return TRUE;

        case IDC_MM_EXIT:
            CeShutdown(0); /* never returns */
            return TRUE;

        case IDC_MM_SAVESTATE:
            if (g_romLoaded)
                CeConfirmAndSaveState(hDlg);
            return TRUE; /* stays open either way, like Input/Sound Config */

        case IDC_MM_LOADSTATE:
            /* Closes and resumes on success (like Resume Game) so the
             * user immediately sees the loaded state; stays open on
             * failure (CeLoadState already showed why). */
            if (g_romLoaded && CeLoadState(hDlg))
                EndDialog(hDlg, IDC_MM_LOADSTATE);
            return TRUE;

        case IDC_MM_SCREENSHOT:
            if (g_romLoaded)
                CeScreenshotAndReport(hDlg);
            return TRUE; /* stays open, like Save State */

        case IDC_MM_INPUT:
            CeShowInputConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_SOUND:
            CeShowSoundConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_VIDEO:
            CeShowVideoConfigDialog(hDlg);
            /* Video Config is where the Japanese/English toggle lives -
             * re-apply here so switching it and returning to this
             * still-open menu updates it immediately. */
            ApplyMainMenuLanguage(hDlg);
            return TRUE;

        case IDCANCEL:
            /* Hardware Back / OS close gesture: same as Resume if a
             * game is already running (nothing to lose by dismissing),
             * otherwise ignored - there's nothing to go back to yet. */
            if (g_romLoaded)
                EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Pauses (if a game is running), shows the menu dialog modally (blocks
 * until closed), then resumes if a game is loaded when it returns -
 * whether that's the game that was already running, or one just picked
 * via Open ROM. Also how the very first "no ROM loaded" screen is shown
 * from WinMain, where wasPlaying is simply false. */
static void ShowMainMenuDialog(HWND hwnd)
{
    int wasPlaying = g_romLoaded && !g_paused;

    if (wasPlaying)
    {
        g_paused = 1;
        CeAudioSetPaused(1);
        CeDisplaySuspend(); /* releases the cached window DC; no full teardown - see ce_display.h */

        /* Autosave SRAM at every pause, not just at graceful shutdown/
         * ROM switch - a real power-off doesn't run CeShutdown() at all,
         * so relying only on those two checkpoints misses that case
         * entirely (lesson from the sister PopSNES port's round 9).
         * Opening the touch-to-reveal menu is a frequent, cheap, natural
         * checkpoint to also save at. */
        CeSaveSram();
    }

    /* Only forces a repaint here when there's no ROM loaded yet (the
     * initial "No ROM loaded" screen, which does need its black
     * background painted once). While a game is paused for the menu,
     * skip this - WM_PAINT below repaints the last frame from
     * ce_display.c's off-screen DIB on top of its black fill (see that
     * handler), so the menu still opens over the paused game. */
    if (!g_romLoaded)
        InvalidateRect(hwnd, NULL, TRUE);
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MAINMENU), hwnd, MainMenuDlgProc);

    if (g_romLoaded)
    {
        g_paused = 0;
        CeAudioSetPaused(0);
        if (!CeDisplayInit(hwnd))
            CeLog("ShowMainMenuDialog: CeDisplayInit failed - continuing without video output");
        CeDisplayResume(); /* no-op under GDI - see ce_display.h */

        /* Re-assert taskbar hiding every time gameplay is (re-)entered,
         * not just once at WinMain startup - the sister PopSNES
         * port found this necessary on this device (see that project's
         * ce_main.c comment): a custom DialogBoxW becoming the
         * foreground window (this menu) may cause the shell to restore
         * the taskbar, so it needs re-hiding every time control returns
         * from the menu dialog to gameplay. */
        CeHideShellChrome(hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Window / shutdown                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        /* Touch-to-reveal, same interaction both prior CE ports use.
         * ShowMainMenuDialog() is modal, so this can't re-enter while
         * already showing (input goes to the dialog, not this window). */
        ShowMainMenuDialog(hwnd);
        return 0;

    case WM_PAINT:
    {
        /* GDI video output (ce_display.c) draws straight into this
         * window's own client area, so - unlike the old GAPI backend,
         * which bypassed window painting entirely while it owned the
         * display - a WM_PAINT here (a modal dialog covering part of the
         * game window, then closing) needs the last rendered frame
         * redrawn, or the exposed region stays black until the next real
         * emulated frame (which won't happen at all while retro_run() is
         * paused for that same dialog). Always erase the invalidated
         * region to black first (covers both the very first "No ROM
         * loaded" screen and any letterbox border around the game rect),
         * then, if a ROM is loaded, redraw the current frame on top via
         * CeDisplayForceRepaint() - it re-blits from the off-screen DIB
         * rather than the core's own (possibly stale by now) frame
         * pointer. */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        if (g_romLoaded)
            CeDisplayForceRepaint();
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void CeShutdown(int exitCode)
{
    /* Lesson from both prior CE ports on this device (see either
     * project's dev notes - snes9x2002's is the more thoroughly
     * documented of the two): a plain `return` from WinMain lets the
     * normal exit path run, which on this device/toolchain combination
     * can itself crash or hang threads mid-teardown. Always terminate
     * via ExitProcess, called directly from here (not via WM_CLOSE/
     * WM_DESTROY/PostQuitMessage, which an earlier prototype traced a real
     * hang to on this device). CeAudioStop() joins the audio thread
     * cleanly before we ever get here, so ExitProcess() isn't tearing
     * down a thread still mid-waveOutWrite. */
    CeAudioStop();
    if (g_romLoaded)
        CeSaveSram();
    retro_unload_game();
    retro_deinit();

    CeDisplayShutdown();
    CeShowShellChrome(g_hwnd);

    if (g_mutex)
        CloseHandle(g_mutex);

    /* Last, after every other CeLog() call above - drains the
     * background writer thread's buffer (ce_log.c) so nothing logged
     * during this teardown is lost before ExitProcess(). No-op if
     * logging was never enabled. */
    CeLogShutdown();

    ExitProcess((UINT)exitCode);
}

/* "HHTaskBar" is the standard window class of the Windows CE Explorer
 * taskbar on this device - FindWindow+ShowWindow(HIDE) on it is what
 * both prior CE ports on this hardware settled on after aygshell.dll's
 * SHFullScreen proved unreliable/fragile (see either project's
 * dev notes). Needs nothing beyond coredll.dll. */
static HWND CeFindTaskBarWindow(void)
{
    return FindWindowW(L"HHTaskBar", NULL);
}

static void CeHideShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
    {
        ShowWindow(hTaskBar, SW_HIDE);
        CeLog("CeHideShellChrome: hid HHTaskBar window directly");
    }
    else
    {
        CeLog("CeHideShellChrome: HHTaskBar window not found");
    }
}

/* Restores shell chrome on exit - this is a shared-shell CE device, not
 * a single-purpose game handheld, so leaving the taskbar hidden after
 * this app closes would affect the user's other apps until reboot. */
static void CeShowShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
        ShowWindow(hTaskBar, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    CeLog("WinMain start, build " __DATE__ " " __TIME__);

    g_hInstance = hInstance;

    g_mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWndClassName, NULL);
        if (existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CeLog("WinMain: another instance is already running, exiting");
        ExitProcess(0);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
                             /* icon.ico (built from CE/icon.png), embedded as
                              * IDI_APPICON in ce_res.rc. The running window is a
                              * chrome-hidden WS_POPUP so this class icon is never
                              * shown on screen, but shipping the icon as the
                              * .exe's first resource is what makes Windows CE's
                              * shell show it for AppMain.exe in the file list. */

    if (!RegisterClassW(&wc))
    {
        CeLog("WinMain: RegisterClassW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"RegisterClassW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    /* WS_POPUP (not just WS_VISIBLE): both prior CE ports on this
     * hardware confirmed a plain overlapped window is still managed by
     * the shell as a regular window and doesn't reliably reclaim the
     * taskbar's screen space once CeHideShellChrome() hides it. */
    g_hwnd = CreateWindowW(kWndClassName, L"PopSG - No ROM loaded", WS_VISIBLE | WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
    {
        CeLog("WinMain: CreateWindowW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"CreateWindowW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    CeHideShellChrome(g_hwnd);
    /* Re-assert visibility/layout after hiding the taskbar - without
     * this the window doesn't re-layout to cover the space the taskbar
     * just vacated (confirmed by both prior CE ports). */
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    CeConfigLoad(); /* before any *_Init() below - they read their settings out of this shared table */
    CeLogSetEnabled(CeConfigGetInt("VideoDebugLog", 0)); /* Video Config's "Enable Debug Logging" - default off; CeVideoInit() re-applies this too */

    CeLangInit(); /* before ResolveMegaCdBiosDir() below - FindBestMegaCdBiosInDirW()'s region tie-break reads CeLangIsJapanese() */
    CeInputInit();
    CeAudioInit();
    CeVideoInit();
    CeFileOpenInit();

    /* Point the core's system directory at a Mega CD BIOS if one can be
     * found by content, ignoring file names - the folder remembered from
     * a previous pick (config key "MegaCDBiosDir"), else next to the exe
     * (that folder only, no subfolders). No copy/rename on disk; the user
     * keeps the BIOS exactly where they put it. */
    ResolveMegaCdBiosDir();

    retro_set_environment(ce_environment);
    retro_set_video_refresh(ce_video_refresh);
    retro_set_audio_sample(ce_audio_sample_noop);
    retro_set_audio_sample_batch(ce_audio_sample_batch);
    retro_set_input_poll(ce_input_poll);
    retro_set_input_state(ce_input_state);

    retro_init();
    CeLog("WinMain: retro_init done");

    /* Start on the menu ("No ROM loaded", black background) - blocks
     * here until the user opens a ROM (ShowMainMenuDialog only returns
     * once g_romLoaded is true, or the app has already exited via Exit
     * inside the dialog). */
    ShowMainMenuDialog(g_hwnd);

    g_running = 1;
    while (g_running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (!g_romLoaded || g_paused)
        {
            /* No game running (still on the "No ROM loaded" screen) or
             * the touch-to-reveal menu is up: nothing to emulate/blit
             * this tick. Sleep instead of spinning the message pump at
             * 100% CPU for no reason. */
            Sleep(10);
            continue;
        }

        /* Frame limiter - pace emulation to real time. Nothing else
         * throttles the loop: retro_run() is called as fast as it
         * returns, and the GDI blit (ce_display.c) has no vsync wait
         * like GAPI's GXBeginDraw/GXEndDraw used to (dev notes round
         * 34/37). A heavy core (Genesis) already takes longer than a
         * frame so it just runs slow; a light one (GG/SMS, Z80 only)
         * finishes well under a frame and would run ~30-40% too fast. */
        if (CeAudioIsActive())
        {
            /* Audio-backed: the drain thread pulls the ring at exactly
             * the output sample rate = real time, so holding here until
             * the ring drains below CE_FRAME_PACING_HIGH_MS locks
             * emulation to playback speed - automatically right for
             * NTSC/PAL/GG/SMS/MD, no wall-clock math, no region
             * assumptions. A title too heavy to reach full speed keeps
             * the ring near-empty and never sleeps here. The 120-cap is
             * just a safety valve (steady-state sleep is a few ms). */
            unsigned guard;
            for (guard = 0; guard < 120; guard++)
            {
                if (CeAudioGetBufferedMs() <= CE_FRAME_PACING_HIGH_MS)
                    break;
                Sleep(1);
            }
        }
        else if (g_coreFps > 1.0)
        {
            /* No audio device open (waveOutOpen failed / sound stack
             * unavailable) - fall back to a GetTickCount limiter keyed
             * off the core's reported fps. Sub-ms budget is carried
             * between frames so the long-run average is right despite
             * GetTickCount's 1ms resolution. */
            static DWORD    s_nextTick = 0;
            static unsigned s_carryUs  = 0;
            DWORD now = GetTickCount();
            unsigned stepMs;

            s_carryUs += (unsigned)(1000000.0 / g_coreFps + 0.5);
            stepMs = s_carryUs / 1000;
            s_carryUs %= 1000;

            if (s_nextTick == 0)
                s_nextTick = now;
            s_nextTick += stepMs;

            if ((int)(s_nextTick - now) > 0 && (int)(s_nextTick - now) < 250)
                Sleep(s_nextTick - now);
            else if ((int)(now - s_nextTick) > 250)
                s_nextTick = now; /* fell far behind (returned from a dialog, heavy scene) - resync */
        }

        if (g_audioBuffStatusCb)
        {
            /* Contractually "called right before retro_run() every
             * frame" (see the RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_
             * STATUS_CALLBACK doc in libretro.h) - only set while Video
             * Config's Frame Skip is above 0 (ce_video.c), so this is a
             * no-op call when it's Off. */
            int active, underrunLikely;
            unsigned occupancyPercent;

            if (CeVideoFrameSkipShouldForceRender())
            {
                /* Frame Skip's own cap (see ce_video.h) has already been
                 * hit: report "buffer not active" so the core's
                 * FRAMESKIP_AUTO logic (which only skips when told the
                 * buffer is active *and* underrunning) renders this one
                 * frame regardless of the real audio state. */
                active = 0;
                occupancyPercent = 0;
                underrunLikely = 0;
            }
            else
            {
                CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
            }
            g_audioBuffStatusCb(active ? true : false, occupancyPercent, underrunLikely ? true : false);
        }

        {
            /* Per-frame timing for the debug log (CeLog() writes nothing
             * unless Video Config's "Enable Debug Logging" is on) - see
             * ce_audio_sample_batch() and ce_display.c's "perf: blit" for
             * the matching counters. retro_run()'s own wall time already
             * includes both of those (the core calls ce_video_refresh/
             * ce_audio_sample_batch synchronously from inside it), so
             * comparing this average against "perf: blit" + "perf:
             * audio_push" shows how much is core emulation vs. CE-side
             * I/O. */
            static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
            DWORD t0 = GetTickCount();
            unsigned elapsed;

            retro_run();

            elapsed = (unsigned)(GetTickCount() - t0);
            s_accumMs += elapsed;
            if (elapsed > s_maxMs)
                s_maxMs = elapsed;
            if (++s_count >= 60)
            {
                unsigned runAvg = s_accumMs / s_count;
                unsigned blitAvg = 0, blitMax = 0;

                CeLog("perf: retro_run avg=%ums max=%ums over %u frames",
                      runAvg, s_maxMs, s_count);

                /* Unified one-line summary, same format across every
                 * core, for side-by-side comparison. The detailed logs
                 * above/below (retro_run's "over N frames", blit's
                 * write/bitblt breakdown, audio underrun
                 * counts) all stay as they are. blit avg/max come from
                 * ce_display.c's own last completed 60-frame window -
                 * it rolls over on a slightly different frame than this
                 * one (skipped frames don't blit), close enough here. */
                CeDisplayGetBlitPerf(&blitAvg, &blitMax);
                CeLog("perf: retro_run avg=%ums max=%ums blit avg=%ums max=%ums",
                      runAvg, s_maxMs, blitAvg, blitMax);

                s_accumMs = 0;
                s_maxMs = 0;
                s_count = 0;
            }
        }

        /* Periodic SRAM autosave - the pause-time save in
         * ShowMainMenuDialog() only helps if the menu actually gets
         * opened before the device is powered off; this covers a
         * straight-through play session that never touches the menu at
         * all. ~30s is arbitrary (same margin the sister PopSNES
         * port uses) - frequent enough to bound how much an in-game save
         * could be lost, infrequent enough that a `.srm` write is not
         * worth timing/skipping for. */
        {
            static DWORD s_lastSramSaveTick = 0;
            DWORD now = GetTickCount();
            if (s_lastSramSaveTick == 0)
                s_lastSramSaveTick = now; /* first frame of gameplay - start the 30s window now, not at an immediate save */
            else if (now - s_lastSramSaveTick >= 30000)
            {
                CeSaveSram();
                s_lastSramSaveTick = now;
            }
        }
    }

    CeLog("WinMain: normal shutdown");
    CeShutdown(0);
    return 0; /* unreachable - CeShutdown() calls ExitProcess() */
}
