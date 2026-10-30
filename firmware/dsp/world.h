/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise: the sounds of the world, beside the synth engine. The wind, the drone, the footsteps (in time,
 * on the eighths), small bells, Seito's bark. The main loop sets the levels and pushes one-shots; world_render
 * runs in the audio interrupt, before eng_render, and hands it a dry stereo block and a reverb send. */
#pragma once
#include <stdint.h>

enum { GROUND_SNOW, GROUND_ICE, GROUND_FUR, GROUND_METAL, GROUND_CAVE, GROUND_BOOTS, NGROUND };

typedef struct {
    float wind;          /* the wind's level, all included (knob, weather, inside or out) */
    float wind_bp;       /* its band, Hz (a storm sits higher) */
    float wind_lp;       /* its top, Hz (inside: muffled) */
    float drone;         /* the drone's level */
    float drone_midi;    /* the drone's tonic (60 + key) */
    float steps;         /* the footsteps' level */
    float bells;         /* the bells' level (the synth knob) */
    float rev;           /* the world's share of reverb, 0..1 */
    float bpm;           /* the steps' tempo: they fall on the eighths */
    float cold;          /* 0..1: the bells shiver */
    uint8_t moving, ground, dog;
} world_params_t;

extern volatile world_params_t WP;

void world_init(void);
void world_bell(float midi, float vel, float decay_s);
void world_pad(float midi, float vel);          /* a slow note that swells and fades (the aurora) */
void world_wood(float midi, float vel);         /* a short wooden note (the pines) */
void world_step(int ground, int accent);
void world_woof(void);
void world_render(float *dry_lr, float *send, uint32_t n);

/* the eighths' clock, and the steps it played (for the screen: the prints, the echo) */
uint32_t world_ticks(void);
uint32_t world_steps(void);
