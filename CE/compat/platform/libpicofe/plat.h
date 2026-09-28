/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for the real platform/libpicofe/plat.h.
 *
 * libpicofe is a whole vendored SDL-based frontend toolkit submodule that
 * was never fetched for this port (see the dev notes: this CE port writes its
 * own Win32 frontend instead, same as this port's sister CE ports). The
 * only thing platform/libretro/libretro.c actually needs from the real
 * plat.h is the PXMAKE pixel-pack macro, used by its local readpng() to
 * build overlay-image pixels (see the "need this for PXMAKE in readpng"
 * comment at its #include site). readpng()'s overlay-loading feature is
 * not exercised by this CE frontend (no UI path ever sets an overlay PNG
 * filename), so the exact packing just needs to compile and be a
 * plausible RGB565 pack, matching the pixel format this port's GAPI blit
 * (CE/ce_display.c) already uses everywhere else.
 */
#ifndef CE_COMPAT_PLAT_H
#define CE_COMPAT_PLAT_H

#define PXMAKE(r, g, b) \
	((unsigned short)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3)))

#endif
