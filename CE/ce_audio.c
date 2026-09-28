/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * waveOut audio output + Sound Config dialog (see ce_audio.h).
 *
 * waveOutOpen/Prepare/Write/CALLBACK_NULL is the same API family
 * confirmed working on this exact hardware by reference sample code
 * (one-shot SFX + a BGM thread). This is the continuous-stream version
 * of the same idea: N prepared buffers reused round-robin, refilled
 * from a ring buffer once waveOut reports each one done (WHDR_DONE),
 * rather than synthesizing a fixed tone and Sleep()-ing for its
 * duration like those samples do.
 *
 * This device's audio output is mono (user-confirmed, 2026-08-01), and
 * the user wants the output format itself configurable (16-bit or
 * 8-bit; 8000/11000/22000/33000/44000 Hz) via Sound Config - none of
 * which matches the SNES core's native output rate (typically ~32000Hz,
 * whatever retro_get_system_av_info() actually reports), so a real
 * resampler is required, not just a format relabel. The core's L/R
 * stereo stream is downmixed to mono and resampled to the configured
 * output rate using a 16.16 fixed-point linear interpolator - same
 * accumulator style as ce_display.c's blit scaler, and for the same
 * reason: no floats anywhere here (this device's VFP is
 * software-emulated only, per the project-wide float audit in
 * the dev notes).
 */
#include "ce_audio.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

#include <string.h>

#define CE_AUDIO_NUM_BUFFERS   4
#define CE_AUDIO_BUFFER_FRAMES 1024  /* mono samples/buffer - ~32ms/buffer at 32kHz */

/* Ring buffer cushion, in mono samples. User-selectable via Sound
 * Config's "Buffer:" spinner (round 27 of the sister PopSNES
 * project, after a report of audible output latency at the old fixed
 * 8192 = ~256ms). s_ring[] is sized for the largest choice;
 * s_ringFrames holds the live capacity and is what every modulus/
 * fullness test uses. The spinner labels these in KB (UpdateBufferLabel:
 * int16_t mono = 2 bytes/frame, so 2048..65536 frames = 4..128 KB);
 * persisted as "SoundBuffer". Bigger rides out this device's irregular
 * retro_run() timing spikes (the dev notes' perf notes) at the cost of
 * latency. */
static const unsigned kBufferChoices[] = { 2048, 4096, 8192, 16384, 32768, 65536 };
#define CE_BUFFER_CHOICE_COUNT (sizeof(kBufferChoices) / sizeof(kBufferChoices[0]))
#define CE_AUDIO_RING_MAX_FRAMES 65536
static unsigned s_ringFrames = 8192; /* live ring capacity; clamped to a kBufferChoices[] entry in CeAudioInit */

/* Sized for the worst case (16-bit mono = 2 bytes/frame); used as
 * fewer valid bytes when the configured bit depth is 8. */
typedef struct
{
    WAVEHDR       hdr;
    unsigned char data[CE_AUDIO_BUFFER_FRAMES * 2];
    int           queued; /* has this buffer ever been handed to waveOutWrite? */
} CeAudioBuffer;

static CRITICAL_SECTION s_ringCs;
static int16_t s_ring[CE_AUDIO_RING_MAX_FRAMES]; /* mono; only the first s_ringFrames entries are live */
static unsigned s_ringRead  = 0;
static unsigned s_ringWrite = 0;
static unsigned s_ringUsed  = 0;

static HWAVEOUT     s_hWaveOut     = NULL;
static unsigned     s_coreRate     = 0;  /* last sample rate retro_get_system_av_info() reported */
static HANDLE       s_audioThread  = NULL;
static volatile int s_audioRunning = 0;
static volatile int s_paused       = 0;  /* mirrors ce_main.c's g_paused - see CeAudioSetPaused */
static CeAudioBuffer s_buffers[CE_AUDIO_NUM_BUFFERS];

static int s_volumeLevel = 8;     /* 0..10, matches the reference samples' own default; 0 = silent, replacing the old separate Mute checkbox (ported from an earlier prototype's own Volume 0..10 - see ce_res.rc's IDD_SOUNDCONFIG comment) */
static int s_volumeScale = 256;   /* 0..256 fixed-point, recomputed from the above */
static int s_bitDepth    = 16;    /* 8 or 16 */

/* Resample quality: 0 = "Smooth" (linear interpolation between
 * consecutive core-rate samples, the original/default behavior), 1 =
 * "Fast" (zero-order hold / drop-sample - just repeats the latest
 * core-rate sample for every output slot, skipping the interpolation
 * multiply-and-shift below). Added on user request to A/B the two;
 * round-12 profiling (the dev notes) already showed the mixer itself
 * costs ~0-2ms/frame against a ~15-36ms retro_run budget, so this
 * is not expected to move the needle on overall frame time - it's
 * offered as a quality/cost trade-off in its own right, not a fix
 * for the CPU/PPU-bound slowdown. */
static int s_interpMode  = 0;     /* 0 = Smooth, 1 = Fast */

static const unsigned kRateChoices[] = { 8000, 11000, 22000, 33000, 44000 };
#define CE_RATE_CHOICE_COUNT (sizeof(kRateChoices) / sizeof(kRateChoices[0]))
static int s_outputRate = 33000;  /* used only when s_useNativeRate is off */

/* "Native" - output at whatever rate the SNES core itself reports
 * (retro_get_system_av_info(), typically ~32000Hz - none of the fixed
 * choices above match it exactly) instead of one of the fixed choices.
 * At native rate s_resampleStep works out to exactly 0x10000 (1.0), so
 * the resampler below degenerates into a pure 1:1 copy - this is also
 * the answer to "let me turn resampling off": matching the rate makes
 * resampling a no-op rather than needing a separate toggle that would
 * otherwise just play audio at the wrong pitch when left off at a
 * mismatched rate. Default on, since it's the highest-fidelity option
 * and doesn't cost anything extra (the resample loop runs regardless -
 * see CeAudioPushSamples). */
