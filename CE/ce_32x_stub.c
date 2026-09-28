/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Backing storage for pico/pico_int.h's NO_32X stub declaration of
 * p32x_event_times[].
 *
 * pico_int.h's "#else" branch of its "#ifndef NO_32X" block already
 * provides empty no-op macros for every 32X function pico.c/state.c call
 * unconditionally (Pico32xInit(), PicoPower32x(), PicoReset32x(),
 * Pico32xPrepare(), Pico32xStartup(), Pico32xShutdown(), PicoFrame32x(),
 * PicoUnload32x(), Pico32xStateLoaded()) - this was an upstream gap in
 * that stub list (missing entries + one wrong-arity macro), not anything
 * CE-specific, so it was fixed directly in pico_int.h (see the comment
 * there) with the user's sign-off, matching this port's "core is not
 * patched" policy for anything short of a genuine upstream bug.
 *
 * p32x_event_times[] is different: pico/state.c uses it as a real array
 * (memset/memcpy target), not just a function call, so a macro can't
 * stand in for it - it needs actual storage. Per the same policy, that
 * storage lives here in a CE-side file rather than in pico_int.h or any
 * other core .c file. The size (5) matches P32X_EVENT_COUNT from the
 * enum in the #ifndef NO_32X branch of pico_int.h (PWM, FILLEND, HINT,
 * MTIMER, STIMER); this array is only ever touched from a runtime path
 * gated on PicoIn.AHW & PAHW_32X, which can never be set true when 32X
 * support isn't compiled in, so its contents never matter.
 *
 * Pico32xMem/PicoScan32xBegin/PicoScan32xEnd are the same situation:
 * pico/draw.c and platform/libretro/libretro.c write/read them directly
 * (not through a function call), also always behind a dead PAHW_32X
 * check. Pico32xMem stays NULL forever here, which is fine since nothing
 * ever dereferences it (the one read, in libretro.c's memory-map setup,
 * is itself behind the same dead check).
 */
#include "pico/pico_int.h"

unsigned int p32x_event_times[5];
struct Pico32xMem *Pico32xMem;
int (*PicoScan32xBegin)(unsigned int num);
int (*PicoScan32xEnd)(unsigned int num);
