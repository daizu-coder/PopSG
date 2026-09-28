/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#include "ce_lang.h"
#include "ce_config.h"

static int g_japanese = 1;   /* default: Japanese UI (user request) */

void CeLangInit(void)
{
    g_japanese = CeConfigGetInt("UILanguageJapanese", 1);
}

int CeLangIsJapanese(void)
{
    return g_japanese;
}

void CeLangSetJapanese(int japanese)
{
    g_japanese = japanese ? 1 : 0;
    CeConfigSetInt("UILanguageJapanese", g_japanese);
}