static int s_useNativeRate = 1;

/* Resampler state (core rate -> s_outputRate), persists across
 * CeAudioPushSamples calls so interpolation is continuous across chunk
 * boundaries, not reset every call. Reset whenever the device (re)opens
 * (OpenAudioDevice) since a rate change invalidates the old phase. */
static uint32_t s_resampleStep  = 0;     /* 16.16 fixed point: core samples per output sample */
static uint32_t s_resampleAccum = 0;     /* 16.16 fixed point: position within the current core-sample interval */
static int16_t  s_resamplePrev  = 0;     /* last core-rate mono sample, for interpolation */

static void RecomputeVolumeScale(void)
{
    if (s_volumeLevel <= 0)
        s_volumeScale = 0;
    else if (s_volumeLevel >= 10)
        s_volumeScale = 256;
    else
        s_volumeScale = (s_volumeLevel * 256) / 10;
}

/* ------------------------------------------------------------------ */
/* Ring buffer (mono, 16-bit signed - canonical internal format         */
/* regardless of the configured output bit depth; that conversion only  */
/* happens where a waveOut buffer is actually filled, in the thread)    */
/* ------------------------------------------------------------------ */

static void RingPushOne(int16_t sample)
{
    static unsigned s_overrunCount = 0;

    if (s_ringUsed >= s_ringFrames)
    {
        /* Ring full (consumer thread falling behind) - drop the oldest
         * frame to make room rather than blocking the caller, which is
         * retro_run() on the main thread. >= not ==: a live Buffer-size
         * shrink can leave s_ringUsed briefly past the new, smaller
         * capacity. */
        s_ringRead = (s_ringRead + 1) % s_ringFrames;
        s_ringUsed--;
        if ((++s_overrunCount % 500) == 1)
            CeLog("CeAudioPushSamples: ring overrun (count=%u), dropping oldest audio",
                  s_overrunCount);
    }

    s_ring[s_ringWrite] = sample;
    s_ringWrite = (s_ringWrite + 1) % s_ringFrames;
    s_ringUsed++;
}

size_t CeAudioPushSamples(const int16_t *data, size_t frames)
{
    size_t i;

    if (!s_hWaveOut)
        return frames; /* no ROM/audio session active yet - discard quietly */

    EnterCriticalSection(&s_ringCs);
    for (i = 0; i < frames; i++)
    {
        int32_t l = data[i * 2 + 0];
        int32_t r = data[i * 2 + 1];
        int32_t mono = (l + r) / 2;

        if (s_volumeScale != 256)
            mono = (mono * s_volumeScale) >> 8;

        /* Resample this one core-rate mono sample into zero or more
         * output-rate samples (16.16 fixed-point linear interpolation,
         * same accumulator style as ce_display.c's blit scaler). Emits a
         * sample every time the accumulator crosses 1.0 core-sample-
         * width; s_resampleStep < 0x10000 (upsampling) emits more than
         * one output sample per core sample, > 0x10000 (downsampling)
         * consumes several core samples before emitting one. */
        while (s_resampleAccum < 0x10000)
        {
            if (s_interpMode == 0)
            {
                int32_t interpolated = s_resamplePrev +
                    (((int32_t)mono - s_resamplePrev) * (int32_t)s_resampleAccum >> 16);
                RingPushOne((int16_t)interpolated);
            }
            else
            {
                RingPushOne((int16_t)mono);
            }
            s_resampleAccum += s_resampleStep;
        }
        s_resampleAccum -= 0x10000;
        s_resamplePrev = (int16_t)mono;
    }
    LeaveCriticalSection(&s_ringCs);

    return frames;
}

void CeAudioGetBufferStatus(int *active, unsigned *occupancyPercent, int *underrunLikely)
{
    unsigned used;

    if (!s_hWaveOut)
    {
        *active = 0;
        *occupancyPercent = 0;
        *underrunLikely = 0;
        return;
    }

    EnterCriticalSection(&s_ringCs);
    used = s_ringUsed;
    LeaveCriticalSection(&s_ringCs);

    *active = 1;
    *occupancyPercent = s_ringFrames ? (used * 100) / s_ringFrames : 0;
    /* No hardware data yet on what threshold actually avoids an audible
     * underrun on this device - 25% (matches common frontend defaults
     * elsewhere in the libretro ecosystem) is a starting guess, not a
     * measured value. */
    *underrunLikely = (*occupancyPercent < 25) ? 1 : 0;
}

int CeAudioIsActive(void)
{
    return s_hWaveOut != NULL;
}

unsigned CeAudioGetBufferedMs(void)
{
    unsigned used, rate;

    if (!s_hWaveOut)
        return 0;

    rate = s_useNativeRate ? s_coreRate : (unsigned)s_outputRate;
    if (rate == 0)
        return 0;

    EnterCriticalSection(&s_ringCs);
    used = s_ringUsed;
    LeaveCriticalSection(&s_ringCs);

    /* used <= s_ringFrames (<= 65536), so used*1000 stays well inside
     * 32-bit - no 64-bit math needed. */
    return (used * 1000u) / rate;
}

/* Pops up to maxFrames mono samples into dst; returns how many were
 * actually available (may be less than maxFrames on underrun). */
static unsigned CeAudioPopSamples(int16_t *dst, unsigned maxFrames)
{
    unsigned popped = 0;

    EnterCriticalSection(&s_ringCs);
    while (popped < maxFrames && s_ringUsed > 0)
    {
        dst[popped] = s_ring[s_ringRead];
        s_ringRead = (s_ringRead + 1) % s_ringFrames;
        s_ringUsed--;
        popped++;
    }
    LeaveCriticalSection(&s_ringCs);

    return popped;
}

