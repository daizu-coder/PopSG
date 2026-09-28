/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for platform/common/ogg.c on this port.
 *
 * The real ogg.c needs either the bundled Tremor decoder (a full vendored
 * Ogg Vorbis library, platform/common/tremor/) or a system libvorbis -
 * neither is worth pulling in for this first CE bring-up, whose stated
 * scope explicitly excludes full Sega CD support (see the dev notes): CD
 * *code* is still compiled in (pico/cd/*.c has no source-level way to
 * exclude it - see the dev notes' architecture notes), but only for its
 * raw BIN/CUE path; CHD images are unsupported (use_libchdr=0, no
 * libchdr submodule) and so are compressed .ogg Redbook audio tracks
 * (this file). A .cue that references a plain, uncompressed .wav track
 * instead of .ogg does not go through this at all.
 *
 * These four symbols are exactly what pico/cd/cd_image.c (handle_ogg())
 * and the CD sound-mixing path call; matching platform/common/mp3_dummy.c's
 * existing "always report unsupported" pattern for the equivalent MP3
 * case rather than inventing a new one.
 */
#include "pico/pico_int.h"

int ogg_get_length(void *f)
{
	(void)f;
	return 0;
}

void ogg_start_play(void *f, int sample_offset)
{
	(void)f;
	(void)sample_offset;
}

void ogg_stop_play(void)
{
}

void ogg_update(s32 *buffer, int length, int stereo)
{
	(void)buffer;
	(void)length;
	(void)stereo;
}
