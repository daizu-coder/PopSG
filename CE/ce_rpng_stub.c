/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-ins for libretro-common's path_is_valid() (file/file_path.c) and
 * rpng_*() (formats/rpng/rpng.c), neither of which is compiled into this
 * build.
 *
 * Both are used by exactly one feature in platform/libretro/libretro.c:
 * load_pico_overlay()/readpng(), which loads a PNG "storybook" overlay
 * image for Sega Pico cartridges (a niche children's-title peripheral
 * with a touch pen, synced to a printed overlay). This CE port has no
 * touch-pen/overlay UI at all, so the feature is unreachable regardless
 * of what these functions do - they only need to exist so libretro.c
 * links and to fail gracefully *if* something ever did reach them.
 *
 * path_is_valid() always reporting "not found" is enough on its own to
 * make load_pico_overlay() never call into readpng() at all (see its
 * `if (!fname || ...)` check); the rpng_*() stubs below exist only as a
 * belt-and-suspenders fallback (readpng() already handles rpng_alloc()
 * returning NULL via its own `if (!rpng || ...) goto done;`), and are
 * never actually invoked in practice.
 */
#include <stddef.h>
#include "platform/libretro/libretro-common/include/formats/rpng.h"

bool path_is_valid(const char *path)
{
	(void)path;
	return false;
}

rpng_t *rpng_alloc(void)
{
	return NULL;
}

void rpng_free(rpng_t *rpng)
{
	(void)rpng;
}

bool rpng_set_buf_ptr(rpng_t *rpng, void *data, size_t len)
{
	(void)rpng;
	(void)data;
	(void)len;
	return false;
}

bool rpng_start(rpng_t *rpng)
{
	(void)rpng;
	return false;
}

bool rpng_iterate_image(rpng_t *rpng)
{
	(void)rpng;
	return false;
}

int rpng_process_image(rpng_t *rpng, void **data, size_t size,
		unsigned *width, unsigned *height, bool supports_rgba)
{
	(void)rpng;
	(void)size;
	(void)width;
	(void)height;
	(void)supports_rgba;
	*data = NULL;
	return -1; /* IMAGE_PROCESS_ERROR_END - anything but IMAGE_PROCESS_NEXT
	              (0), or the caller's "while (ret == ...NEXT)" loop never
	              terminates */
}

bool rpng_is_valid(rpng_t *rpng)
{
	(void)rpng;
	return false;
}