static DWORD WINAPI CeAudioThreadProc(LPVOID param)
{
    static unsigned s_underrunCount = 0;
    /* Underrun shaping (drain thread only, not touched from any other
     * thread): s_lastMono is the last real sample we emitted, used as the
     * start point of a short fade to zero when the ring runs dry;
     * s_hadUnderrun makes the first fully-real buffer after a gap fade
     * back in from zero. Replaces a hard memset(0) that inserted a flat
     * step mid-waveform on every partial underrun - that step, in and
     * out, was an audible low-frequency chop rather than the clean
     * dropout a real silence would be. Ported 1:1 from the sister PopGBA
     * port, HW-verified there 2026-08-31. */
    static int16_t s_lastMono    = 0;
    static int     s_hadUnderrun = 0;
    unsigned bufIdx = 0;
    int16_t monoTemp[CE_AUDIO_BUFFER_FRAMES];
    (void)param;

    while (s_audioRunning)
    {
        CeAudioBuffer *buf = &s_buffers[bufIdx];
        unsigned popped;
        unsigned i;

        if (buf->queued)
        {
            /* Wait for waveOut to finish playing this buffer before we
             * overwrite the memory it's reading from. */
            while (s_audioRunning && !(buf->hdr.dwFlags & WHDR_DONE))
                Sleep(1);
            if (!s_audioRunning)
                break;
        }

        popped = CeAudioPopSamples(monoTemp, CE_AUDIO_BUFFER_FRAMES);
        if (popped > 0)
            s_lastMono = monoTemp[popped - 1];

        if (popped < CE_AUDIO_BUFFER_FRAMES)
        {
            /* Underrun - the core hasn't produced enough samples yet
             * (this device's emulation often runs slower than real-time,
             * see the dev notes' perf notes). Instead of a hard memset(0)
             * that steps straight from the last real sample to zero (an
             * audible click), ramp from that sample down to zero over
             * ~6ms, then hold zero for any remaining pad - so a starved
             * buffer sounds like a clean fade to silence, not a chopped
             * edge. */
            unsigned pad  = CE_AUDIO_BUFFER_FRAMES - popped;
            unsigned fade = pad < 256 ? pad : 256;
            int32_t  base = s_lastMono;
            unsigned k;
            for (k = 0; k < fade; k++)
                monoTemp[popped + k] =
                    (int16_t)(base - (base * (int32_t)(k + 1)) / (int32_t)fade);
            if (pad > fade)
                memset(&monoTemp[popped + fade], 0,
                       (pad - fade) * sizeof(int16_t));
            s_hadUnderrun = 1;
            s_lastMono    = 0;

            /* While paused (touch-to-reveal menu / a settings dialog is
             * up), retro_run() isn't being called at all, so the ring
             * *always* starves here - that's expected silence, not the
             * emulator falling behind, and counting/logging it as
             * "underrun" made round-20's hardware logs impossible to
             * read (a single long settings-dialog visit could log
             * hundreds of these). Still play the silence (buf->data
             * below), just don't count it. */
            if (!s_paused && (++s_underrunCount % 500) == 1)
                CeLog("CeAudioThreadProc: audio underrun (count=%u), fading to silence",
                      s_underrunCount);
        }
        else if (s_hadUnderrun)
        {
            /* First buffer that's fully real audio again after a gap -
             * fade it in from zero over ~3ms so the return isn't a click
             * either. */
            unsigned fin = 128, k;
            for (k = 0; k < fin; k++)
                monoTemp[k] = (int16_t)(((int32_t)monoTemp[k] * (int32_t)(k + 1))
                                        / (int32_t)fin);
            s_hadUnderrun = 0;
        }

        if (s_bitDepth == 16)
        {
            memcpy(buf->data, monoTemp, CE_AUDIO_BUFFER_FRAMES * sizeof(int16_t));
        }
        else
        {
            /* 8-bit PCM is unsigned, centred on 128. */
            for (i = 0; i < CE_AUDIO_BUFFER_FRAMES; i++)
                buf->data[i] = (unsigned char)((monoTemp[i] >> 8) + 128);
        }

        buf->hdr.dwFlags &= ~WHDR_DONE;
        waveOutWrite(s_hWaveOut, &buf->hdr, sizeof(WAVEHDR));
        buf->queued = 1;

        bufIdx = (bufIdx + 1) % CE_AUDIO_NUM_BUFFERS;
    }

    return 0;
}

void CeAudioStop(void)
{
    unsigned i;

    if (s_audioRunning)
    {
        s_audioRunning = 0;
        if (s_audioThread)
        {
            WaitForSingleObject(s_audioThread, 2000);
            CloseHandle(s_audioThread);
            s_audioThread = NULL;
        }
    }

    if (s_hWaveOut)
    {
        /* Let whatever's still physically queued in the driver finish
         * playing naturally instead of cutting it off with
         * waveOutReset() - this device's DAC audibly pops on that abrupt
         * truncation (same root cause the earlier prototype's sound shutdown avoided
         * by waiting instead of resetting; that fix eliminated the
         * equivalent click there, confirmed on real hardware 2026-08-03,
         * see the dev notes). CeAudioThreadProc has already exited by this
         * point (joined above), so no new buffers are being queued -
         * this just waits out whichever ones are already in flight.
         * Bounded to 500ms (same margin as the earlier prototype) so a stuck/never-DONE
         * buffer can't hang the Sound Config dialog's OK handler. */
        unsigned ticks = GetTickCount();
        int allDone = 0;
        while (!allDone && GetTickCount() - ticks < 500)
        {
            allDone = 1;
            for (i = 0; i < CE_AUDIO_NUM_BUFFERS; i++)
                if (s_buffers[i].queued && !(s_buffers[i].hdr.dwFlags & WHDR_DONE))
                    allDone = 0;
        }

        for (i = 0; i < CE_AUDIO_NUM_BUFFERS; i++)
            waveOutUnprepareHeader(s_hWaveOut, &s_buffers[i].hdr, sizeof(WAVEHDR));
        waveOutClose(s_hWaveOut);
        s_hWaveOut = NULL;
    }
}

