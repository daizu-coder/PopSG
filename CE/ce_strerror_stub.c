/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Minimal strerror()/perror() for this cegcc toolchain.
 *
 * <string.h>/<stdio.h> declare these (matching the real C runtime's
 * signatures) but no cegcc library actually implements them - Windows CE
 * has no POSIX errno/strerror table. zlib/gzio.c's gzerror() calls
 * strerror() (via its own zstrerror() macro) on its Z_ERRNO path, and
 * pico/sound/vgm.c calls both gzerror() (real VGM file loading) and
 * perror() directly (its gzopen() failure path), so these need to exist
 * and behave sanely, not just compile.
 */
#include <stdio.h>
#include <errno.h>

static char s_strerror_buf[32];

char *strerror(int errnum)
{
	sprintf(s_strerror_buf, "error %d", errnum);
	return s_strerror_buf;
}

void perror(const char *s)
{
	if (s && *s)
		fprintf(stderr, "%s: %s\n", s, strerror(errno));
	else
		fprintf(stderr, "%s\n", strerror(errno));
}
