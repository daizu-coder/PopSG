/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * waveOut-backed audio output for the CE frontend: a lock-protected
 * ring buffer fed by retro_audio_sample_batch_t (called from the main
 * thread, inside retro_run - must never block) and drained by a
 * dedicated high-priority thread that owns the actual waveOut device,
 * same "producer on the emulation thread, consumer on its own thread"
 * split as the previous WSNES9X-based CE port used.
 */
#ifndef CE_AUDIO_H
#define CE_AUDIO_H

#include <windows.h>
#include <stdint.h>

/* Loads saved volume/mute from the registry and readies the ring
 * buffer's lock. Call once from WinMain, before any ROM is loaded. */
void CeAudioInit(void);

/* Opens (or re-opens, if the rate changed) the waveOut device at
 * sampleRateHz and starts the drain thread. Safe to call every time a
 * ROM loads (including File>Open reloads) - a no-op if already running
 * at the same rate. */
void CeAudioStart(double sampleRateHz);

/* Stops the drain thread and closes the waveOut device. Safe to call
 * even if never started. */
void CeAudioStop(void);

/* retro_audio_sample_batch_t hook - copies interleaved L/R int16
 * samples into the ring buffer (applying the current volume/mute),
 * dropping the oldest buffered audio on overrun rather than blocking.
 * Always returns frames (all "consumed" from the core's point of
 * view), matching the libretro contract. */
size_t CeAudioPushSamples(const int16_t *data, size_t frames);

void CeShowSoundConfigDialog(HWND owner);

/* Reports this device's actual audio ring buffer fill level, for
 * ce_main.c to forward to the core's retro_audio_buffer_status_callback_t
 * (RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK) once per frame -
 * what Video Config's frame skip "Auto" mode (ce_video.c) needs to
 * actually do anything, since the core's FRAMESKIP_AUTO/AUTO_THRESHOLD
 * logic only skips frames based on values reported through that
 * callback (see libretro/libretro.c's check_variables/retro_run). Safe
 * to call every frame regardless of whether a device is open. */
void CeAudioGetBufferStatus(int *active, unsigned *occupancyPercent, int *underrunLikely);

/* True while a waveOut device is actually open (CeAudioStart succeeded
 * and CeAudioStop hasn't run). ce_main.c's frame limiter uses this to
 * decide between audio-backed pacing and its GetTickCount fallback. */
int CeAudioIsActive(void);

/* Milliseconds of not-yet-consumed audio currently sitting in the ring
 * buffer, at the current output rate (0 if no device is open). The
 * drain thread pulls the ring at exactly the output sample rate = real
 * time, so ce_main.c's frame limiter holds a frame back whenever this
 * climbs above a few frames' worth - that's what keeps a light core
 * (GG/SMS) from outrunning real time now that the GDI blit has no
 * implicit vsync wait (see that limiter, and dev notes round 34/37). */
unsigned CeAudioGetBufferedMs(void);

/* Tells the drain thread that retro_run() has stopped being called (the
 * touch-to-reveal menu / a settings dialog is up, ce_main.c's g_paused).
 * Ring will legitimately starve during this time since nothing is
 * pushing new samples - CeAudioThreadProc keeps playing silence exactly
 * as before, this only stops it from misreporting that starvation as an
 * "audio underrun" (which the log/counter otherwise can't tell apart
 * from the emulator genuinely falling behind - see round-20 dev notes
 * investigation). Call from ce_main.c wherever g_paused is set/cleared. */
void CeAudioSetPaused(int paused);

#endif