/* Pushes enough silence through the ring that, by the time the caller's
 * upcoming CeAudioStop() hard-resets waveOut, whatever was last actually
 * written to the device is silence rather than a mid-waveform sample -
 * only relevant when reopening a device that's already running (a real
 * rate/bit-depth change mid-session), not the first open for a freshly
 * loaded ROM. This targets the *truncation* click specifically; the
 * user-reported pop on every bit-depth switch (2026-08-01) may instead
 * (or additionally) come from this device's DAC/codec itself reacting to
 * the format renegotiation inside waveOutClose/waveOutOpen, which is not
 * something software on this side of the API can suppress - see
 * SoundConfigDlgProc's IDOK comment. Still needs real-hardware
 * confirmation either way. */
static void DrainToSilenceBeforeStop(void)
{
    unsigned i, needed;

    if (!s_audioRunning)
        return;

    needed = s_ringFrames + CE_AUDIO_NUM_BUFFERS * CE_AUDIO_BUFFER_FRAMES;

    EnterCriticalSection(&s_ringCs);
    for (i = 0; i < needed; i++)
        RingPushOne(0);
    LeaveCriticalSection(&s_ringCs);

    /* Bounded wait for the drain thread to actually push that silence
     * out through waveOut, not a poll-until-truly-drained loop - this
     * runs on the Sound Config dialog's OK handler and must not risk
     * hanging it. ~150ms covers CE_AUDIO_NUM_BUFFERS buffers' worth of
     * playback at any of this port's supported rates with margin. */
    Sleep(150);
}

/* (Re)opens waveOut using s_coreRate (must already be set) plus the
 * current s_outputRate/s_bitDepth/mono config, and (re)starts the drain
 * thread. Called both from CeAudioStart (new ROM loaded) and from the
 * Sound Config dialog's OK handler (user changed rate/bit depth while a
 * session is already running). */
static void OpenAudioDevice(void)
{
    WAVEFORMATEX wfx;
    unsigned i;
    unsigned effectiveRate;

    if (s_hWaveOut)
        DrainToSilenceBeforeStop();

    CeAudioStop();

    if (s_coreRate == 0)
        return; /* no ROM loaded yet - nothing to open against */

    effectiveRate = s_useNativeRate ? s_coreRate : (unsigned)s_outputRate;

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1; /* mono - this device's audio output, user-confirmed 2026-08-01 */
    wfx.nSamplesPerSec  = (DWORD)effectiveRate;
    wfx.wBitsPerSample  = (WORD)s_bitDepth;
    wfx.nBlockAlign     = (WORD)(wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&s_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        CeLog("OpenAudioDevice: waveOutOpen failed for rate=%u bits=%d", effectiveRate, s_bitDepth);
        s_hWaveOut = NULL;
        return;
    }

    s_ringRead = s_ringWrite = s_ringUsed = 0;
    s_resampleStep  = ((uint32_t)s_coreRate << 16) / (uint32_t)effectiveRate;
    s_resampleAccum = 0;
    s_resamplePrev  = 0;

    for (i = 0; i < CE_AUDIO_NUM_BUFFERS; i++)
    {
        memset(&s_buffers[i], 0, sizeof(s_buffers[i]));
        s_buffers[i].hdr.lpData         = (LPSTR)s_buffers[i].data;
        s_buffers[i].hdr.dwBufferLength = CE_AUDIO_BUFFER_FRAMES * (s_bitDepth == 16 ? 2 : 1);
        waveOutPrepareHeader(s_hWaveOut, &s_buffers[i].hdr, sizeof(WAVEHDR));
    }

    s_audioRunning = 1;
    s_audioThread = CreateThread(NULL, 0, CeAudioThreadProc, NULL, 0, NULL);
    if (s_audioThread)
        SetThreadPriority(s_audioThread, THREAD_PRIORITY_HIGHEST);
    else
        CeLog("OpenAudioDevice: CreateThread failed, error=%lu", (unsigned long)GetLastError());

    CeLog("OpenAudioDevice: waveOut open, core=%uHz output=%uHz (%s) bits=%d mono",
          s_coreRate, effectiveRate, s_useNativeRate ? "native" : "fixed", s_bitDepth);
}

void CeAudioSetPaused(int paused)
{
    s_paused = paused;
}

void CeAudioStart(double sampleRateHz)
{
    unsigned rate = (unsigned)(sampleRateHz + 0.5);

    if (rate == 0)
        return;

    if (s_hWaveOut && rate == s_coreRate)
        return; /* already running for this core rate - LoadRomFlow calls this on every successful load, including File>Open reloads */

    s_coreRate = rate;
    OpenAudioDevice();
}

/* ------------------------------------------------------------------ */
/* Config file + Sound Config dialog                                    */
/* ------------------------------------------------------------------ */

