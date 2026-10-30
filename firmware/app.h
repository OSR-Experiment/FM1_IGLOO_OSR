/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise: a musical walk on an ice field, for the M-VAVE FM-1 (on fm1-base).
 * This header is the app's half of the contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "BANQUISE"
#define APP_SPLASH_RGB RGB(143, 216, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 240u)      /* the whole screen in one canvas: the world is drawn in one pass */

/* the store: the walk (position, places found, the map seen, the knobs) and the panel calibration (the platform's) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
