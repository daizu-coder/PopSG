/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Physical keyboard -> SNES joypad input for the CE frontend. Polling-
 * based (GetAsyncKeyState), not message-based, so it works the same way
 * whether the app has keyboard focus quirks or not, and so the Input
 * Config dialog (remap-by-press) can reuse the exact same primitive.
 */
#ifndef CE_INPUT_H
#define CE_INPUT_H

#include <windows.h>
#include <stdint.h>

/* Loads any saved key mapping from the registry (falls back to the
 * built-in defaults for anything not saved yet). Call once from
 * WinMain before the first CeInputPoll(). */
void CeInputInit(void);

/* retro_input_poll_t hook: samples all mapped keys once per frame. */
void CeInputPoll(void);

/* retro_input_state_t hook: id is a RETRO_DEVICE_ID_JOYPAD_* constant. */
int16_t CeInputState(unsigned port, unsigned device, unsigned index, unsigned id);

/* Modal native dialog (IDD_INPUTCONFIG): click a button, then press the
 * physical key to bind to that SNES button. OK persists the mapping to
 * the registry; Cancel discards changes made in this dialog session. */
void CeShowInputConfigDialog(HWND owner);

/* True while a remap button click is waiting for a key press (see
 * BeginWaitForKey in ce_input.c). ce_main.c's WndProc uses this to log
 * messages that reach the *main* window during that window - diagnostic
 * for the still-undetected "\x6c7a\x5b9a" (Decide/OK) button: if its
 * WM_KEYDOWN (or anything else) is being routed to the main frame window
 * instead of the modal IDD_INPUTCONFIG dialog, InputConfigDlgProc's own
 * diagnostic logging would never see it, but this would. */
int CeInputIsWaitingForKeyRemap(void);

#endif