void CeAudioInit(void)
{
    InitializeCriticalSection(&s_ringCs);

    s_volumeLevel   = CeConfigGetInt("SoundVolume", s_volumeLevel);
    s_outputRate    = CeConfigGetInt("SoundRate", s_outputRate);
    s_bitDepth      = CeConfigGetInt("SoundBits", s_bitDepth);
    s_useNativeRate = CeConfigGetInt("SoundNative", s_useNativeRate);
    s_interpMode    = CeConfigGetInt("SoundInterp", s_interpMode);

    /* Ring buffer size - snap a value that isn't in the current
     * kBufferChoices[] table to the 8192 default. */
    {
        unsigned k, want = (unsigned)CeConfigGetInt("SoundBuffer", (int)s_ringFrames);
        s_ringFrames = 8192;
        for (k = 0; k < CE_BUFFER_CHOICE_COUNT; k++)
            if (kBufferChoices[k] == want) { s_ringFrames = want; break; }
    }

    CeLog("CeAudioInit: loaded volume=%d rate=%d bits=%d native=%d interp=%d buffer=%u from config file",
          s_volumeLevel, s_outputRate, s_bitDepth, s_useNativeRate, s_interpMode, s_ringFrames);

    RecomputeVolumeScale();
}

/* Registry-based persistence (samDesired/RegFlushKey lessons of round
 * 3/8 - see the dev notes) didn't survive an actual power-off on this
 * device (round 9 user report) - now goes through ce_config.c's plain
 * config file instead, same as ce_input.c/ce_video.c. */
static void CeAudioSaveConfig(void)
{
    CeConfigSetInt("SoundVolume", s_volumeLevel);
    CeConfigSetInt("SoundRate", s_outputRate);
    CeConfigSetInt("SoundBits", s_bitDepth);
    CeConfigSetInt("SoundNative", s_useNativeRate);
    CeConfigSetInt("SoundInterp", s_interpMode);
    CeConfigSetInt("SoundBuffer", (int)s_ringFrames);
    CeConfigSave();

    CeLog("CeAudioSaveConfig: saved volume=%d rate=%d bits=%d native=%d interp=%d buffer=%u",
          s_volumeLevel, s_outputRate, s_bitDepth, s_useNativeRate, s_interpMode, s_ringFrames);
}

/* Volume/Rate/Bits/Quality are all "-/value/+" spinners now (ported from
 * an earlier prototype's own Sound Settings dialog, per user
 * request), not a COMBOBOX + RADIOBUTTON pairs - see ce_res.rc's
 * IDD_SOUNDCONFIG comment for why (physical-key focus needs a
 * WS_TABSTOP PUSHBUTTON, and this device's dialog manager doesn't
 * reliably drive a COMBOBOX or RADIOBUTTON group with the D-pad
 * either). Each Update*Label function below just refreshes its own
 * value readout; the underlying s_* state is mutated directly by
 * whatever triggered the change (a -/+ tap, a Left/Right key, or a
 * WM_COMMAND click), same pattern ce_video.c uses for scale mode. */
static void UpdateVolumeLabel(HWND hDlg)
{
    wchar_t text[8];
    _snwprintf(text, 8, L"%d", s_volumeLevel);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE), text);
}

/* Combined step index: 0 is "Native", 1..CE_RATE_CHOICE_COUNT are
 * kRateChoices[0..] - one linear list a -/+ pair (or Left/Right) can
 * step through, replacing the old combo box's index. */
static int CurrentRateStepIndex(void)
{
    unsigned i;

    if (s_useNativeRate)
        return 0;

    for (i = 0; i < CE_RATE_CHOICE_COUNT; i++)
        if ((int)kRateChoices[i] == s_outputRate)
            return (int)(i + 1);

    return 0; /* fallback: Native */
}

static void SetRateFromStepIndex(int index)
{
    if (index <= 0)
        s_useNativeRate = 1;
    else if (index <= (int)CE_RATE_CHOICE_COUNT)
    {
        s_useNativeRate = 0;
        s_outputRate = (int)kRateChoices[index - 1];
    }
}

static void UpdateRateLabel(HWND hDlg)
{
    wchar_t text[16];
    if (s_useNativeRate)
        _snwprintf(text, 16, L"Native");
    else
        _snwprintf(text, 16, L"%u Hz", (unsigned)s_outputRate);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_RATE_VALUE), text);
}

/* Holds the spinner's in-progress choice until commit actually applies
 * it to s_bitDepth. Unlike s_outputRate/s_useNativeRate (which are only
 * ever read inside OpenAudioDevice(), so touching them mid-dialog is
 * harmless), s_bitDepth is read directly by CeAudioThreadProc() on every
 * buffer it fills - live while the device is still open in the OLD
 * format. Setting s_bitDepth as soon as the spinner changed (as this
 * used to) let the thread start writing e.g. 8-bit-packed data into
 * buffers still sized/declared for 16-bit PCM on a still-16-bit waveOut
 * device, which the driver then replayed as garbage 16-bit samples - a
 * sustained drone (or, at zero volume, a repeating constant-byte tone)
 * that continued for as long as the dialog stayed open, since
 * OpenAudioDevice() (the only thing that reopens the device to match)
 * doesn't run until commit (user-reported, 2026-08-03). */
static int s_pendingBitDepth;

static void UpdateBitsLabel(HWND hDlg)
{
    if (CeLangIsJapanese())
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),
                        s_pendingBitDepth == 8 ? L"8\x30d3\x30c3\x30c8" /* 8ビット */
                                               : L"16\x30d3\x30c3\x30c8"); /* 16ビット */
    else
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),
                        s_pendingBitDepth == 8 ? L"8-bit" : L"16-bit");
}

static void UpdateQualityLabel(HWND hDlg)
{
    if (CeLangIsJapanese())
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE),
                        s_interpMode == 1 ? L"\x4f4e\x97f3\x8cea" /* 低音質 */
                                          : L"\x9ad8\x97f3\x8cea"); /* 高音質 */
    else
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE),
                        s_interpMode == 1 ? L"Fast" : L"Smooth");
}

