/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise: the sounds of the world. See world.h. Single precision, no libc (the FM-1). A port of the browser
 * prototype's Web Audio graph: the same filters, levels and envelopes. */
#include "world.h"
#include "fastmath.h"
#include "plat.h"

#define SR 44100.0f
volatile world_params_t WP;

static inline float mtof(float m) { return 440.0f * fm_exp2f((m - 69.0f) * (1.0f / 12.0f)); }

static uint32_t rng = 0x9E3779B9u;
static inline float noise(void)
{
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* TPT state-variable filter: g = tan(pi fc / SR), k = 1 / Q */
typedef struct { float ic1, ic2; } svf_t;
typedef struct { float lp, bp; } svf_out_t;
static inline svf_out_t svf(svf_t *f, float x, float g, float k)
{
    svf_out_t o;
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    o.lp = v2;
    o.bp = v1;
    return o;
}
static inline float svf_g(float fc) { return fm_tanf(3.14159265f * fm_clampf(fc, 20.0f, 18000.0f) / SR); }

static inline float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static inline float saw(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }
static inline float tri(float ph) { return 1.0f - 4.0f * fm_fabsf(ph - 0.5f); }

/* ------------------------------------------------------------------ one-shots (main loop -> render) --- */
enum { EV_BELL, EV_PAD, EV_WOOD, EV_STEP, EV_WOOF };
typedef struct { uint8_t t, a; float m, v, d; } ev_t;
#define EQ 32u
static ev_t eq[EQ];
static volatile uint32_t eq_head, eq_tail;
static void push(uint8_t t, uint8_t a, float m, float v, float d)
{
    uint32_t h = eq_head, nh = (h + 1u) % EQ;
    if (nh == eq_tail) return;            /* full (the render is not running): drop */
    eq[h].t = t; eq[h].a = a; eq[h].m = m; eq[h].v = v; eq[h].d = d;
    __asm__ volatile("" ::: "memory");
    eq_head = nh;
}
void world_bell(float midi, float vel, float decay_s) { push(EV_BELL, 0, midi, vel, decay_s); }
void world_pad(float midi, float vel) { push(EV_PAD, 0, midi, vel, 5.0f); }
void world_wood(float midi, float vel) { push(EV_WOOD, 0, midi, vel, 0.5f); }
void world_step(int ground, int accent) { push(EV_STEP, (uint8_t)(ground | (accent ? 0x80 : 0)), 0, 0, 0); }
void world_woof(void) { push(EV_WOOF, 0, 0, 0, 0); }

/* ------------------------------------------------------------------ voices --- */
/* tones: the bells (triangle + two sines), the aurora's pads (two sines, slow), the pines' wood */
#define NT 12
typedef struct {
    uint8_t on, kind;
    float ph[3], inc[3], amp[3];
    float env, peak, atk_inc, dec_mul, dec_lin;
    uint32_t atk_n;
} tone_t;
static tone_t tone[NT];

/* steps and barks: noise or a sweeping square through a lowpass */
#define NS 6
typedef struct {
    uint8_t on, bark;
    svf_t f;
    float g, env, peak, atk_inc, dec_mul;
    uint32_t atk_n, delay_n;
    float ph, inc, inc_mul, ping;      /* the metal ping / the bark's square */
    float ping_env, ping_mul;
} hit_t;
static hit_t hit[NS];

static tone_t *free_tone(void)
{
    int i, best = 0;
    for (i = 0; i < NT; i++) if (!tone[i].on) return &tone[i];
    for (i = 1; i < NT; i++) if (tone[i].env < tone[best].env) best = i;
    return &tone[best];
}
static hit_t *free_hit(void)
{
    int i, best = 0;
    for (i = 0; i < NS; i++) if (!hit[i].on) return &hit[i];
    for (i = 1; i < NS; i++) if (hit[i].env < hit[best].env) best = i;
    return &hit[best];
}

static void start_tone(int kind, float m, float vel, float decay)
{
    tone_t *t = free_tone();
    float f = mtof(m) * fm_exp2f(WP.cold * noise() * 15.0f / 1200.0f), atk;
    int i;
    for (i = 0; i < 3; i++) t->ph[i] = 0.0f;
    t->kind = (uint8_t)kind;
    if (kind == EV_PAD) {
        t->inc[0] = f / SR; t->amp[0] = 1.0f;
        t->inc[1] = f * 2.003f / SR; t->amp[1] = 0.27f;
        t->inc[2] = 0.0f; t->amp[2] = 0.0f;
        atk = 1.5f;
        t->dec_mul = 1.0f;
        t->dec_lin = vel / (3.5f * SR);          /* down to nothing at 5 s */
    } else if (kind == EV_WOOD) {
        t->inc[0] = f / SR; t->amp[0] = 1.0f;
        t->inc[1] = f * 3.98f / SR; t->amp[1] = 0.35f;
        t->inc[2] = f * 9.1f / SR; t->amp[2] = 0.1f;
        atk = 0.002f;
        t->dec_mul = fm_powf(0.001f / fm_maxf(vel, 0.002f), 1.0f / (decay * SR));
        t->dec_lin = 0.0f;
    } else {
        t->inc[0] = f / SR; t->amp[0] = 1.0f;
        t->inc[1] = f * 2.0f / SR; t->amp[1] = 0.25f;
        t->inc[2] = f * 3.01f / SR; t->amp[2] = 0.08f;
        atk = 0.005f;
        t->dec_mul = fm_powf(0.001f / fm_maxf(vel, 0.002f), 1.0f / (decay * SR));
        t->dec_lin = 0.0f;
    }
    t->env = 0.0f;
    t->peak = vel;
    t->atk_n = (uint32_t)(atk * SR);
    t->atk_inc = vel / (float)t->atk_n;
    t->on = 1;
}

/* the steps: [lowpass Hz, length s, level] for snow, ice, fur, metal, cave, boots */
static const float STEP_CONF[NGROUND][3] = {
    {650.0f, 0.14f, 0.16f}, {1300.0f, 0.06f, 0.1f}, {380.0f, 0.12f, 0.1f},
    {2400.0f, 0.05f, 0.1f}, {900.0f, 0.1f, 0.13f}, {900.0f, 0.08f, 0.15f}};

static void start_hit(int ground, int accent, uint32_t delay_n)
{
    hit_t *h = free_hit();
    const float *c = STEP_CONF[ground < NGROUND ? ground : 0];
    float lv = c[2] * (accent ? 1.0f : 0.7f);
    h->bark = 0;
    h->f.ic1 = h->f.ic2 = 0.0f;
    h->g = svf_g(c[0] * (accent ? 1.0f : 0.85f));
    h->env = 0.0f; h->peak = lv;
    h->atk_n = (uint32_t)(0.008f * SR);
    h->atk_inc = lv / (float)h->atk_n;
    h->dec_mul = fm_powf(0.001f / lv, 1.0f / ((c[1] - 0.008f) * SR));
    h->delay_n = delay_n;
    h->ping_env = 0.0f;
    if (ground == GROUND_METAL) {
        h->ph = 0.0f; h->inc = (310.0f + 40.0f * (noise() * 0.5f + 0.5f)) / SR;
        h->ping_env = 0.05f; h->ping_mul = fm_powf(0.001f / 0.05f, 1.0f / (0.12f * SR));
    }
    h->on = 1;
}

static void start_bark(uint32_t delay_n)
{
    hit_t *h = free_hit();
    h->bark = 1;
    h->f.ic1 = h->f.ic2 = 0.0f;
    h->g = svf_g(1200.0f);
    h->env = 0.0f; h->peak = 0.1f;
    h->atk_n = (uint32_t)(0.01f * SR);
    h->atk_inc = 0.1f / (float)h->atk_n;
    h->dec_mul = fm_powf(0.001f / 0.1f, 1.0f / (0.13f * SR));
    h->delay_n = delay_n;
    h->ph = 0.0f; h->inc = 330.0f / SR;
    h->inc_mul = fm_powf(170.0f / 330.0f, 1.0f / (0.11f * SR));
    h->on = 1;
}

/* ------------------------------------------------------------------ the bed: wind, drone --- */
static svf_t wbp, wlp, dlp;
static float w_lfo, w_gust, w_pan, d_lfo;     /* LFO phases, in turns */
static float d_ph[4], d_midi;
static float s_wind, s_drone;                  /* levels, smoothed per block */
static float step_ph;
static volatile uint32_t ticks, steps;
uint32_t world_ticks(void) { return ticks; }
uint32_t world_steps(void) { return steps; }

void world_init(void)
{
    WP.wind = 0.0f; WP.wind_bp = 480.0f; WP.wind_lp = 6000.0f;
    WP.drone = 0.0f; WP.drone_midi = 62.0f; WP.steps = 0.65f; WP.bells = 0.75f; WP.rev = 0.55f;
    WP.bpm = 84.0f; WP.cold = 0.0f; WP.moving = 0; WP.ground = 0; WP.dog = 0;
    d_midi = 62.0f;
}

void world_render(float *dry, float *send, uint32_t n)
{
    uint32_t s, i;
    float wl, gbp, glp, gd, dinc[4], ang, rdry, rsend, bells = WP.bells;
    const float DSEMI[4] = {-24.0f, -24.0f, -17.0f, -36.0f}, DDET[4] = {-6.0f, 7.0f, 0.0f, 0.0f};
    const float DLVL[4] = {0.22f, 0.22f, 0.14f, 0.25f};   /* the sub: half the browser's (a small speaker cannot play it, it only ate the headroom) */
    float half_tick = 0.0f;

    /* the one-shots */
    while (eq_tail != eq_head) {
        ev_t e = eq[eq_tail];
        __asm__ volatile("" ::: "memory");
        eq_tail = (eq_tail + 1u) % EQ;
        if (e.t == EV_STEP) start_hit(e.a & 0x7F, e.a & 0x80, 0);
        else if (e.t == EV_WOOF) { start_bark(0); start_bark((uint32_t)(0.16f * SR)); }
        else start_tone(e.t, e.m, e.v, e.d);
    }

    /* per block: the slow things */
    s_wind += (WP.wind - s_wind) * 0.03f;               /* ~0.2 s */
    s_drone += (WP.drone - s_drone) * 0.02f;
    d_midi += (WP.drone_midi - d_midi) * 0.004f;         /* ~1.2 s glide */
    w_lfo += 0.07f * (float)n / SR; if (w_lfo >= 1.0f) w_lfo -= 1.0f;
    w_gust += 0.13f * (float)n / SR; if (w_gust >= 1.0f) w_gust -= 1.0f;
    w_pan += 0.05f * (float)n / SR; if (w_pan >= 1.0f) w_pan -= 1.0f;
    d_lfo += 0.045f * (float)n / SR; if (d_lfo >= 1.0f) d_lfo -= 1.0f;
    wl = s_wind * (1.0f + 0.8f * fm_sin_turns(w_gust));
    gbp = svf_g(WP.wind_bp + 260.0f * fm_sin_turns(w_lfo));
    glp = svf_g(WP.wind_lp);
    gd = svf_g(380.0f + 160.0f * fm_sin_turns(d_lfo));
    for (i = 0; i < 4; i++) dinc[i] = mtof(d_midi + DSEMI[i] + DDET[i] * 0.01f) / SR;
    ang = fm_clampf(WP.rev, 0.0f, 1.0f) * 1.5707963f;
    rdry = fm_maxf(0.2f, fm_cosf(ang));
    rsend = fm_sinf(ang) * 0.9f;
    {
        float p = (fm_sin_turns(w_pan) + 1.0f) * 0.5f;    /* the wind wanders, left to right: 0..1 */
        float pl = fm_cosf(p * 1.5707963f) * 1.2f, pr = fm_sinf(p * 1.5707963f) * 1.2f;
        for (s = 0; s < n; s++) {
            float x = noise(), wv, dv = 0.0f, bl = 0.0f, st = 0.0f;
            svf_out_t o;
            /* wind: band-passed noise, then a lowpass */
            o = svf(&wbp, x, gbp, 1.25f);
            o = svf(&wlp, o.bp * 1.25f, glp, 1.4142f);
            wv = o.lp * wl * 0.17f;
            /* drone: two detuned saws, a fifth, a sub sine, through a slow lowpass */
            if (s_drone > 1e-4f) {
                for (i = 0; i < 4; i++) {
                    dv += (i < 3 ? saw(d_ph[i], dinc[i]) : fm_sin_turns(d_ph[i])) * DLVL[i];
                    d_ph[i] += dinc[i]; if (d_ph[i] >= 1.0f) d_ph[i] -= 1.0f;
                }
                o = svf(&dlp, dv, gd, 0.5f);
                dv = o.lp * s_drone;
            }
            /* bells, pads, wood */
            for (i = 0; i < NT; i++) {
                tone_t *t = &tone[i];
                float v;
                if (!t->on) continue;
                if (t->atk_n) { t->env += t->atk_inc; t->atk_n--; }
                else if (t->dec_lin > 0.0f) { t->env -= t->dec_lin; if (t->env <= 0.0f) { t->on = 0; continue; } }
                else { t->env *= t->dec_mul; if (t->env < 0.0005f * t->peak + 1e-6f) { t->on = 0; continue; } }
                v = (t->kind == EV_BELL ? tri(t->ph[0]) : fm_sin_turns(t->ph[0])) * t->amp[0]
                  + fm_sin_turns(t->ph[1]) * t->amp[1] + fm_sin_turns(t->ph[2]) * t->amp[2];
                bl += v * t->env;
                t->ph[0] += t->inc[0]; if (t->ph[0] >= 1.0f) t->ph[0] -= 1.0f;
                t->ph[1] += t->inc[1]; if (t->ph[1] >= 1.0f) t->ph[1] -= 1.0f;
                t->ph[2] += t->inc[2]; if (t->ph[2] >= 1.0f) t->ph[2] -= 1.0f;
            }
            bl *= bells;
            /* steps and barks */
            for (i = 0; i < NS; i++) {
                hit_t *h = &hit[i];
                float src, v;
                if (!h->on) continue;
                if (h->delay_n) { h->delay_n--; continue; }
                if (h->atk_n) { h->env += h->atk_inc; h->atk_n--; }
                else { h->env *= h->dec_mul; if (h->env < 0.001f && h->ping_env < 0.001f) { h->on = 0; continue; } }
                if (h->bark) {
                    src = h->ph < 0.5f ? 1.0f : -1.0f;
                    h->ph += h->inc; if (h->ph >= 1.0f) h->ph -= 1.0f;
                    h->inc *= h->inc_mul;
                } else src = noise();
                o = svf(&h->f, src, h->g, 2.0f);
                v = o.lp * h->env;
                if (h->ping_env > 0.0f) {
                    v += fm_sin_turns(h->ph) * h->ping_env;
                    h->ph += h->inc; if (h->ph >= 1.0f) h->ph -= 1.0f;
                    h->ping_env *= h->ping_mul;
                    if (h->ping_env < 1e-4f) h->ping_env = 0.0f;
                }
                st += v;
            }
            st *= WP.steps * 6.0f;              /* the browser squeezed everything through a compressor: the steps were up front */
            /* the eighths: a step on each while walking, and Seito's paws in between */
            step_ph += WP.bpm * (1.0f / 30.0f) / SR;
            if (WP.moving && WP.dog && half_tick == 0.0f && step_ph >= 0.5f) { half_tick = 1.0f; start_hit(GROUND_ICE, 0, 0); }
            if (step_ph >= 1.0f) {
                step_ph -= 1.0f;
                half_tick = 0.0f;
                ticks++;
                if (WP.moving) { start_hit(WP.ground, (ticks & 1u) == 0u, 0); steps++; }
            }
            dry[2 * s] = (wv * pl + dv + (bl * rdry) + st) * 1.5f;
            dry[2 * s + 1] = (wv * pr + dv + (bl * rdry) + st) * 1.5f;
            send[s] = (wv * 0.7f + dv * 0.6f + bl * rsend + st * 0.1f) * 1.5f;
        }
    }
}