/* Buffer size spinner - ordered like Rate (Left steps down, Right up,
 * no wrap at the ends), stepping through kBufferChoices[]. The readout
 * is shown in KB, untranslated in both languages (same as Rate's
 * numeric "%u Hz"). */
static int CurrentBufferStepIndex(void)
{
    unsigned i;
    for (i = 0; i < CE_BUFFER_CHOICE_COUNT; i++)
        if (kBufferChoices[i] == s_ringFrames)
            return (int)i;
    return 2; /* fallback: 8192 */
}

static void SetBufferFromStepIndex(int index)
{
    if (index < 0)
        index = 0;
    if (index >= (int)CE_BUFFER_CHOICE_COUNT)
        index = (int)CE_BUFFER_CHOICE_COUNT - 1;
    s_ringFrames = kBufferChoices[index];
}

static void UpdateBufferLabel(HWND hDlg)
{
    wchar_t text[16];
    /* s_ring[] is int16_t mono, so bytes = frames * 2; the six choices
     * 2048..65536 frames are 4/8/16/32/64/128 KB. Internally everything
     * (s_ringFrames, kBufferChoices[], the SoundBuffer config key)
     * counts frames - this is a display-only conversion. */
    _snwprintf(text, 16, L"%u KB", (s_ringFrames * 2u) / 1024u);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BUFFER_VALUE), text);
}

/* Snapshotted once at WM_INITDIALOG, compared against at commit time
 * only to decide whether OpenAudioDevice() needs to reopen waveOut
 * (disruptive - a brief pop even with CeAudioStop()'s drain-first
 * fix) - *not* for an IDCANCEL revert, since this device has no
 * meaningful "discard changes" gesture (see the IDOK/IDCANCEL handling
 * below): Volume/Rate/Bits/Quality are all mutated directly in s_* as
 * the user steps through them (like ce_video.c's scale mode), same as
 * every settings dialog in an earlier prototype. */
static int s_sessionStartOutputRate;
static int s_sessionStartUseNativeRate;
static int s_sessionStartBitDepth;
/* Ring capacity is just a modulus (no waveOut reopen), so this is only
 * snapshotted to decide whether to flush the ring on commit - see the
 * IDOK/IDCANCEL handler. */
static unsigned s_sessionStartRingFrames;

#define WM_SETSOUNDFOCUS (WM_APP + 203)

static int SoundNeighborDown(int id)
{
    switch (id)
    {
    case IDC_SC_VOLUME_VALUE:  return IDC_SC_RATE_VALUE;
    case IDC_SC_RATE_VALUE:    return IDC_SC_BITS_VALUE;
    case IDC_SC_BITS_VALUE:    return IDC_SC_QUALITY_VALUE;
    case IDC_SC_QUALITY_VALUE: return IDC_SC_BUFFER_VALUE;
    case IDC_SC_BUFFER_VALUE:  return IDOK;
    case IDOK:                 return IDC_SC_VOLUME_VALUE;
    }
    return id;
}

static int SoundNeighborUp(int id)
{
    switch (id)
    {
    case IDC_SC_VOLUME_VALUE:  return IDOK;
    case IDC_SC_RATE_VALUE:    return IDC_SC_VOLUME_VALUE;
    case IDC_SC_BITS_VALUE:    return IDC_SC_RATE_VALUE;
    case IDC_SC_QUALITY_VALUE: return IDC_SC_BITS_VALUE;
    case IDC_SC_BUFFER_VALUE:  return IDC_SC_QUALITY_VALUE;
    case IDOK:                 return IDC_SC_BUFFER_VALUE;
    }
    return id;
}

static WNDPROC s_pSoundOrigProc = NULL;

/* Same blanket WM_GETDLGCODE/WANTALLKEYS subclassing as ce_video.c's
 * VideoCtrlProc, for the same reason (see that function's comment) -
 * this device's dialog manager doesn't reliably move focus with the
 * arrow keys between dissimilar control types, or route decide to
 * anything but the DEFPUSHBUTTON unless a control claims every key and
 * handles it itself. */
static LRESULT CALLBACK SoundCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);

    if (message == WM_GETDLGCODE)
    {
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    }
    else if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
            SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborUp(id)));
            return 0;

        case VK_DOWN:
            SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborDown(id)));
            return 0;

        case VK_RETURN:
            if (id == IDC_SC_VOLUME_VALUE || id == IDC_SC_RATE_VALUE ||
                id == IDC_SC_BITS_VALUE || id == IDC_SC_QUALITY_VALUE ||
                id == IDC_SC_BUFFER_VALUE)
            {
                /* Spinners, not toggles - decide just moves on to the
                 * next control, same as every value spinner here. */
                SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborDown(id)));
            }
            else if (id == IDOK)
            {
                /* IDOK is subclassed too now (for the Up/Down wrap), so
                 * its own decide press has to be forwarded explicitly
                 * instead of falling through with no effect. */
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            return 0;

        case VK_LEFT:
            if (id == IDC_SC_VOLUME_VALUE)
            {
                if (s_volumeLevel > 0) s_volumeLevel--;
                RecomputeVolumeScale();
                UpdateVolumeLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_RATE_VALUE)
            {
                int idx = CurrentRateStepIndex();
                if (idx > 0) SetRateFromStepIndex(idx - 1);
                UpdateRateLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BITS_VALUE)
            {
                /* Only two states, so Left and Right are deliberately
                 * identical (both flip) - unlike Volume/Rate, which are
                 * ordered spinners where direction matters. */
                s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
                UpdateBitsLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_QUALITY_VALUE)
            {
                s_interpMode = !s_interpMode;
                UpdateQualityLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BUFFER_VALUE)
            {
                SetBufferFromStepIndex(CurrentBufferStepIndex() - 1);
                UpdateBufferLabel(GetParent(hWnd));
            }
            return 0;

        case VK_RIGHT:
            if (id == IDC_SC_VOLUME_VALUE)
            {
                if (s_volumeLevel < 10) s_volumeLevel++;
                RecomputeVolumeScale();
                UpdateVolumeLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_RATE_VALUE)
            {
                int idx = CurrentRateStepIndex();
                if (idx < (int)CE_RATE_CHOICE_COUNT) SetRateFromStepIndex(idx + 1);
                UpdateRateLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BITS_VALUE)
            {
                s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
                UpdateBitsLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_QUALITY_VALUE)
            {
                s_interpMode = !s_interpMode;
                UpdateQualityLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BUFFER_VALUE)
            {
                SetBufferFromStepIndex(CurrentBufferStepIndex() + 1);
                UpdateBufferLabel(GetParent(hWnd));
            }
            return 0;

        case VK_ESCAPE:
            /* Claiming WANTALLKEYS above means this control, not the
             * dialog manager, now sees the physical Back key too -
             * without this it would silently do nothing while focus was
             * on one of these controls, instead of committing and
             * closing like OK does (see IDOK/IDCANCEL below). */
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pSoundOrigProc, hWnd, message, wParam, lParam);
}

/* Press-and-hold auto-repeat for Volume's -/+ only (matching the earlier prototype: its
 * own Rate/Bits/Quality -/+ don't get this either - they step through a
 * short, ordered/two-state list where repeated firing isn't as useful
 * as it is for Volume's wider 0..10 range). Same subclass-with-a-timer
 * technique as ce_video.c's Frame Skip -/+ (FrameSkipButtonSubclassProc)
 * - kept as its own small copy here rather than a shared helper, same
 * as this project's existing per-file duplication (e.g.
 * IsRoutineDialogChatter/IsRoutineWindowChatter). */
#define VOLUME_REPEAT_TIMER_ID     1
#define VOLUME_REPEAT_INITIAL_MS   500
#define VOLUME_REPEAT_INTERVAL_MS  120

static WNDPROC s_origVolumeBtnProc = NULL;
static int     s_volumeRepeatDir   = 0; /* +1 = up, -1 = down, 0 = not held */
static int     s_volumeRepeatFast  = 0;

static LRESULT CALLBACK VolumeButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        s_volumeRepeatDir  = (GetDlgCtrlID(hwnd) == IDC_SC_VOLUME_PLUS) ? 1 : -1;
        s_volumeRepeatFast = 0;
        SetTimer(hwnd, VOLUME_REPEAT_TIMER_ID, VOLUME_REPEAT_INITIAL_MS, NULL);
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        KillTimer(hwnd, VOLUME_REPEAT_TIMER_ID);
        s_volumeRepeatDir = 0;
        break;

    case WM_TIMER:
        if (wParam == VOLUME_REPEAT_TIMER_ID && s_volumeRepeatDir != 0)
        {
            if (!s_volumeRepeatFast)
            {
                s_volumeRepeatFast = 1;
                SetTimer(hwnd, VOLUME_REPEAT_TIMER_ID, VOLUME_REPEAT_INTERVAL_MS, NULL);
            }

            if (s_volumeRepeatDir > 0)
            {
                if (s_volumeLevel < 10) s_volumeLevel++;
            }
            else
            {
                if (s_volumeLevel > 0) s_volumeLevel--;
            }
            RecomputeVolumeScale();
            UpdateVolumeLabel(GetParent(hwnd));
        }
        break;
    }

    return CallWindowProc(s_origVolumeBtnProc, hwnd, msg, wParam, lParam);
}

/* One-shot at WM_INITDIALOG (nothing in this dialog changes
 * CeLangIsJapanese() while it's open). The Rate/Bits/Quality value
 * readouts are deliberately left out here - Rate is numeric/technical
 * (untranslated in both languages, same call made for Video Config's
 * scale-mode radios), and Bits/Quality's own text comes from
 * UpdateBitsLabel/UpdateQualityLabel above instead. */
/* Labels repainted by WM_PAINT via CeBmpFontPaintLabel() (Shinonome
 * bitmap-font migration) - the "-/value/+" spinner buttons plus OK are
 * BS_OWNERDRAW (ce_res.rc) instead and redraw themselves via
 * WM_DRAWITEM. */
static const int kSoundLabelIds[] = {
    IDC_SC_LBL_VOLUME, IDC_SC_LBL_RATE, IDC_SC_LBL_BITS, IDC_SC_LBL_QUALITY,
    IDC_SC_LBL_BUFFER,
};
#define CE_SOUND_LABEL_COUNT (sizeof(kSoundLabelIds) / sizeof(kSoundLabelIds[0]))

static void ApplySoundConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_SC_LBL_VOLUME,    L"\x97f3\x91cf\x3a");            /* 音量: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_RATE,      L"\x30ec\x30fc\x30c8\x3a");      /* レート: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BITS,      L"\x30d3\x30c3\x30c8\x3a");      /* ビット: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_QUALITY,   L"\x97f3\x8cea\x3a");            /* 音質: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BUFFER,    L"\x30d0\x30c3\x30d5\x30a1\x3a"); /* バッファ: */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_SC_LBL_VOLUME,    L"Volume:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_RATE,      L"Rate:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BITS,      L"Bits:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_QUALITY,   L"Quality:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BUFFER,    L"Buffer:");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_SOUND_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kSoundLabelIds[i]), SW_HIDE);
}

static INT_PTR CALLBACK SoundConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        s_sessionStartOutputRate = s_outputRate;
        s_sessionStartUseNativeRate = s_useNativeRate;
        s_sessionStartBitDepth = s_bitDepth;
        s_sessionStartRingFrames = s_ringFrames;
        s_pendingBitDepth = s_bitDepth;

        UpdateVolumeLabel(hDlg);
        UpdateRateLabel(hDlg);
        UpdateBitsLabel(hDlg);
        UpdateQualityLabel(hDlg);
        UpdateBufferLabel(hDlg);
        ApplySoundConfigLanguage(hDlg);

        s_pSoundOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE),  GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_RATE_VALUE),    GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),    GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE), GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_BUFFER_VALUE),  GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                 GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);

        /* Volume's -/+ auto-repeat - see VolumeButtonSubclassProc's own
         * comment above. Independent of the SoundCtrlProc subclassing
         * above (these two buttons aren't WS_TABSTOP, so SoundCtrlProc
         * never touches them). */
        s_volumeRepeatDir = 0;
        s_origVolumeBtnProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_MINUS), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_MINUS), GWLP_WNDPROC, (LONG_PTR)VolumeButtonSubclassProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_PLUS),  GWLP_WNDPROC, (LONG_PTR)VolumeButtonSubclassProc);

        /* Belt-and-suspenders initial focus, same pattern as every other
         * dialog in this port - a synchronous SetFocus() from
         * WM_INITDIALOG alone doesn't always stick on this device. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
        PostMessage(hDlg, WM_SETSOUNDFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        unsigned i;
        hdc = BeginPaint(hDlg, &ps);
        for (i = 0; i < CE_SOUND_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kSoundLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
            PostMessage(hDlg, WM_SETSOUNDFOCUS, 0, 0);
        }
        break;

    case WM_SETSOUNDFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SC_VOLUME_MINUS:
            if (s_volumeLevel > 0) s_volumeLevel--;
            RecomputeVolumeScale();
            UpdateVolumeLabel(hDlg);
            return TRUE;

        case IDC_SC_VOLUME_PLUS:
            if (s_volumeLevel < 10) s_volumeLevel++;
            RecomputeVolumeScale();
            UpdateVolumeLabel(hDlg);
            return TRUE;

        case IDC_SC_RATE_MINUS:
        {
            int idx = CurrentRateStepIndex();
            if (idx > 0) SetRateFromStepIndex(idx - 1);
            UpdateRateLabel(hDlg);
            return TRUE;
        }

        case IDC_SC_RATE_PLUS:
        {
            int idx = CurrentRateStepIndex();
            if (idx < (int)CE_RATE_CHOICE_COUNT) SetRateFromStepIndex(idx + 1);
            UpdateRateLabel(hDlg);
            return TRUE;
        }

        case IDC_SC_BITS_MINUS:
        case IDC_SC_BITS_PLUS:
            /* Same touch-only pair as Volume/Rate's own -/+ above, but
             * Bits only has two states, so both buttons do the same
             * thing (flip) - see SoundCtrlProc's Left/Right handling for
             * why. */
            s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
            UpdateBitsLabel(hDlg);
            return TRUE;

        case IDC_SC_QUALITY_MINUS:
        case IDC_SC_QUALITY_PLUS:
            s_interpMode = !s_interpMode;
            UpdateQualityLabel(hDlg);
            return TRUE;

        case IDC_SC_BUFFER_MINUS:
            SetBufferFromStepIndex(CurrentBufferStepIndex() - 1);
            UpdateBufferLabel(hDlg);
            return TRUE;

        case IDC_SC_BUFFER_PLUS:
            SetBufferFromStepIndex(CurrentBufferStepIndex() + 1);
            UpdateBufferLabel(hDlg);
            return TRUE;

        case IDOK:
        case IDCANCEL:
        {
            /* Physical Back (IDCANCEL) acts the same as touching OK
             * here - this device has no meaningful "discard changes"
             * gesture, only "go back", so both commit and close (same
             * philosophy as every settings dialog in an earlier prototype).
             * Only s_outputRate/s_useNativeRate/
             * s_pendingBitDepth (vs. their session-start snapshot) need
             * a reopen - volume/quality apply live in software and
             * never touch waveOut. */
            int reopenNeeded = (s_outputRate != s_sessionStartOutputRate ||
                                 s_pendingBitDepth != s_sessionStartBitDepth ||
                                 s_useNativeRate != s_sessionStartUseNativeRate);

            if (reopenNeeded)
            {
                /* Quiesce the audio thread/device BEFORE s_bitDepth
                 * changes under it - see s_pendingBitDepth. CeAudioStop()
                 * is safe to call here even though OpenAudioDevice()
                 * below will effectively call it again: both its
                 * audioRunning and hWaveOut checks make the second call
                 * a no-op once this one has already torn things down. */
                CeAudioStop();
            }

            s_bitDepth = s_pendingBitDepth;

            /* Ring capacity is just a modulus - no waveOut reopen - but
             * if the user changed it, reset the read/write cursors so
             * they can't sit past the (possibly smaller) new capacity.
             * A brief silence here is fine; the drain thread refills it
             * from the next retro_run(). Not gated by reopenNeeded. */
            if (s_ringFrames != s_sessionStartRingFrames)
            {
                EnterCriticalSection(&s_ringCs);
                s_ringRead = s_ringWrite = s_ringUsed = 0;
                LeaveCriticalSection(&s_ringCs);
            }

            CeAudioSaveConfig();

            /* Reopening waveOut used to audibly pop; CeAudioStop() no
             * longer calls waveOutReset() (let queued buffers finish
             * naturally instead of cutting them off), which real-
             * hardware testing confirmed silences the pop for the
             * output-rate case (2026-08-03). Only reopen when the format
             * actually changed, not on every commit (e.g. just
             * adjusting volume shouldn't touch waveOut at all). */
            if (reopenNeeded)
                OpenAudioDevice();

            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

void CeShowSoundConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_SOUNDCONFIG),
               owner, SoundConfigDlgProc);
}
