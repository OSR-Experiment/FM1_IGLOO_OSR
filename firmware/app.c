/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise for the FM-1: a musical walk on an ice field. Part of the unity build (device/unity.c,
 * host/plat_host.c), after the platform: gfx.c is in scope. The synth is dsp/engine.c (OSR-1's), the world's
 * sounds dsp/world.c. A port of the browser prototype (index.html in the repository's root).
 *
 * Step 1 of the port: the walk. The ice field (96 x 96 tiles), Obsimo, Seito, the wind, the drone and the steps,
 * day and night, the igloo and its synth, and the places that wake the synth up.
 *
 * Controls
 *   EDIT / PLAY / SEQ / REC   walk up / down / left / right
 *   LFO        action          SCL  chord grid        HOME  go home
 *   ALGORITHM  the sky (cycle, day, night, storm)    PRESET  the place's sound
 *   K1 wind  K2 drone  K3 steps  K4 synth             keys  chords (near a place, or at home) */
#include "fastmath.h"
#include "engine.h"
#include "world.h"
#include "data.h"
#include "draw.h"

static int32_t iclamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float smooth(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }
static float hyp(float x, float y) { return fm_sqrtf(x * x + y * y); }
static float fm_fmodf_pos(float x, float m) { return x - fm_floorf(x / m) * m; }
static float fm_atan2f_approx(float y, float x)   /* within 0.005 rad: enough for a compass needle */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a, r, s;
    if (ax < 1e-9f && ay < 1e-9f) return 0.0f;
    a = fm_minf(ax, ay) / fm_maxf(ax, ay);
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0.0f) r = 3.14159274f - r;
    if (y < 0.0f) r = -r;
    return r;
}

static uint32_t rs = 0x2545F491u;
static float frand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (float)(rs >> 8) * (1.0f / 16777216.0f); }

static void text_cat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do t[n++] = (char)('0' + u % 10u); while ((u /= 10u) && n < 11);
    if (v < 0) *b++ = '-';
    while (n) *b++ = t[--n];
    *b = 0;
}
/* "SPLEEN ANGLAIS" -> "Spleen anglais" */
static void nice(const char *s, char *b)
{
    int first = 1;
    for (; *s; s++, b++) {
        char c = *s;
        if (!first && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        *b = c;
        first = 0;
    }
    *b = 0;
}

/* ------------------------------------------------------------------ the world --- */
#define T 16
#define MW 96
#define MH 96
enum { SNOW, ICE, RIDGE, SEA, HOLE, OBJ, TREE, PITT };
static uint8_t map[MW * MH] PLAT_POOL;
static uint8_t explored[MW * MH] PLAT_POOL;
static inline int at(int x, int y) { return (x < 0 || y < 0 || x >= MW || y >= MH) ? SEA : map[y * MW + x]; }
static inline void put(int x, int y, int t) { if (x >= 0 && y >= 0 && x < MW && y < MH) map[y * MW + x] = (uint8_t)t; }
static inline int solid(int t) { return t == RIDGE || t == SEA || t == HOLE || t == OBJ || t == TREE || t == PITT; }

enum { SC_OUT, SC_IGLOO };
enum { PL_IGLOO, PL_TROU, PL_BATEAU, PL_GROTTE, PL_FORET, PL_PHARE, PL_PATIN, PL_PHOQUES, PL_FEU, NPLACE };
typedef struct {
    const char *name;
    float cx, cy, r;
    int8_t door_x, door_y;
    const char *grid;
    uint8_t key;
    float rev, cut;
    const char *var[5];
} place_t;
static const place_t PLACES[NPLACE] = {
    {"The igloo", 48, 51, 8, 48, 52, "REVERIE INDIE", 5, 0.3f, 0.62f, {"VELOURS", "CASSETTE", "WURLI", "TINES", "MELLOW"}},
    {"The big hole", 30, 66, 8, -1, -1, "LYDIEN FLOTTANT", 2, 0.8f, 0.78f, {"GOUTTES", "KALIMBA", "CLOCHE", "MARIMBA", "VERRE"}},
    {"The stranded boat", 60, 9.5f, 8, 60, 11, "SPLEEN ANGLAIS", 9, 0.66f, 0.52f, {"ORGUE NOYE", "ORGUE FANE", "HARMONIUM", "METAL DOUX", "BANDE USEE"}},
    {"The ice cave", 80, 71, 8, 80, 72, "CIEL ETRANGE", 4, 0.92f, 0.82f, {"CRISTAUX", "VERRE", "AUBE", "SITAR FANTOME", "FLUTE DE VERRE"}},
    {"The forest", 19, 40, 14, -1, -1, "DORIEN CHAUD", 2, 0.5f, 0.6f, {"KOTO", "MARIMBA", "GUITARE SECHE", "HARPE", "KALIMBA"}},
    {"The lighthouse", 88, 15.5f, 8, -1, -1, "CINEMA", 5, 0.72f, 0.55f, {"ORGUE FANE", "AUBE", "BRUME", "DERIVE", "VITRAIL"}},
    {"The ice rink", 52, 82, 10, -1, -1, "DISCO", 0, 0.4f, 0.62f, {"RAVE LOINTAIN", "VITRAIL", "STAB HOUSE", "SIROP", "WURLI"}},
    {"The seals", 22, 7, 8, -1, -1, "POP LUMINEUX", 7, 0.6f, 0.7f, {"CLOCHE", "KALIMBA", "VERRE", "MARIMBA", "GOUTTES"}},
    {"The campfire", 68, 56.5f, 8, -1, -1, "COUCHER DE SOLEIL", 7, 0.35f, 0.5f, {"GUITARE SECHE", "WURLI", "CASSETTE", "MELLOW", "HARMONIUM"}},
};
static const char *const IGLOO_BANK[10] = {"VELOURS", "TINES", "WURLI", "CASSETTE", "MELLOW", "SIROP", "VITRAIL", "HARPE",
                                           "CLAVECIN LUNAIRE", "PIANO JOUET"};
static const char *const IGLOO_GRIDS[10] = {"REVERIE INDIE", "PSYCHE REVEUR", "BALLADE", "INDIE PSYCHE", "COUCHER DE SOLEIL",
                                            "LYDIEN FLOTTANT", "SPLEEN ANGLAIS", "MELANCOLIE", "REVE 80S", "AMBIENT"};
#define FIRE_X 68
#define FIRE_Y 57
#define PIT_X 6
#define PIT_Y 63
#define PIT2_X 90
#define PIT2_Y 87
#define PHARE_X (89 * T)
#define PHARE_Y (17 * T)

typedef struct { const char *name; uint8_t key; float bpm, wind, rev; } weather_t;
static const weather_t WEATHER[3] = {{"Day", 2, 84, 0.35f, 0.55f}, {"Night", 9, 72, 0.22f, 0.7f}, {"Storm", 4, 96, 1.0f, 0.45f}};
static const char *const MODES[4] = {"Day / night cycle", "Day", "Night", "Storm"};
#define DAY_LEN 480.0f

static int sound_idx(const char *n)
{
    int i;
    for (i = 0; i < NFAMILY * PER_FAMILY; i++) if (!strcmp(SOUND_NAME[i], n)) return i;
    return 0;
}
static int grid_idx(const char *n)
{
    int i;
    for (i = 0; i < NGRID; i++) if (!strcmp(GRID_DEF[i].name, n)) return i;
    return 0;
}

/* the map: the browser's, tile for tile (the same seed, the same noise) */
static int32_t seed;
static float rnd(void)
{
    int32_t hi = seed / 127773, lo = seed % 127773, t = 16807 * lo - 2836 * hi;   /* Park-Miller, Schrage's way */
    if (t <= 0) t += 2147483647;
    seed = t;
    return (float)seed / 2147483647.0f;
}
static void clear_ridges(float cx, float cy, float r)
{
    int x, y;
    for (y = (int)fm_floorf(cy - r); y <= cy + r; y++)
        for (x = (int)fm_floorf(cx - r); x <= cx + r; x++)
            if (at(x, y) == RIDGE) put(x, y, SNOW);
}
static void build_map(void)
{
    int x, y, i, k;
    seed = 23;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) {
            float n = fm_sinf((float)x * 0.19f) * fm_cosf((float)y * 0.17f) + fm_sinf((float)(x + y) * 0.09f)
                    + 0.5f * fm_sinf((float)x * 0.05f - (float)y * 0.07f);
            put(x, y, n > 1.05f ? ICE : SNOW);
        }
    for (x = 0; x < MW; x++) {
        float dd = 3.0f + 1.6f * fm_sinf((float)x * 0.21f) + fm_sinf((float)x * 0.9f) * 0.7f;
        int d = (int)fm_floorf(dd + 0.5f);
        for (y = 0; y < d; y++) put(x, y, SEA);
    }
    for (y = 0; y < MH; y++) {
        int d = 1 + (int)fm_floorf(fm_fabsf(fm_sinf((float)y * 0.3f)) * 1.5f + 0.5f);
        for (k = 0; k < d; k++) { put(k, y, SEA); put(MW - 1 - k, y, SEA); }
    }
    for (x = 0; x < MW; x++) { put(x, MH - 1, SEA); if (fm_sinf((float)x * 0.5f) > 0.3f) put(x, MH - 2, SEA); }
    for (i = 0; i < 46; i++) {
        int px = 4 + (int)fm_floorf(rnd() * 88.0f), py = 8 + (int)fm_floorf(rnd() * 84.0f);
        int dx = rnd() < 0.5f ? 1 : 0, dy = 1 - dx, len = 3 + (int)fm_floorf(rnd() * 6.0f);
        for (k = 0; k < len; k++) { if (rnd() > 0.2f) put(px, py, RIDGE); px += dx; py += dy; }
    }
    for (i = 0; i < NPLACE; i++) clear_ridges(PLACES[i].cx, PLACES[i].cy + 1.0f, 7.0f);
    clear_ridges(82, 46, 3);
    for (y = 61; y <= 71; y++)
        for (x = 25; x <= 35; x++)
            if (hyp((float)(x - 30), (float)(y - 66) * 1.15f) < 3.4f) put(x, y, HOLE);
    for (y = 7; y <= 10; y++) for (x = 55; x <= 65; x++) put(x, y, OBJ);
    for (x = 54; x <= 66; x++) if (at(x, 6) == SEA) put(x, 6, SNOW);
    for (y = 50; y <= 52; y++) for (x = 46; x <= 50; x++) put(x, y, OBJ);
    for (y = 69; y <= 72; y++) for (x = 77; x <= 83; x++) put(x, y, OBJ);
    for (i = 0; i < NPLACE; i++) if (PLACES[i].door_x >= 0) put(PLACES[i].door_x, PLACES[i].door_y, SNOW);
    put(60, 11, SNOW); put(60, 10, OBJ);
    /* the forest: snowy pines around a winding path and a small clearing */
    for (y = 29; y <= 51; y++)
        for (x = 3; x <= 36; x++) {
            float ex = (float)(x - 19) / 15.0f, ey = (float)(y - 40) / 10.0f, e = ex * ex + ey * ey;
            int path, clearing;
            if (e > 1.0f) continue;
            path = fm_fabsf((float)y - (40.0f + 2.5f * fm_sinf((float)x * 0.45f))) < 1.3f
                || fm_fabsf((float)x - (19.0f + 2.0f * fm_sinf((float)y * 0.5f))) < 1.1f;
            clearing = hyp((float)(x - 19), (float)(y - 40)) < 2.6f;
            put(x, y, (!path && !clearing && rnd() < 0.5f - e * 0.2f) ? TREE : (at(x, y) == RIDGE ? SNOW : at(x, y)));
        }
    put(FIRE_X, FIRE_Y, OBJ);
    for (y = PIT_Y - 3; y <= PIT_Y + 3; y++) for (x = PIT_X - 3; x <= PIT_X + 3; x++) if (at(x, y) != SEA) put(x, y, SNOW);
    put(PIT_X, PIT_Y, PITT);
    for (y = PIT2_Y - 2; y <= PIT2_Y + 2; y++) for (x = PIT2_X - 2; x <= PIT2_X + 2; x++) if (at(x, y) != SEA) put(x, y, SNOW);
    put(PIT2_X, PIT2_Y, PITT);
    /* the skating rink */
    for (y = 74; y <= 90; y++)
        for (x = 40; x <= 64; x++) {
            float ex = (float)(x - 52) / 7.5f, ey = (float)(y - 82) / 4.6f, e = ex * ex + ey * ey;
            if (e < 1.0f) put(x, y, ICE);
            else if (e < 1.6f && at(x, y) == RIDGE) put(x, y, SNOW);
        }
    put(47, 52, SNOW); put(49, 52, SNOW);
    for (y = 0; y <= 4; y++) for (x = 16; x <= 28; x++) put(x, y, SEA);
    for (x = 19; x <= 25; x++) put(x, 3, OBJ);
    for (y = 5; y <= 11; y++)
        for (x = 15; x <= 29; x++)
            if (at(x, y) == RIDGE || at(x, y) == TREE || at(x, y) == SEA) put(x, y, SNOW);
    for (y = 14; y <= 16; y++) for (x = 88; x <= 89; x++) put(x, y, OBJ);
}

static void reveal(float px, float py)
{
    int cx = (int)(px / T), cy = (int)(py / T), x, y;
    const int R = 7;
    for (y = cy - R; y <= cy + R; y++)
        for (x = cx - R; x <= cx + R; x++)
            if (x >= 0 && y >= 0 && x < MW && y < MH && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= R * R) explored[y * MW + x] = 1;
}

/* ------------------------------------------------------------------ the state, and its save --- */
#define SAVE_MAGIC 0x42514E31u            /* "BQN1" */
typedef struct {
    uint32_t magic;
    float px, py, dog_x, dog_y, day_clock, play_time, cold;
    uint8_t scene, dog_found, weather_mode, igloo_sound, igloo_grid, synth_on;
    uint8_t kn[4];                        /* wind, drone, steps, synth: 0..20 (x 5 %) */
    uint8_t variant[NPLACE];
    uint8_t place_grid[NPLACE];           /* 255: the place's own */
    uint16_t discovered;
    uint8_t seen[MW * MH / 8];            /* the map seen, a bit a tile */
} save_t;
static save_t sv;

typedef struct { float x, y; uint8_t dir, moving; uint32_t step_anim; } actor_t;
enum { D_DOWN, D_UP, D_LEFT, D_RIGHT };
static actor_t P;
static struct { float x, y; uint8_t dir, found; } DOG;
static int scene;
static float out_x, out_y;               /* where to come back out */
static int weather, weather_mode;
static float day_clock = 40.0f, play_time, cold;
static float kn[4] = {0.6f, 0.55f, 0.65f, 0.75f};
static uint8_t variant[NPLACE], place_grid[NPLACE];
static uint16_t discovered = 1u << PL_IGLOO;
static int igloo_sound, igloo_grid, synth_on;
static uint32_t save_due;                 /* plat_ms when to save (0: nothing to save) */

/* the trail behind Obsimo: Seito follows it */
#define TRAIL_N 40
static float trail_x[TRAIL_N], trail_y[TRAIL_N];
static int trail_n;
static void trail_push(float x, float y)
{
    if (trail_n == TRAIL_N) {
        memmove(trail_x, trail_x + 1, sizeof(float) * (TRAIL_N - 1));
        memmove(trail_y, trail_y + 1, sizeof(float) * (TRAIL_N - 1));
        trail_n--;
    }
    trail_x[trail_n] = x; trail_y[trail_n] = y; trail_n++;
}

/* footprints, and the last steps' notes (the echo) */
#define PRINT_N 160
static struct { float x, y; uint32_t t, lit; float note; } prints[PRINT_N];
static int print_head, print_count;
#define LOG_N 16
static int step_log[LOG_N], log_n, echo_idx;   /* indexes into prints */

/* ------------------------------------------------------------------ chords --- */
typedef struct { uint8_t semi; char q; } chord_def_t;
static chord_def_t grid_w[NGRID][8], grid_b[NGRID][5];
static const char *const NOTE[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const uint8_t WHITE_IDX[8] = {0, 2, 4, 5, 7, 9, 11, 12}, BLACK_IDX[5] = {1, 3, 6, 8, 10};
static struct { int grid, key, rich; } st;

static int parse_chords(const char *s, chord_def_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        int v = 0;
        while (*s == ' ') s++;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        out[n].semi = (uint8_t)v;
        out[n].q = *s ? *s++ : 'M';
        n++;
    }
    return n;
}
static int quality_ivs(char q, int rich, int8_t *iv)
{
    static const int8_t M[4][6] = {{0,4,7}, {0,4,7,11}, {0,4,7,11,14}, {0,4,7,11,14,21}};
    static const int8_t V[4][6] = {{0,4,7}, {0,4,7,10}, {0,4,7,10,14}, {0,4,7,10,14,21}};
    static const int8_t m[4][6] = {{0,3,7}, {0,3,7,10}, {0,3,7,10,14}, {0,3,7,10,14,17}};
    static const int8_t D[4][6] = {{0,4,7,10}, {0,4,7,10}, {0,4,7,10,14}, {0,4,7,10,14,21}};
    static const int8_t d[4][6] = {{0,3,6}, {0,3,6,10}, {0,3,6,10,14}, {0,3,6,10,14,17}};
    static const int8_t s4[4][6] = {{0,5,7}, {0,5,7,10}, {0,5,7,10,14}, {0,5,7,10,14,21}};
    static const int8_t s2[4][6] = {{0,2,7}, {0,2,7,10}, {0,2,7,10}, {0,2,7,10,21}};
    static const int8_t a[4][6] = {{0,4,8}, {0,4,8,11}, {0,4,8,11,14}, {0,4,8,11,14}};
    static const uint8_t NM[4] = {3, 4, 5, 6}, ND[4] = {4, 4, 5, 6}, NS2[4] = {3, 4, 4, 5}, NA[4] = {3, 4, 5, 5};
    const int8_t *src;
    int n, i;
    rich = iclamp(rich, 0, 3);
    switch (q) {
    case 'V': src = V[rich]; n = NM[rich]; break;
    case 'm': src = m[rich]; n = NM[rich]; break;
    case 'D': src = D[rich]; n = ND[rich]; break;
    case 'd': src = d[rich]; n = NM[rich]; break;
    case 's': src = s4[rich]; n = NM[rich]; break;
    case 'S': src = s2[rich]; n = NS2[rich]; break;
    case 'a': src = a[rich]; n = NA[rich]; break;
    default: src = M[rich]; n = NM[rich]; break;
    }
    for (i = 0; i < n; i++) iv[i] = src[i];
    return n;
}

typedef struct { int root; int8_t iv[6]; int n; int oct; } chord_t;
#define KEY_C4 7                         /* key index of C4 (key 0 is F3) */
static chord_t build_chord(int key)
{
    chord_t c;
    const chord_def_t *d;
    int rel = key - KEY_C4, pos = ((rel % 12) + 12) % 12, oct = rel < 0 ? -1 : rel / 12, i;
    d = &grid_w[st.grid][0];
    if (pos == 0 && oct >= 1) d = &grid_w[st.grid][7];
    else {
        for (i = 0; i < 7; i++) if (WHITE_IDX[i] == pos) d = &grid_w[st.grid][i];
        for (i = 0; i < 5; i++) if (BLACK_IDX[i] == pos) d = &grid_b[st.grid][i];
    }
    c.root = (st.key + d->semi) % 12;
    c.n = quality_ivs(d->q, st.rich, c.iv);
    c.oct = oct;
    return c;
}
static int has(const chord_t *c, int pc)
{
    int i;
    for (i = 0; i < c->n; i++) if (c->iv[i] % 12 == pc) return 1;
    return 0;
}
static void chord_name(const chord_t *c, char *b)
{
    int m = has(c, 3) && !has(c, 4), M = has(c, 4);
    int sus4 = !m && !M && has(c, 5), sus2 = !m && !M && !has(c, 5) && has(c, 2);
    int dim = m && has(c, 6) && !has(c, 7), aug = M && has(c, 8) && !has(c, 7);
    int maj7 = has(c, 11), min7 = has(c, 10), six = has(c, 9), add9 = has(c, 2) && !sus2;
    int eleven = has(c, 5) && (m || M);
    b[0] = 0;
    text_cat(b, NOTE[c->root]);
    if (dim) { text_cat(b, min7 ? "m7b5" : "dim"); return; }
    if (m) text_cat(b, "m");
    if (aug) text_cat(b, "+");
    if (maj7 || min7) {
        if (maj7) text_cat(b, "maj");
        text_cat(b, six ? "13" : eleven ? "11" : add9 ? "9" : "7");
    } else if (six && add9) text_cat(b, "6/9");
    else if (six) text_cat(b, "6");
    else if (add9) text_cat(b, "add9");
    if (sus4) text_cat(b, "sus4");
    if (sus2) text_cat(b, "sus2");
}
static void voice_chord(const chord_t *c, float *out)
{
    int pcs[6], np = 0, i, r, base, best_set = 0;
    int center = 64 + c->oct * 7;
    float best = 1e9f;
    for (i = 0; i < c->n; i++) {
        int p = (c->root + c->iv[i]) % 12, k, dup = 0;
        for (k = 0; k < np; k++) if (pcs[k] == p) dup = 1;
        if (!dup) pcs[np++] = p;
    }
    if (np > 4) { int k, j = 0; for (k = 0; k < np; k++) if (pcs[k] != (c->root + 7) % 12) pcs[j++] = pcs[k]; np = j; }
    if (np > 4) { int k, j = 0; for (k = 0; k < np; k++) if (pcs[k] != c->root) pcs[j++] = pcs[k]; np = j; }
    if (np > 4) np = 4;
    for (r = 0; r < np; r++)
        for (base = 50; base <= 72; base++) {
            int notes[4], k, cur = base, cnt = 0;
            float mean = 0.0f, cost;
            for (k = 0; k < np; k++) {
                int p = pcs[(r + k) % np];
                while (((cur % 12) + 12) % 12 != p) cur++;
                notes[cnt++] = cur++;
            }
            if (cnt == 3) notes[cnt++] = notes[0] + 12;
            if (notes[0] < 50 || notes[cnt - 1] > 84) continue;
            for (k = 0; k < 4; k++) mean += (float)notes[k];
            mean *= 0.25f;
            cost = fm_fabsf(mean - (float)center);
            if (cost < best) { best = cost; for (k = 0; k < 4; k++) out[k] = (float)notes[k]; best_set = 1; }
        }
    if (!best_set) for (i = 0; i < 4; i++) out[i] = 60.0f + (float)i * 4.0f;
    /* opened (drop 2), as the walk's synth plays them */
    out[2] -= 12.0f;
    for (i = 1; i < 4; i++) { int j; for (j = i; j > 0 && out[j - 1] > out[j]; j--) { float t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; } }
}

/* ------------------------------------------------------------------ the music: which place, which sound --- */
static int ctx_place = -1, near_place = -1;
static float near_w, near_any_w;
static int held = -1, latched = -1, user_here, last_home_chord = KEY_C4;
static int chord_tones[6] = {0, 4, 7, 11}, n_tones = 4;
static char chord_txt[16];
static float syn_level, syn_target, syn_tc = 0.5f;   /* the engine's fader */
static uint32_t hold_until, swap_at;     /* a sound swap under the fader: fade down, swap at swap_at */
static int swap_pending;
static float next_chord_t, resume_t, music_t;   /* seconds, the music's clock */
static int prog_idx;
static const int PROG[4] = {KEY_C4 + 0, KEY_C4 + 9, KEY_C4 + 5, KEY_C4 + 7};

static float bpm(void) { return scene == SC_IGLOO ? 84.0f : WEATHER[weather].bpm; }
static float beat(void) { return 60.0f / bpm(); }
static int drone_key(void) { return scene == SC_OUT && ctx_place >= 0 ? PLACES[ctx_place].key : scene == SC_IGLOO ? st.key : WEATHER[weather].key; }

static const char *cur_sound_name(void)
{
    if (scene == SC_IGLOO) return IGLOO_BANK[igloo_sound];
    return ctx_place >= 0 ? PLACES[ctx_place].var[variant[ctx_place]] : "";
}

static void set_grid(void)
{
    if (scene == SC_IGLOO) { st.grid = grid_idx(IGLOO_GRIDS[igloo_grid]); st.key = 5; }
    else if (ctx_place >= 0) {
        st.grid = place_grid[ctx_place] != 255 ? place_grid[ctx_place] : grid_idx(PLACES[ctx_place].grid);
        st.key = PLACES[ctx_place].key;
    } else { st.grid = grid_idx("AMBIENT"); st.key = WEATHER[weather].key; }
    st.rich = GRID_DEF[st.grid].rich;
}

static void send_sound(void)
{
    sound_t s = SOUNDS[sound_idx(cur_sound_name())];
    s.wob = (s.wob > 0.0f ? s.wob : 5.0f) + cold * 28.0f;
    if (cold > 0.3f) s.wob_rate = 5.0f + cold * 3.0f;
    if (scene == SC_OUT) { s.atk = fm_maxf(s.atk, 0.45f); s.rel = fm_maxf(s.rel, 1.4f); }   /* outside it only swells and fades */
    eng_sound(&s);
}
static void send_hold(void) { rhythm_t r; memset(&r, 0, sizeof r); r.hold = 1; eng_rhythm(&r); }

static void play_idx(int idx, float glide)
{
    chord_t c = build_chord(idx);
    float notes[4];
    int i;
    voice_chord(&c, notes);
    chord_name(&c, chord_txt);
    n_tones = c.n;
    for (i = 0; i < c.n; i++) chord_tones[i] = (c.root + c.iv[i]) % 12;
    eng_chord(notes, (float)(36 + c.root), glide);
}

static float cutoff(void)
{
    float cut = scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].cut : 0.6f) : 0.66f;
    return 70.0f * fm_powf(18000.0f / 70.0f, cut - cold * 0.12f);
}
static float rev_amount(void)
{
    return scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].rev : WEATHER[weather].rev) : 0.22f;
}

/* the knobs and the scene into the world's levels */
static void apply_levels(void)
{
    const weather_t *W = &WEATHER[weather];
    int muffled = scene != SC_OUT, forest = scene == SC_OUT && ctx_place == PL_FORET;
    WP.wind = kn[0] * (0.25f + 0.75f * W->wind) * (muffled ? 0.25f : forest ? 0.6f : 1.0f);
    WP.wind_lp = muffled ? 380.0f : forest ? 1300.0f : 6000.0f;
    WP.wind_bp = W->wind > 0.8f ? 900.0f : 480.0f;
    WP.drone = scene == SC_IGLOO ? 0.0f : kn[1] * 0.3f;
    WP.drone_midi = (float)(60 + drone_key());
    WP.steps = kn[2];
    WP.bells = kn[3];
    WP.rev = scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].rev : W->rev) : 0.3f;
    WP.bpm = bpm();
    WP.cold = cold;
}

/* every change of sound happens under the engine's own fader: down, swap, back up */
static void setup_music(float fade_s, uint32_t wait_ms, int keep_sound)
{
    set_grid();
    eng_params(bpm(), cutoff(), 1.0f, 0, 0, 1);
    eng_fx(rev_amount(), 0.0f);
    apply_levels();
    if (keep_sound) return;
    {
        int leaving = scene == SC_OUT && ctx_place < 0;
        if (fade_s <= 0.0f) fade_s = leaving ? 0.7f : 0.12f;
        if (!wait_ms) wait_ms = leaving ? 2600u : 420u;
    }
    syn_target = 0.0f;
    syn_tc = fade_s;
    swap_pending = 1;
    swap_at = plat_ms() + wait_ms;
    hold_until = swap_at;
}
static void swap_now(void)
{
    swap_pending = 0;
    if (scene == SC_OUT && ctx_place < 0) { eng_stop(); return; }
    send_sound();
    send_hold();
    if (scene == SC_IGLOO) { if (latched >= 0) play_idx(latched, 0.0f); else eng_stop(); }
    else if (held >= 0) play_idx(held, 0.0f);
    else { prog_idx = 0; play_idx(PROG[0], 0.0f); next_chord_t = music_t + 8.0f * beat(); }
}

static void eng_target(void)
{
    float w, target, tc;
    int mine;
    if (swap_pending || plat_ms() < hold_until) return;
    w = scene == SC_OUT ? (ctx_place >= 0 ? smooth(near_w) * 0.85f : 0.0f) : 1.0f;
    mine = scene == SC_OUT && user_here;
    target = mine ? (held >= 0 ? fm_maxf(w, 0.7f) : 0.0f) : w;
    tc = mine ? (held >= 0 ? 0.03f : 1.4f) : target > syn_level ? 1.1f : 0.6f;
    syn_target = target;
    syn_tc = tc;
}

static void music_tick(void)
{
    if (scene == SC_IGLOO || swap_pending || plat_ms() < hold_until) return;
    if (held < 0 && !(scene == SC_OUT && user_here) && music_t >= resume_t && music_t >= next_chord_t && (scene != SC_OUT || ctx_place >= 0)) {
        prog_idx = (prog_idx + 1) % 4;
        play_idx(PROG[prog_idx], 0.8f);
        next_chord_t = music_t + 8.0f * beat();
    }
}

/* ------------------------------------------------------------------ small sounds --- */
static const int PENTA[5] = {0, 2, 4, 7, 9};
static float chime_note(void)
{
    int k = drone_key();
    return (float)(72 + k + PENTA[(int)(frand() * 4.999f)] + (frand() < 0.3f ? 12 : 0) - (k > 6 ? 12 : 0));
}

/* ------------------------------------------------------------------ messages on screen --- */
static struct { char text[40]; uint32_t until; } ban;
static void banner(const char *s)
{
    uint32_t i;
    for (i = 0; i < sizeof ban.text - 1u && s[i]; i++) ban.text[i] = s[i];
    ban.text[i] = 0;
    ban.until = plat_ms() + 2300u;
}
void app_say(const char *a, const char *b)
{
    char t[48];
    t[0] = 0;
    if (a) text_cat(t, a);
    if (b && b[0]) { text_cat(t, " "); text_cat(t, b); }
    banner(t);
}

/* ------------------------------------------------------------------ saving --- */
static void save_soon(void) { save_due = plat_ms() + 1500u; }
static void save_now(void)
{
    int i;
    memset(&sv, 0, sizeof sv);
    sv.magic = SAVE_MAGIC;
    sv.px = scene == SC_OUT ? P.x : out_x;
    sv.py = scene == SC_OUT ? P.y : out_y;
    sv.dog_x = DOG.x; sv.dog_y = DOG.y;
    sv.day_clock = day_clock; sv.play_time = play_time; sv.cold = cold;
    sv.scene = SC_OUT;
    sv.dog_found = DOG.found;
    sv.weather_mode = (uint8_t)weather_mode;
    sv.igloo_sound = (uint8_t)igloo_sound; sv.igloo_grid = (uint8_t)igloo_grid; sv.synth_on = 0;
    for (i = 0; i < 4; i++) sv.kn[i] = (uint8_t)(kn[i] * 20.0f + 0.5f);
    for (i = 0; i < NPLACE; i++) { sv.variant[i] = variant[i]; sv.place_grid[i] = place_grid[i]; }
    sv.discovered = discovered;
    for (i = 0; i < MW * MH; i++) if (explored[i]) sv.seen[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (plat_store_save(OBJ_SETTINGS, &sv, sizeof sv) == 0) save_due = 0;
    else save_due = plat_ms() + 5000u;
}
static int load_save(void)
{
    int i;
    if (plat_store_load(OBJ_SETTINGS, &sv, sizeof sv) != (int)sizeof sv || sv.magic != SAVE_MAGIC) return 0;
    if (!(sv.px > 0.0f && sv.px < MW * T && sv.py > 0.0f && sv.py < MH * T)) return 0;
    P.x = sv.px; P.y = sv.py;
    DOG.x = sv.dog_x; DOG.y = sv.dog_y; DOG.found = sv.dog_found ? 1 : 0;
    day_clock = sv.day_clock; play_time = sv.play_time; cold = clamp01(sv.cold);
    weather_mode = sv.weather_mode % 4u;
    igloo_sound = sv.igloo_sound % 10u; igloo_grid = sv.igloo_grid % 10u;
    for (i = 0; i < 4; i++) kn[i] = clamp01((float)sv.kn[i] * 0.05f);
    for (i = 0; i < NPLACE; i++) { variant[i] = sv.variant[i] % 5u; place_grid[i] = sv.place_grid[i] < NGRID ? sv.place_grid[i] : 255; }
    discovered = sv.discovered | (1u << PL_IGLOO);
    for (i = 0; i < MW * MH; i++) explored[i] = (uint8_t)((sv.seen[i >> 3] >> (i & 7)) & 1u);
    return 1;
}

/* ------------------------------------------------------------------ the day --- */
static float night_amt(void)
{
    float p;
    if (weather_mode == 1 || weather_mode == 3) return 0.0f;
    if (weather_mode == 2) return 1.0f;
    p = day_clock / DAY_LEN;
    p -= fm_floorf(p);
    return p < 0.55f ? 0.0f : p < 0.65f ? smooth((p - 0.55f) / 0.1f) : p < 0.9f ? 1.0f : 1.0f - smooth((p - 0.9f) / 0.1f);
}
static float aurora_at;
static void cycle_tick(float dt)
{
    int eff;
    float na;
    day_clock += dt;
    na = night_amt();
    eff = weather_mode == 0 ? (na > 0.5f ? 1 : 0) : weather_mode - 1;
    if (eff != weather) {
        weather = eff;
        setup_music(0, 0, 1);
        if (weather_mode == 0 && scene == SC_OUT) banner(eff ? "Night falls" : "The day breaks");
    }
    /* at night the aurora sings, softly, a note every two bars */
    if (scene == SC_OUT && na > 0.7f && music_t > aurora_at) {
        int k = chord_tones[(int)(frand() * (float)n_tones * 0.999f)];
        aurora_at = music_t + 8.0f * beat();
        world_pad((float)(84 + k - 12), 0.045f * na);
    }
}

/* ------------------------------------------------------------------ moving about --- */
static int inside_solid(float px, float py)
{
    if (py > 206.0f) return !(fm_fabsf(px - 120.0f) < 12.0f);
    if (hyp(px - 120.0f, py - 128.0f) > 82.0f && hyp(px - 50.0f, py - 84.0f) > 14.0f) return 1;
    if (px > 68 && px < 172 && py > 40 && py < 80) return 1;       /* the aquarium, against the back wall */
    if (px > 86 && px < 154 && py > 114 && py < 148) return 1;     /* the synth, in the middle */
    if (px > 40 && px < 66 && py > 110 && py < 138) return 1;      /* the drum machine's table */
    if (px > 170 && px < 200 && py > 114 && py < 144) return 1;    /* the radio */
    if (px > 56 && px < 90 && py > 158 && py < 188) return 1;      /* the wardrobe */
    if (px > 152 && px < 186 && py > 158 && py < 186) return 1;    /* the journal desk */
    if (hyp(px - 50.0f, py - 84.0f) <= 14.0f && px < 44.0f) return 1;   /* the arch (the trophy room: later) */
    return 0;
}
static int free_at(float px, float py)
{
    static const float OX[4] = {-5, 5, -5, 5}, OY[4] = {2, 2, 7, 7};
    int i;
    for (i = 0; i < 4; i++) {
        float x = px + OX[i], y = py + OY[i];
        if (scene == SC_OUT) { if (x < 0.0f || y < 0.0f || solid(at((int)(x / T), (int)(y / T)))) return 0; }
        else if (inside_solid(x, y)) return 0;
    }
    return 1;
}

static float cam_x, cam_y;
static uint32_t still_since;
static int tuto_shown = 1;               /* the arrows' help, until the first steps */
static float tuto_x, tuto_y;

static void enter_scene(int s)
{
    int prev = scene;
    if (s == scene) return;
    if (scene == SC_OUT) { out_x = P.x; out_y = P.y + 10.0f; }
    scene = s;
    if (s == SC_OUT) { P.x = out_x; P.y = out_y; P.dir = D_DOWN; }
    else { P.x = 120.0f; P.y = 205.0f; P.dir = D_UP; }
    if (s == SC_IGLOO && prev == SC_OUT) {
        synth_on = 0;
        save_soon();                     /* home keeps the walk */
    }
    held = -1; latched = -1; resume_t = 0.0f;
    banner(s == SC_IGLOO ? "Home" : "The ice field");
    if (DOG.found) {
        if (s != SC_OUT) { DOG.x = P.x - 14.0f; DOG.y = P.y + 4.0f; }
        else { DOG.x = P.x; DOG.y = P.y + 16.0f; }
    }
    trail_n = 0;
    ctx_place = -1;
    near_w = s == SC_OUT ? 0.0f : 1.0f;
    setup_music(0.1f, 380u, 0);
}

static void go_home(void)
{
    if (scene == SC_IGLOO) { banner("You're already home"); return; }
    enter_scene(SC_IGLOO);
    out_x = 48.0f * T + 8.0f; out_y = 54.0f * T + 2.0f;
    world_bell(79, 0.05f, 0.8f); world_bell(76, 0.05f, 0.8f); world_bell(72, 0.05f, 0.8f);
}

/* ------------------------------------------------------------------ the igloo's spots --- */
typedef struct { int16_t x0, y0, x1, y1; const char *hint; int id; } spot_t;
enum { SP_SYNTH, SP_DRUMS, SP_FISH, SP_RADIO, SP_WARDROBE, SP_JOURNAL, SP_ARCH };
static const spot_t SPOTS[6] = {
    {88, 116, 152, 146, "LFO: synth ON / OFF  SCL: grid", SP_SYNTH},
    {42, 112, 64, 136, "Something is missing here...", SP_DRUMS},
    {70, 42, 170, 78, "The tank is empty: go fishing!", SP_FISH},
    {170, 114, 200, 144, "The radio (soon)", SP_RADIO},
    {58, 160, 90, 188, "The wardrobe (soon)", SP_WARDROBE},
    {154, 160, 184, 184, "The journal (soon)", SP_JOURNAL},
};
static const spot_t *igloo_spot(void)
{
    const spot_t *b = 0;
    float bd = 15.0f, px = P.x, py = P.y + 4.0f;
    int i;
    for (i = 0; i < 6; i++) {
        const spot_t *sp = &SPOTS[i];
        float dx = fm_maxf(fm_maxf((float)sp->x0 - px, 0.0f), px - (float)sp->x1);
        float dy = fm_maxf(fm_maxf((float)sp->y0 - py, 0.0f), py - (float)sp->y1);
        float d = hyp(dx, dy);
        if (d < bd) { bd = d; b = sp; }
    }
    return b;
}

static void toggle_synth(void)
{
    synth_on = !synth_on;
    if (!synth_on) { latched = -1; eng_stop(); banner("Synth off"); }
    else { banner("Synth on"); latched = last_home_chord; play_idx(latched, 0.2f); }
}

static int near_hole(void) { return scene == SC_OUT && hyp(P.x / T - PLACES[PL_TROU].cx, P.y / T - PLACES[PL_TROU].cy) < 6.2f; }
static int near_fire(void) { return scene == SC_OUT && hyp(P.x / T - FIRE_X - 0.5f, P.y / T - FIRE_Y - 0.5f) < 3.6f; }

static void action(void)
{
    if (scene == SC_IGLOO) {
        const spot_t *sp = igloo_spot();
        if (!sp) return;
        if (sp->id == SP_SYNTH) toggle_synth();
        else banner(sp->hint);
        return;
    }
    if (near_hole()) { banner("Fishing comes in the next update"); return; }
    if (near_fire()) { banner("The fire comes in the next update"); return; }
}

static const char *hint(void)
{
    if (scene == SC_IGLOO) { const spot_t *sp = igloo_spot(); return sp ? sp->hint : ""; }
    if (hyp(P.x / T - PIT_X - 0.5f, P.y / T - PIT_Y - 0.5f) < 3.0f || hyp(P.x / T - PIT2_X - 0.5f, P.y / T - PIT2_Y - 0.5f) < 3.0f)
        return "A tiny little hole...";
    if (near_place == PL_IGLOO && near_any_w > 0.5f) return "EDIT: go inside";
    if (ctx_place >= 0) return "Keys: play  SCL: grid  PRESET: sound";
    return "";
}

/* ------------------------------------------------------------------ the keys --- */
static void press(int i)
{
    if (scene == SC_IGLOO) {
        if (!synth_on) { banner("The synth is off (LFO on it)"); return; }
        if (latched == i) { latched = -1; eng_stop(); return; }   /* the same chord again: let it ring out */
        latched = i; last_home_chord = i;
        play_idx(i, 0.25f);
        return;
    }
    if (near_place == PL_IGLOO && near_any_w > 0.3f && ctx_place < 0) { banner("Go into the igloo to play"); return; }
    if (ctx_place < 0) { banner("The synth plays near places"); return; }
    held = i;
    play_idx(i, 0.2f);
    user_here = 1;
    eng_target();
}
static void release(int i)
{
    if (scene == SC_IGLOO || held != i) return;
    held = -1;
    resume_t = music_t + 4.0f * beat();
    next_chord_t = resume_t;
    eng_target();
}

static void step_grid(void)
{
    char t[24];
    if (scene == SC_IGLOO) igloo_grid = (igloo_grid + 1) % 10;
    else if (ctx_place >= 0) place_grid[ctx_place] = (uint8_t)(((place_grid[ctx_place] != 255 ? place_grid[ctx_place] : grid_idx(PLACES[ctx_place].grid)) + 1) % NGRID);
    else { banner("Walk up to a place"); return; }
    set_grid();
    nice(GRID_DEF[st.grid].name, t);
    banner(t);
    save_soon();
    if (latched >= 0) play_idx(latched, 0.3f);
    else if (held >= 0) play_idx(held, 0.3f);
    else if (scene == SC_OUT) play_idx(PROG[prog_idx], 0.8f);
}
static void step_sky(int d)
{
    weather_mode = (weather_mode + d + 4) % 4;
    banner(MODES[weather_mode]);
    cycle_tick(0.0f);
    save_soon();
}
static void step_preset(int d)
{
    char t[24];
    if (scene == SC_IGLOO) {
        igloo_sound = (igloo_sound + d + 10) % 10;
        nice(IGLOO_BANK[igloo_sound], t); banner(t);
        save_soon();
        setup_music(0.05f, 160u, 0);
        return;
    }
    if (ctx_place < 0) { banner("Walk up to a place"); return; }
    variant[ctx_place] = (uint8_t)((variant[ctx_place] + d + 5) % 5);
    nice(PLACES[ctx_place].var[variant[ctx_place]], t); banner(t);
    setup_music(0.25f, 600u, 0);
}
static void knob(int k, int32_t e)
{
    static const char *const NAME[4] = {"Wind", "Drone", "Steps", "Synth"};
    char t[24], n[8];
    kn[k] = clamp01(kn[k] + (float)e * 0.05f);
    kn[k] = (float)(int32_t)(kn[k] * 20.0f + 0.5f) * 0.05f;
    t[0] = 0; text_cat(t, NAME[k]); text_cat(t, " ");
    int_text((int32_t)(kn[k] * 100.0f + 0.5f), n); text_cat(t, n); text_cat(t, "%");
    banner(t);
    apply_levels();
    save_soon();
}

/* ------------------------------------------------------------------ input --- */
static uint32_t in_keys, in_btn;
static uint8_t dirs[4];                  /* up, down, left, right held */
static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k, lit_keys = 0, lit_btn = 0;
    int32_t e;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(in_keys & bit)) press((int)k);
        else if (!(keys & bit) && (in_keys & bit)) release((int)k);
    }
    dirs[0] = (btn >> B_EDIT) & 1u;
    dirs[1] = (btn >> B_PLAY) & 1u;
    dirs[2] = (btn >> B_SEQ) & 1u;
    dirs[3] = (btn >> B_REC) & 1u;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (in_btn & bit)) continue;
        switch (k) {
        case B_LFO: action(); break;
        case B_SEL: step_grid(); break;
        case B_HOME: go_home(); break;
        default: break;
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) step_sky(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_PRESET)) != 0) step_preset(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_SELECT)) != 0) step_preset(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K1)) != 0) knob(0, e);
    if ((e = plat_enc(EN_K2)) != 0) knob(1, e);
    if ((e = plat_enc(EN_K3)) != 0) knob(2, e);
    if ((e = plat_enc(EN_K4)) != 0) knob(3, e);
    in_keys = keys;
    in_btn = btn;
    /* lights: the four arrows dim, LFO lit when there is something to do, the chord's key */
    if (latched >= 0) lit_keys |= 1u << (uint32_t)latched;
    if (held >= 0) lit_keys |= 1u << (uint32_t)held;
    if (hint()[0]) lit_btn |= 1u << B_LFO;
    if (synth_on && scene == SC_IGLOO) lit_btn |= LED_PLAY_GREEN;
    plat_lights(lit_btn, lit_keys, (1u << B_EDIT) | (1u << B_SEQ) | (1u << B_REC) | (1u << B_PLAY) | (1u << B_LFO), (1u << NKEYS) - 1u, 1);
}
void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ the update --- */
static uint32_t last_ms, last_steps, last_ticks;
static float idle_since_t, idle_chime_t;
static int idle;

static void dog_follow(int behind)
{
    float tx, ty;
    if (trail_n <= behind) return;
    tx = trail_x[trail_n - 1 - behind]; ty = trail_y[trail_n - 1 - behind];
    if (hyp(tx - DOG.x, ty - DOG.y) > 2.0f) {
        DOG.dir = tx > DOG.x ? D_RIGHT : D_LEFT;
        DOG.x += (tx - DOG.x) * 0.12f;
        DOG.y += (ty - DOG.y) * 0.12f;
    }
}

static void steps_tick(void)
{
    uint32_t s = world_steps(), tk = world_ticks(), now = plat_ms();
    /* each step the audio played: a print in the snow, and sometimes a chime */
    while (last_steps != s) {
        last_steps++;
        P.step_anim++;
        if (scene == SC_OUT) {
            float note = 0.0f;
            int i = print_head;
            if (near_w < 0.3f && frand() < 0.3f) { note = chime_note(); world_bell(note, 0.07f * (1.0f - near_w / 0.3f), 2.2f); }
            prints[i].x = P.x + ((P.step_anim & 1u) ? 3.0f : -3.0f);
            prints[i].y = P.y + 6.0f;
            prints[i].t = now; prints[i].lit = 0; prints[i].note = note;
            print_head = (print_head + 1) % PRINT_N;
            if (print_count < PRINT_N) print_count++;
            if (log_n == LOG_N) { memmove(step_log, step_log + 1, sizeof(int) * (LOG_N - 1)); log_n--; }
            step_log[log_n++] = i;
        }
    }
    /* standing still for a bar: your last steps' notes come back, softly, in a loop */
    while (last_ticks != tk) {
        last_ticks++;
        if (scene == SC_OUT && !P.moving && log_n >= 4 && music_t - idle_since_t > 4.0f * beat()) {
            int pi = step_log[echo_idx % log_n];
            if (prints[pi].note > 0.0f) { world_bell(prints[pi].note, 0.045f * (1.0f - near_w), 1.2f); prints[pi].lit = now; }
            echo_idx++;
        }
    }
}

static void update(void)
{
    uint32_t now = plat_ms();
    float dt = (float)(now - last_ms) * 0.001f, sp, ox, oy;
    int dx, dy, i;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    play_time += dt;
    music_t += dt;
    dx = (dirs[3] ? 1 : 0) - (dirs[2] ? 1 : 0);
    dy = (dirs[1] ? 1 : 0) - (dirs[0] ? 1 : 0);
    P.moving = (uint8_t)(dx || dy);
    if (P.moving) {
        sp = 66.0f * dt * (scene == SC_OUT && WEATHER[weather].wind > 0.8f ? 0.95f / 1.1f : 1.0f) / (dx && dy ? 1.41421356f : 1.0f);
        ox = P.x; oy = P.y;
        if (dx) {
            if (free_at(P.x + (float)dx * sp, P.y)) P.x += (float)dx * sp;
            else if (!dy) { int o; for (o = -1; o <= 1; o += 2) if (free_at(P.x + (float)dx * sp, P.y + (float)o * 4.0f) && free_at(P.x, P.y + (float)o)) { P.y += (float)o; break; } }
        }
        if (dy) {
            if (free_at(P.x, P.y + (float)dy * sp)) P.y += (float)dy * sp;
            else if (!dx) { int o; for (o = -1; o <= 1; o += 2) if (free_at(P.x + (float)o * 4.0f, P.y + (float)dy * sp) && free_at(P.x + (float)o, P.y)) { P.x += (float)o; break; } }
        }
        (void)ox; (void)oy;
        P.dir = dx && !dy ? (dx > 0 ? D_RIGHT : D_LEFT) : (dy > 0 ? D_DOWN : dy < 0 ? D_UP : P.dir);
        trail_push(P.x, P.y);
        if (tuto_shown && hyp(P.x - tuto_x, P.y - tuto_y) > 24.0f) tuto_shown = 0;
    }
    if (P.moving) { still_since = now; if (idle) { idle = 0; echo_idx = 0; } }
    else if (!idle) { idle = 1; idle_since_t = music_t; }
    /* doors */
    if (scene == SC_OUT) {
        int tx = (int)(P.x / T), ty = (int)((P.y + 2.0f) / T);
        for (i = 0; i < NPLACE; i++) {
            const place_t *p = &PLACES[i];
            if (p->door_x < 0 || dy >= 0) continue;
            if ((tx > p->door_x ? tx - p->door_x : p->door_x - tx) <= (i == PL_IGLOO ? 1 : 0) && ty == p->door_y) {
                if (i == PL_IGLOO) { enter_scene(SC_IGLOO); return; }
                if (now > ban.until) banner("Frozen shut. Next update!");
            }
        }
    } else if (P.y > 224.0f && dy > 0) { enter_scene(SC_OUT); return; }
    /* the cold: outside it creeps in, home warms you up */
    {
        float rate = near_fire() ? -1.0f / 15.0f : scene == SC_OUT ? (WEATHER[weather].wind > 0.8f ? 1.0f / 90.0f : 1.0f / 220.0f) : -1.0f / 8.0f;
        cold = clamp01(cold + rate * dt);
        WP.cold = cold;
    }
    if (scene == SC_OUT) {
        int best = -1, best_m = -1, next_ctx;
        float bw = 0.0f, bwm = 0.0f;
        reveal(P.x, P.y);
        for (i = 0; i < NPLACE; i++) {
            const place_t *p = &PLACES[i];
            float d = hyp(P.x / T - p->cx, P.y / T - (p->cy + 1.0f)), w = clamp01((p->r - d) / 5.0f);
            if (w > bw) { bw = w; best = i; }
            if (i != PL_IGLOO && w > bwm) { bwm = w; best_m = i; }   /* home only makes music once you're inside */
            if (d < p->r - 1.0f && !(discovered & (1u << i))) { discovered |= (uint16_t)(1u << i); banner(p->name); save_soon(); }
        }
        near_place = best; near_any_w = bw; near_w = bwm;
        next_ctx = near_w > 0.5f ? best_m : near_w < 0.3f ? -1 : ctx_place;
        if (next_ctx != ctx_place) {
            ctx_place = next_ctx; user_here = 0;
            if (ctx_place >= 0 && DOG.found) world_woof();
            setup_music(0, 0, 0);
        }
        if (!DOG.found && hyp(P.x - DOG.x, P.y - DOG.y) < 30.0f) { DOG.found = 1; banner("Seito! He follows Obsimo"); world_woof(); save_soon(); }
        if (DOG.found) dog_follow(18);
    } else if (DOG.found) dog_follow(14);
    WP.dog = (uint8_t)(DOG.found && scene == SC_OUT);
    WP.moving = P.moving;
    WP.ground = scene == SC_IGLOO ? GROUND_FUR : at((int)(P.x / T), (int)(P.y / T)) == ICE ? GROUND_ICE : GROUND_SNOW;
    steps_tick();
    /* the chimes while you stand: a note now and then */
    if (scene == SC_OUT && !P.moving && near_w <= 0.3f && music_t > idle_chime_t) {
        if (frand() < 0.35f && !(log_n >= 4 && idle && music_t - idle_since_t > 4.0f * beat())) world_bell(chime_note(), 0.05f, 2.6f);
        idle_chime_t = music_t + 1.6f + frand() * 2.0f;
    }
    cycle_tick(dt);
    /* the synth's fader, and the sound swap under it */
    if (swap_pending && now >= swap_at) { swap_now(); }
    eng_target();
    {
        float k = 1.0f - fm_expf(-dt / fm_maxf(0.01f, syn_tc));
        syn_level += (syn_target - syn_level) * k;
    }
    music_tick();
    /* the walk keeps itself: a few seconds after a change, never while the synth plays (a flash write stops the sound) */
    if (save_due && now >= save_due && syn_level < 0.02f && !P.moving) save_now();
}

/* ------------------------------------------------------------------ platform contract --- */
void app_init(void)
{
    int g;
    for (g = 0; g < NGRID; g++) {
        parse_chords(GRID_DEF[g].white, grid_w[g], 8);
        parse_chords(GRID_DEF[g].black, grid_b[g], 5);
    }
    build_map();
    memset(explored, 0, sizeof explored);
    memset(place_grid, 255, sizeof place_grid);
    P.x = 48.0f * T + 8.0f; P.y = 53.6f * T + 8.0f; P.dir = D_UP;
    DOG.x = 82.0f * T + 8.0f; DOG.y = 46.0f * T + 8.0f; DOG.dir = D_LEFT;
    if (load_save()) tuto_shown = 0;
    tuto_x = P.x; tuto_y = P.y;
    scene = SC_OUT;
    world_init();
    eng_init();
    weather = weather_mode == 0 ? (night_amt() > 0.5f ? 1 : 0) : weather_mode - 1;
    set_grid();
    send_sound();
    send_hold();
    setup_music(0.1f, 400u, 0);
    reveal(P.x, P.y);
}

static float wdry[2 * APP_BLOCK], wsend[APP_BLOCK];
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    if (n > APP_BLOCK) n = APP_BLOCK;
    world_render(wdry, wsend, n);
    eng_world(wdry, wsend, syn_level * kn[3] * 1.33f);
    eng_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_panic(void) { eng_panic(); }

/* ------------------------------------------------------------------ drawing: colours --- */
#define C_SNOW1 RGB(233, 241, 248)
#define C_SNOW2 RGB(226, 235, 243)
#define C_INK RGB(234, 242, 250)
#define C_DARK RGB(8, 14, 22)
#define C_GOLD RGB(255, 207, 138)
#define C_SKY RGB(143, 216, 255)

static float pulse;                      /* the music's level, for the glows */
static uint32_t frame_ms;
static float tsec(void) { return (float)frame_ms * 0.001f; }

/* ------------------------------------------------------------------ drawing: the tiles --- */
static void tile(int t, int x, int y, int32_t sx, int32_t sy, float time)
{
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    switch (t) {
    case ICE:
        box(sx, sy, T, T, ((x + y) & 1) ? RGB(191, 224, 242) : RGB(184, 218, 238));
        if (h % 3u == 0u) box_a(sx + (int32_t)(h % 10u), sy + (int32_t)(h % 8u) + 2, 5, 1, RGB(255, 255, 255), 18);
        if (h % 7u == 0u) box(sx + 2, sy + (int32_t)(h % 12u), 10, 1, RGB(156, 200, 226));
        break;
    case RIDGE:
        box(sx, sy, T, T, C_SNOW2);
        box(sx + 1, sy + 4, 14, 11, RGB(198, 216, 232));
        box(sx + 2, sy + 3, 9, 4, RGB(247, 251, 255));
        box(sx + 1, sy + 13, 14, 2, RGB(169, 195, 217));
        break;
    case SEA: {
        float w = fm_sinf(time * 1.5f + (float)x * 0.8f + (float)y) * 3.0f;
        box(sx, sy, T, T, ((x + y) & 1) ? RGB(29, 74, 112) : RGB(26, 68, 102));
        box_a(sx + 4 + (int32_t)w, sy + 6 + (int32_t)(h % 5u), 6, 1, RGB(140, 200, 240), 11);
        if (h % 5u == 0u) box(sx + (int32_t)(h % 9u), sy + (int32_t)(h % 7u), 4, 3, C_SNOW1);
        break;
    }
    case PITT:
        box(sx, sy, T, T, C_SNOW2);
        oval((float)sx + 8.0f, (float)sy + 9.0f, 7.0f, 5.0f, RGB(195, 212, 227), 32, 0);
        oval((float)sx + 8.0f, (float)sy + 9.0f, 5.0f, 3.5f, RGB(11, 20, 32), 32, 0);
        line_a(sx + 14, sy + 9, sx + 17, sy + 6, RGB(120, 150, 180), 19);
        line_a(sx + 2, sy + 10, sx - 2, sy + 13, RGB(120, 150, 180), 19);
        break;
    default:
        box(sx, sy, T, T, ((x + y) & 1) ? C_SNOW1 : C_SNOW2);
        if (h % 4u == 0u) box(sx + (int32_t)(h % 12u), sy + (int32_t)(h % 9u) + 3, 3, 1, RGB(211, 224, 236));
        if (h % 9u == 0u) dot(sx + (int32_t)(h % 13u), sy + (int32_t)(h % 11u), RGB(255, 255, 255));
        break;
    }
}

/* ------------------------------------------------------------------ drawing: the sprites --- */
static const char *const SPR_DOWN[16] = {"....HHHH....", "...HHHHHH...", "..HHHHHHHH..", "..HHHHHHHH..", "..HSSSSSSH..", "..SESSSSES..",
    "..SSSSSSSS..", "...SSssSS...", "..GGGGGGGG..", ".SGGGGGGGGS.", ".SGGGGGGGGS.", "..gGGGGGGg..", "..BBBBBBBB..", "..BBB..BBB..",
    "..BB....BB..", "..KK....KK.."};
static const char *const SPR_UP[16] = {"....HHHH....", "...HHHHHH...", "..HHHHHHHH..", "..HHHHHHHH..", "..HHHHHHHH..", "..hHHHHHHh..",
    "..SSSSSSSS..", "...SSSSSS...", "..GGGGGGGG..", ".SGGGGGGGGS.", ".SGGGGGGGGS.", "..gGGGGGGg..", "..BBBBBBBB..", "..BBB..BBB..",
    "..BB....BB..", "..KK....KK.."};
static const char *const SPR_SIDE[16] = {"....HHHH....", "...HHHHHH...", "..HHHHHHHH..", "..HHHHHHHH..", "..HHHSSSSH..", "..HHSSSESS..",
    "...HSSSSSS..", "....SSSss...", "...GGGGGG...", "...GGGGGGS..", "...GGGGGGS..", "...gGGGGg...", "...BBBBBB...", "...BBBBBB...",
    "...BB..BB...", "...KK..KK..."};
static const char *const DOG_ROWS[8] = {"..........CC", ".........CCN", "C.......CCCC", ".C..CCCCCCC.", "..CCCCCCCCC.", "..CcCCCCCc..",
    "..C.C..C.C..", "..c.c..c.c.."};
static const pal_t PAL[12] = {{'H', RGB(243, 211, 107)}, {'h', RGB(201, 166, 64)}, {'S', RGB(242, 198, 160)}, {'s', RGB(217, 164, 127)},
    {'E', RGB(31, 26, 36)}, {'G', RGB(63, 174, 90)}, {'g', RGB(47, 138, 71)}, {'B', RGB(43, 58, 103)}, {'K', RGB(27, 23, 32)},
    {'C', RGB(138, 90, 60)}, {'c', RGB(110, 70, 48)}, {'N', RGB(42, 29, 24)}};

static void draw_player(int32_t sx, int32_t sy)
{
    const char *rows[16];
    const char *const *src = P.dir == D_LEFT || P.dir == D_RIGHT ? SPR_SIDE : P.dir == D_UP ? SPR_UP : SPR_DOWN;
    int walk = P.moving && (P.step_anim & 1u), side = P.dir == D_LEFT || P.dir == D_RIGHT, shiver;
    int i;
    for (i = 0; i < 16; i++) rows[i] = src[i];
    if (walk) {
        rows[14] = side ? "...B....B..." : "...BB..BB...";
        rows[15] = side ? "...K....K..." : "...KK..KK...";
    }
    shiver = cold > 0.6f && ((frame_ms / 55u) & 1u) ? 1 : 0;
    box_a(sx - 5, sy + 7, 10, 2, RGB(40, 70, 100), 8);
    sprite(rows, 16, sx - 6 + shiver, sy - 9 - (walk ? 1 : 0), P.dir == D_LEFT, PAL, 12);
}

static void draw_dog(int32_t sx, int32_t sy, float time)
{
    const char *rows[8];
    int i;
    for (i = 0; i < 8; i++) rows[i] = DOG_ROWS[i];
    if ((int)(time * (DOG.found ? 8.0f : 3.0f)) & 1) rows[2] = ".C......CCCC";
    box_a(sx - 5, sy + 4, 11, 2, RGB(40, 70, 100), 8);
    sprite(rows, 8, sx - 6, sy - 4, DOG.dir == D_LEFT, PAL, 12);
}

/* ------------------------------------------------------------------ drawing: the monuments --- */
static void draw_tree(int32_t sx, int32_t sy)
{
    static const struct { int8_t dy, w, h; uint16_t c; } L[3] = {{0, 15, 7, RGB(44, 88, 71)}, {-6, 12, 7, RGB(51, 102, 80)}, {-11, 8, 6, RGB(60, 115, 90)}};
    int i;
    box_a(sx + 1, sy + 13, 14, 3, RGB(40, 70, 100), 7);
    box(sx + 7, sy + 10, 2, 5, RGB(90, 59, 42));
    for (i = 0; i < 3; i++) {
        float cx = (float)sx + 8.0f, by = (float)(sy + 11 + L[i].dy), w = (float)L[i].w, h = (float)L[i].h;
        tri(cx - w / 2.0f, by, cx + w / 2.0f, by, cx, by - h, L[i].c, 32);
        box((int32_t)(cx - w / 4.0f), (int32_t)(by - h + 2.0f), (int32_t)(w / 2.0f), 2, RGB(242, 248, 252));
    }
}

static void draw_hole(int32_t sx, int32_t sy, float time)
{
    int i;
    oval((float)sx, (float)sy, 60, 50, RGB(184, 208, 226), 32, 0);
    oval((float)sx, (float)sy, 54, 44, RGB(18, 58, 92), 32, 0);
    for (i = 0; i < 6; i++)
        box_a(sx - 36 + (i % 3) * 26 + (int32_t)(fm_sinf(time + (float)i) * 4.0f), sy - 20 + (i / 3) * 22, 10, 1, RGB(120, 190, 240), A32(0.25f + pulse * 0.3f));
}

static void draw_fire(int32_t sx, int32_t sy, float time)
{
    int i;
    for (i = 0; i < 7; i++) {
        float a = (float)i / 7.0f * 6.2831853f;
        box(sx + (int32_t)(fm_cosf(a) * 9.0f) - 2, sy + (int32_t)(fm_sinf(a) * 5.0f) - 1, 4, 3, (i & 1) ? RGB(139, 152, 163) : RGB(111, 124, 136));
    }
    box(sx - 7, sy - 1, 14, 3, RGB(90, 59, 42));
    box(sx - 2, sy - 4, 4, 8, RGB(74, 47, 34));
    {
        float h = 9.0f + fm_sinf(time * 11.0f) * 2.0f + fm_sinf(time * 17.0f) * 1.5f + pulse * 8.0f;
        static const struct { uint16_t c; float w, k; } F[3] = {{RGB(214, 69, 42), 7, 1}, {RGB(255, 138, 58), 5, 0.78f}, {RGB(255, 215, 106), 3, 0.5f}};
        for (i = 0; i < 3; i++) {
            float sway = fm_sinf(time * 6.0f + (float)i) * 1.5f;
            tri((float)sx - F[i].w, (float)sy, (float)sx + F[i].w, (float)sy, (float)sx + sway * 1.5f, (float)sy - h * F[i].k, F[i].c, 32);
        }
    }
}

static void draw_boat(int32_t sx, int32_t sy, float time)
{
    int i;
    /* the hull leans a little (the browser turns it by -0.1 rad): each column a touch higher to the right */
    for (i = -84; i < 84; i += 4) {
        int32_t lift = (int32_t)((float)i * 0.1f);
        box(sx + i, sy - 10 - lift, 4, 20, RGB(122, 47, 42));
        box(sx + i, sy + 10 - lift, 4, 6, RGB(91, 33, 30));
        box(sx + i, sy - 12 - lift, 4, 4, RGB(244, 248, 252));
    }
    box(sx - 20, sy - 38, 50, 28, RGB(233, 225, 212));
    for (i = 0; i < 3; i++) box(sx - 14 + i * 15, sy - 32, 9, 8, RGB(61, 90, 115));
    box(sx - 22, sy - 40, 54, 4, RGB(244, 248, 252));
    box(sx - 58, sy - 64, 4, 60, RGB(74, 58, 48));
    box(sx + 46, sy - 58, 3, 46, RGB(74, 58, 48));
    box(sx - 57 + (int32_t)(fm_sinf(time * 1.3f) * 4.0f), sy - 60, 1, 36, RGB(139, 122, 102));
    box(sx - 92, sy + 12, 184, 10, RGB(214, 228, 239));
    box(sx - 70, sy + 11, 50, 3, RGB(247, 251, 255));
    box(sx + 20, sy + 13, 44, 3, RGB(247, 251, 255));
    box(sx - 4, sy + 6, 2, 22, RGB(74, 58, 48));
    box(sx + 4, sy + 6, 2, 22, RGB(74, 58, 48));
    for (i = 10; i < 28; i += 5) box(sx - 4, sy + i, 10, 1, RGB(74, 58, 48));
}

static void draw_igloo_out(int32_t sx, int32_t sy, int night)
{
    int r;
    oval((float)sx, (float)sy + 12.0f, 42, 34, RGB(200, 217, 231), 32, 1);
    oval((float)sx, (float)sy + 10.0f, 39, 31, RGB(242, 247, 251), 32, 1);
    for (r = -24; r <= 6; r += 7) {                      /* the rows of blocks, inside the dome */
        float y = (float)(10 + r), d = y / 31.0f - 10.0f / 31.0f, w;
        d = ((float)r) / 31.0f;
        w = 39.0f * fm_sqrtf(fm_maxf(0.0f, 1.0f - d * d));
        span(sx - (int32_t)w + 1, sx + (int32_t)w, sy + 10 + r, RGB(195, 212, 227), 32);
        (void)y;
    }
    box(sx - 10, sy + 2, 20, 12, RGB(230, 238, 245));
    oval((float)sx, (float)sy + 14.0f, 7, 10, night ? RGB(255, 190, 110) : RGB(43, 58, 76), night ? A32(0.85f + pulse * 0.15f) : 32, 1);
    box_a(sx - 9, sy + 14, 18, night ? 18 : 4, RGB(255, 190, 110), 6);
}

static void draw_cave_mound(int32_t sx, int32_t sy)
{
    int i;
    oval((float)sx, (float)sy + 8.0f, 56, 40, RGB(156, 196, 220), 32, 1);
    oval((float)sx - 6.0f, (float)sy + 4.0f, 46, 30, RGB(196, 226, 242), 32, 1);
    oval((float)sx, (float)sy + 8.0f, 11, 15, RGB(13, 29, 44), 32, 1);
    for (i = 0; i < 6; i++) box(sx - 40 + i * 14, sy - 24 + (i % 2) * 6, 3, 8, RGB(233, 247, 255));
}

static float phare_ang;
static void draw_phare(int32_t sx, int32_t sy)
{
    int i;
    oval((float)sx, (float)sy + 2.0f, 20, 8, RGB(125, 138, 150), 32, 0);
    oval((float)sx - 3.0f, (float)sy, 15, 6, RGB(167, 179, 190), 32, 0);
    for (i = 0; i < 6; i++) {
        int32_t y = sy - 6 - i * 7;
        float w = 12.0f - (float)i * 0.8f;
        box(sx - (int32_t)(w / 2.0f), y - 7, (int32_t)w, 7, (i & 1) ? RGB(244, 241, 234) : RGB(200, 70, 58));
    }
    box(sx - 8, sy - 50, 16, 2, RGB(42, 36, 48));
    glow(sx, sy - 54, 18, RGB(255, 236, 160), 0.2f);
    box_a(sx - 5, sy - 58, 10, 8, RGB(255, 232, 150), 8);
    box(sx - 6, sy - 61, 12, 3, RGB(42, 36, 48));
    box(sx - 1, sy - 64, 2, 3, RGB(42, 36, 48));
}
/* the lantern's two beams, sweeping (slowly: the lighthouse waits to be played, next update) */
static void draw_beams(int32_t lx, int32_t ly, int night)
{
    int b;
    float alpha = 0.1f * (night ? 1.5f : 0.85f);
    uint16_t col = night ? RGB(255, 240, 180) : RGB(255, 196, 90);
    if (lx < -380 || lx > 620 || ly < -380 || ly > 620) return;
    for (b = 0; b < 2; b++) {
        float a = phare_ang + (b ? 3.14159265f : 0.0f), L = 340.0f;
        int seg;
        /* the beam fades with distance: four lengths of a narrowing alpha */
        for (seg = 0; seg < 4; seg++) {
            float r0 = L * (float)seg / 4.0f, r1 = L * (float)(seg + 1) / 4.0f, w = 0.14f, k = 1.0f - (float)seg / 4.0f;
            tri((float)lx + fm_cosf(a - w) * r0, (float)ly + fm_sinf(a - w) * r0 * 0.6f, (float)lx + fm_cosf(a - w) * r1, (float)ly + fm_sinf(a - w) * r1 * 0.6f,
                (float)lx + fm_cosf(a + w) * r1, (float)ly + fm_sinf(a + w) * r1 * 0.6f, col, A32(alpha * k));
            tri((float)lx + fm_cosf(a - w) * r0, (float)ly + fm_sinf(a - w) * r0 * 0.6f, (float)lx + fm_cosf(a + w) * r0, (float)ly + fm_sinf(a + w) * r0 * 0.6f,
                (float)lx + fm_cosf(a + w) * r1, (float)ly + fm_sinf(a + w) * r1 * 0.6f, col, A32(alpha * k));
        }
    }
}

static void draw_rink_sign(int32_t cx, int32_t cy)
{
    float rx = 7.1f * T, ry = 4.2f * T;
    int i;
    for (i = 0; i < 4; i++) oring((float)cx, (float)cy, rx + 2.5f + (float)i, ry + 2.5f + (float)i, RGB(247, 251, 255), 32);
    oring((float)cx, (float)cy, rx + 7.0f, ry + 7.0f, RGB(160, 190, 215), 22);
    oring((float)cx, (float)cy, rx * 0.5f, ry * 0.5f, RGB(255, 255, 255), 11);
    box(cx - 1, cy - (int32_t)ry - 22, 2, 14, RGB(122, 82, 54));
    box(cx - 22, cy - (int32_t)ry - 30, 44, 10, RGB(233, 217, 184));
    cv_text(cx - text_w(&FONT_T, "ICE RINK") / 2, cy - (int32_t)ry - 31, &FONT_T, "ICE RINK", RGB(91, 70, 54));
}

/* ------------------------------------------------------------------ drawing: outside --- */
typedef struct { int32_t y; uint8_t kind; int16_t a, b; } obj_t;
enum { O_BOAT, O_IGLOO, O_CAVE, O_FIRE, O_PHARE, O_DOG, O_PLAYER, O_TREE };
#define NOBJ 160
static obj_t objs[NOBJ];
static int nobj;
static void obj_add(int32_t y, int kind, int a, int b)
{
    if (nobj < NOBJ) { objs[nobj].y = y; objs[nobj].kind = (uint8_t)kind; objs[nobj].a = (int16_t)a; objs[nobj].b = (int16_t)b; nobj++; }
}

static struct { float x, y, s; } flakes[90];
static struct { uint8_t x, y; float p; } stars[40];

static void draw_outside(float time)
{
    float na = weather_mode == 3 ? 0.0f : night_amt();
    int night = na > 0.5f, storm = weather == 2;
    int tx0, ty0, x, y, i;
    int32_t camx, camy;
    cam_x = fm_clampf(P.x - 120.0f, 0.0f, (float)(MW * T - 240));
    cam_y = fm_clampf(P.y - 120.0f, 0.0f, (float)(MH * T - 240));
    camx = (int32_t)cam_x; camy = (int32_t)cam_y;
    tx0 = camx / T; ty0 = camy / T;
    for (y = ty0; y <= ty0 + 15; y++)
        for (x = tx0; x <= tx0 + 15; x++) tile(at(x, y), x, y, x * T - camx, y * T - camy, time);
    /* the footprints, fading (faster in a storm); lit when the echo plays them */
    {
        uint32_t life = storm ? 9000u : 45000u;
        for (i = 0; i < print_count; i++) {
            int k = (print_head - 1 - i + PRINT_N) % PRINT_N;
            uint32_t age = frame_ms - prints[k].t;
            int lit;
            if (age >= life) continue;
            lit = prints[k].lit && frame_ms - prints[k].lit < 250u;
            if (lit) box((int32_t)prints[k].x - camx - 1, (int32_t)prints[k].y - camy, 4, 3, C_GOLD);
            else box_a((int32_t)prints[k].x - camx - 1, (int32_t)prints[k].y - camy, 3, 2, RGB(150, 175, 200), A32(0.55f * (1.0f - (float)age / (float)life)));
        }
    }
    draw_hole(30 * T + 8 - camx, 66 * T + 8 - camy, time);
    draw_rink_sign(52 * T + 8 - camx, 82 * T + 8 - camy);
    nobj = 0;
    obj_add(10 * T, O_BOAT, 0, 0);
    obj_add(52 * T, O_IGLOO, 0, 0);
    obj_add(72 * T, O_CAVE, 0, 0);
    obj_add((FIRE_Y + 1) * T, O_FIRE, 0, 0);
    obj_add(PHARE_Y, O_PHARE, 0, 0);
    obj_add((int32_t)DOG.y, O_DOG, 0, 0);
    obj_add((int32_t)P.y, O_PLAYER, 0, 0);
    for (y = ty0 - 1; y <= ty0 + 16; y++)
        for (x = tx0; x <= tx0 + 15; x++)
            if (at(x, y) == TREE) obj_add(y * T + 13, O_TREE, x, y);
    for (i = 1; i < nobj; i++) {               /* by depth */
        obj_t o = objs[i];
        int j = i;
        while (j > 0 && objs[j - 1].y > o.y) { objs[j] = objs[j - 1]; j--; }
        objs[j] = o;
    }
    for (i = 0; i < nobj; i++) {
        obj_t *o = &objs[i];
        switch (o->kind) {
        case O_BOAT: draw_boat(60 * T + 8 - camx, (int32_t)(8.5f * T) + 8 - camy, time); break;
        case O_IGLOO: draw_igloo_out(48 * T + 8 - camx, 50 * T + 8 - camy, night); break;
        case O_CAVE: draw_cave_mound(80 * T + 8 - camx, 70 * T + 8 - camy); break;
        case O_FIRE: draw_fire(FIRE_X * T + 8 - camx, FIRE_Y * T + 8 - camy, time); break;
        case O_PHARE: draw_phare(PHARE_X - camx, PHARE_Y - camy); break;
        case O_DOG: draw_dog((int32_t)DOG.x - camx, (int32_t)DOG.y - camy, time); break;
        case O_PLAYER: draw_player((int32_t)P.x - camx, (int32_t)P.y - camy); break;
        case O_TREE: draw_tree(o->a * T - camx, o->b * T - camy); break;
        default: break;
        }
    }
    /* the fog of the unexplored */
    for (y = ty0; y <= ty0 + 15; y++)
        for (x = tx0; x <= tx0 + 15; x++) {
            int edge = 0;
            if (x < 0 || y < 0 || x >= MW || y >= MH || explored[y * MW + x]) continue;
            if ((x + 1 < MW && explored[y * MW + x + 1]) || (x > 0 && explored[y * MW + x - 1]) || (y + 1 < MH && explored[(y + 1) * MW + x])
                || (y > 0 && explored[(y - 1) * MW + x])) edge = 1;
            if (night) box_a(x * T - camx, y * T - camy, T, T, RGB(14, 22, 44), edge ? 18 : 28);
            else box_a(x * T - camx, y * T - camy, T, T, edge ? RGB(240, 246, 251) : RGB(238, 244, 249), edge ? 19 : 32);
        }
    /* the night: a dark veil, the dusk's orange, the stars, the aurora */
    if (na > 0.02f) {
        box_a(0, 0, SW, SH, RGB(6, 12, 38), A32(0.55f * na));
        if (na > 0.5f && na < 0.95f) box_a(0, 0, SW, SH, RGB(255, 140, 90), A32(0.12f * (1.0f - fm_fabsf(na - 0.7f) * 4.0f)));
        for (i = 0; i < 40; i++) dot_a(stars[i].x, (int32_t)((float)stars[i].y * 0.4f), RGB(255, 255, 255), A32((0.3f + 0.5f * fm_fabsf(fm_sinf(time + stars[i].p))) * na));
        {
            int band;
            for (band = 0; band < 3; band++)
                for (x = 0; x < SW; x += 2) {
                    float yy = 18.0f + (float)band * 14.0f + fm_sinf((float)x * 0.03f + time * 0.4f + (float)band) * 10.0f + fm_sinf((float)x * 0.011f - time * 0.2f) * 8.0f;
                    int32_t hh = 16 + (int32_t)(fm_sinf((float)x * 0.05f + time) * 6.0f);
                    if (band == 1) box_a(x, (int32_t)yy, 2, hh, RGB(255, 120, 200), A32((0.09f + pulse * 0.12f) * na));
                    else box_a(x, (int32_t)yy, 2, hh, RGB(90, 255, 170), A32((0.13f + pulse * 0.16f) * na));
                }
        }
    }
    if (storm) {
        box_a(0, 0, SW, SH, RGB(225, 235, 245), 11);
        for (i = 0; i < 90; i++) {
            flakes[i].x -= 3.2f * flakes[i].s; flakes[i].y += 1.1f * flakes[i].s;
            if (flakes[i].x < 0.0f) flakes[i].x += 240.0f;
            if (flakes[i].y > 240.0f) flakes[i].y -= 240.0f;
            box_a((int32_t)flakes[i].x, (int32_t)flakes[i].y, 2 + (int32_t)(flakes[i].s * 2.0f), 1, RGB(255, 255, 255), 27);
        }
    } else {
        for (i = 0; i < 25; i++) {
            flakes[i].y += 0.25f * flakes[i].s; flakes[i].x += fm_sinf(time + flakes[i].s * 9.0f) * 0.2f;
            if (flakes[i].y > 240.0f) flakes[i].y -= 240.0f;
            dot_a((int32_t)flakes[i].x, (int32_t)flakes[i].y, RGB(255, 255, 255), 29);
        }
    }
    {   /* the campfire's glow */
        int32_t fx = FIRE_X * T + 8 - camx, fy = FIRE_Y * T + 8 - camy;
        if (fx > -80 && fx < 320 && fy > -80 && fy < 320) {
            glow(fx, fy - 4, 46 + (int32_t)(pulse * 10.0f + fm_sinf(time * 9.0f) * 2.0f), RGB(255, 170, 80), night ? 0.45f : 0.22f);
            if (night) draw_fire(fx, fy, time);
        }
    }
    /* the halo: the world fades at the edges, more in a storm and when cold */
    {
        float inner = storm ? 50.0f : 92.0f - cold * 25.0f, amax = 0.62f + cold * 0.25f + na * 0.15f;
        uint16_t hc = RGB((int)(236.0f + (8.0f - 236.0f) * na), (int)(244.0f + (14.0f - 244.0f) * na), (int)(250.0f + (34.0f - 250.0f) * na));
        uint8_t lut[256];
        int32_t d;
        for (d = 0; d < 256; d++) lut[d] = (uint8_t)A32(amax * clamp01(((float)d - inner) / (172.0f - inner)));
        for (y = 0; y < SH; y++) {
            int32_t dy = y - 120;
            for (x = 0; x < SW; x++) {
                uint32_t a = lut[dist_at(x - 120, dy)];
                if (a) { uint16_t *p = &cv_px[y * SW + x]; *p = swap16(blend565(swap16(*p), hc, a)); }
            }
        }
    }
    draw_beams(PHARE_X - camx, PHARE_Y - camy - 54, night);
}

/* ------------------------------------------------------------------ drawing: home --- */
static void draw_igloo_in(float time)
{
    int lit = latched >= 0 && synth_on, i;
    const int CX = 120, CY = 128;
    int32_t x, y;
    box(0, 0, SW, SH, RGB(23, 20, 28));
    oval(CX, CY, 101, 101, RGB(185, 203, 218), 32, 0);
    oval(CX, CY, 94, 94, RGB(213, 226, 236), 32, 0);
    for (i = 0; i < 28; i++) {
        float a = (float)i / 28.0f * 6.2831853f + (float)(i % 2) * 0.05f;
        line_a(CX + (int32_t)(fm_cosf(a) * 88.0f), CY + (int32_t)(fm_sinf(a) * 88.0f), CX + (int32_t)(fm_cosf(a) * 100.0f), CY + (int32_t)(fm_sinf(a) * 100.0f), RGB(140, 165, 188), 22);
    }
    /* the floor: planks, inside the room, warm in the middle */
    for (y = CY - 87; y <= CY + 87; y++) {
        int32_t k = (y - 40) / 9, w;
        float d = (float)(y - CY) / 87.0f;
        uint16_t c = (k & 1) ? RGB(167, 125, 92) : RGB(176, 133, 98);
        if (y < 40 || d * d > 1.0f) continue;
        w = (int32_t)(87.0f * fm_sqrtf(1.0f - d * d));
        span(CX - w, CX + w, y, c, 32);
        if (((y - 40) % 9) == 8) span(CX - w, CX + w, y, RGB(70, 45, 30), 11);
        else { int32_t sx = 40 + (k * 37) % 140; if (sx > CX - w && sx < CX + w) dot_a(sx, y, RGB(70, 45, 30), 11); }
    }
    for (y = 40; y < 220; y++)
        for (x = 30; x < 210; x++) {
            int32_t d = dist_at(x - 120, y - 132), w2 = (x - CX) * (x - CX) + (y - CY) * (y - CY);
            if (w2 > 87 * 87) continue;
            if (d < 100) {
                float k = (float)d / 100.0f;
                uint16_t *p = &cv_px[y * SW + x];
                /* warm light near the middle, dusk at the walls */
                if (k < 0.55f) *p = swap16(blend565(swap16(*p), RGB(255, 205, 140), A32((0.28f + pulse * 0.2f) * (1.0f - k / 0.55f))));
                else *p = swap16(blend565(swap16(*p), RGB(20, 10, 20), A32(0.35f * (k - 0.55f) / 0.45f)));
            }
        }
    oring(CX, CY, 87.5f, 87.5f, RGB(60, 40, 30), 16);
    /* the aquarium against the back wall: empty for now */
    box_a(72, 44, 102, 40, RGB(30, 20, 20), 10);
    box(68, 40, 104, 40, RGB(106, 74, 56)); box(68, 40, 104, 3, RGB(125, 90, 69));
    box(71, 43, 98, 32, RGB(230, 242, 250));
    for (y = 45; y < 73; y++) {
        float k = (float)(y - 45) / 28.0f;
        span(73, 167, y, RGB((int)(134.0f + (47.0f - 134.0f) * k), (int)(205.0f + (111.0f - 205.0f) * k), (int)(239.0f + (158.0f - 239.0f) * k)), 32);
    }
    box(73, 68, 94, 5, RGB(217, 199, 154));
    box(84, 66, 7, 4, RGB(139, 152, 163)); box(140, 67, 6, 3, RGB(139, 152, 163));
    for (i = 0; i < 6; i++) {
        int32_t px = 78 + i * 16;
        float sw = fm_sinf(time * 0.8f + (float)i) * 2.5f;
        int32_t top = 54 + (i % 3) * 3, yy;
        for (yy = 70; yy > top; yy--) {
            float k = (float)(70 - yy) / (float)(70 - top);
            box(px + (int32_t)(sw * 1.4f * k), yy, 2, 1, (i & 1) ? RGB(60, 115, 90) : RGB(79, 143, 106));
        }
    }
    for (i = 0; i < 6; i++) box_a(73 + (int32_t)(fm_fmodf_pos((float)(i * 23) + time * 6.0f, 86.0f)), 47 + (i * 5) % 14, 8, 1, RGB(255, 255, 255), 4);
    box_a(75, 46, 20, 1, RGB(255, 255, 255), 18);
    cv_text(120 - text_w(&FONT_T, "GO FISHING!") / 2, 52, &FONT_T, "GO FISHING!", RGB(255, 255, 255));
    /* the rug, and the synth in the middle of the room: its keys light up on the chord */
    oval(120, 150, 40, 26, RGB(143, 59, 51), 32, 0);
    oring(120, 150, 34, 21, RGB(233, 217, 184), 32);
    box_a(91, 119, 64, 30, RGB(30, 20, 20), 10);
    box(88, 116, 64, 30, RGB(94, 66, 50)); box(88, 116, 64, 3, RGB(110, 78, 59));
    box(91, 120, 58, 23, RGB(42, 36, 48));
    box(93, 122, 54, 19, RGB(236, 230, 218));
    for (i = 0; i < 2; i++) {
        int32_t cx = i ? 141 : 99;
        oval((float)cx, 126, 3.5f, 3.5f, RGB(58, 51, 64), 32, 0);
        oval((float)cx, 126, fm_minf(3.0f, (3.0f + (lit ? pulse * 2.0f : 0.0f)) * 0.6f), fm_minf(3.0f, (3.0f + (lit ? pulse * 2.0f : 0.0f)) * 0.6f), RGB(90, 82, 98), 32, 0);
    }
    box(108, 123, 24, 6, RGB(14, 27, 28));
    box(134, 124, 3, 3, synth_on ? RGB(255, 154, 92) : RGB(58, 44, 34));
    for (i = 0; i < 22; i += 2) {
        int32_t yy = lit ? (int32_t)(fm_sinf(time * 9.0f + (float)i * 0.7f) * 2.0f * fm_minf(1.0f, pulse * 3.0f + 0.2f)) : 0;
        dot(109 + i, 126 - yy, lit ? RGB(127, 224, 200) : synth_on ? RGB(43, 74, 70) : RGB(14, 27, 28));
    }
    {
        static const int WH[8] = {0, 2, 4, 5, 7, 9, 11, 0};
        static const int BL[5][2] = {{1, 0}, {3, 1}, {6, 3}, {8, 4}, {10, 5}};
        for (i = 0; i < 8; i++) {
            int on = 0, k;
            for (k = 0; lit && k < n_tones; k++) if (chord_tones[k] == WH[i]) on = 1;
            box(96 + i * 6, 131, 5, 9, on ? RGB(255, 201, 138) : RGB(255, 255, 255));
        }
        for (i = 0; i < 5; i++) {
            int on = 0, k;
            for (k = 0; lit && k < n_tones; k++) if (chord_tones[k] == BL[i][0]) on = 1;
            box(100 + BL[i][1] * 6, 131, 3, 5, on ? RGB(255, 154, 92) : RGB(42, 36, 48));
        }
    }
    /* the arch to the trophy room (closed for now), its glow */
    box(36, 76, 22, 20, RGB(106, 74, 56));
    box(34, 74, 26, 3, RGB(230, 239, 246)); box(34, 95, 26, 3, RGB(230, 239, 246));
    glow(52, 86, 22, RGB(255, 207, 138), 0.28f + pulse * 0.1f);
    box(64, 72, 1, 7, RGB(201, 154, 58)); box(62, 69, 5, 1, RGB(255, 213, 106)); box(64, 67, 1, 5, RGB(255, 213, 106)); box(63, 68, 3, 3, RGB(255, 213, 106));
    /* the drum machine's table (left): something missing */
    box_a(43, 115, 24, 24, RGB(30, 20, 20), 8);
    box(40, 112, 24, 24, RGB(106, 74, 56)); box(40, 112, 24, 2, RGB(125, 90, 69));
    box_a(44, 118, 16, 12, RGB(255, 255, 255), 4);
    cv_text(52 - text_w(&FONT_T, "?") / 2, 118, &FONT_T, "?", RGB(180, 170, 160));
    /* the radio (right) */
    box_a(173, 117, 30, 28, RGB(30, 20, 20), 8);
    box(170, 114, 30, 28, RGB(106, 74, 56)); box(170, 114, 30, 3, RGB(125, 90, 69));
    oval(180, 128, 7, 7, RGB(217, 199, 168), 32, 0);
    for (i = 0; i < 3; i++) oring(180, 128, 2.0f + (float)i * 2.0f, 2.0f + (float)i * 2.0f, RGB(80, 60, 40), 19);
    box(192, 120, 4, 4, RGB(58, 44, 34));
    box(191, 130, 6, 6, RGB(42, 31, 24)); box(193, 131, 2, 2, RGB(217, 199, 168));
    /* the wardrobe (bottom left) */
    box_a(61, 163, 32, 28, RGB(30, 20, 20), 8);
    box(58, 160, 32, 28, RGB(106, 74, 56)); box(58, 160, 32, 3, RGB(125, 90, 69));
    box(74, 163, 1, 23, RGB(90, 61, 46)); box(71, 173, 2, 3, RGB(217, 179, 106)); box(76, 173, 2, 3, RGB(217, 179, 106));
    box(60, 165, 2, 8, RGB(63, 174, 90));
    /* the journal desk (bottom right) */
    box_a(157, 163, 30, 24, RGB(30, 20, 20), 8);
    box(154, 160, 30, 24, RGB(106, 74, 56));
    box(158, 164, 10, 13, RGB(207, 198, 178)); box(169, 164, 10, 13, RGB(207, 198, 178));
    for (i = 0; i < 3; i++) { box(160, 167 + i * 3, 6, 1, RGB(184, 163, 122)); box(171, 167 + i * 3, 6, 1, RGB(184, 163, 122)); }
    /* the door */
    box(108, 210, 24, 30, RGB(15, 13, 20));
    box(105, 208, 3, 18, RGB(185, 203, 218)); box(132, 208, 3, 18, RGB(185, 203, 218));
    if (DOG.found) draw_dog((int32_t)DOG.x, (int32_t)DOG.y, time);
    draw_player((int32_t)P.x, (int32_t)P.y);
}

/* ------------------------------------------------------------------ drawing: the bars, the messages --- */
static void draw_hud(void)
{
    char t[48], n[24];
    const char *title;
    int i;
    box_a(0, 0, SW, 15, C_DARK, 18);
    box_a(0, 225, SW, 15, C_DARK, 18);
    title = scene == SC_IGLOO ? "HOME" : (near_any_w > 0.5f && near_place >= 0) ? PLACES[near_place].name : "THE ICE FIELD";
    cv_text(5, 2, &FONT_T, title, C_SKY);
    /* the thermometer */
    box_a(174, 5, 22, 5, RGB(255, 255, 255), 6);
    box(174, 5, (int32_t)(22.0f * (1.0f - cold)), 5, cold > 0.6f ? C_SKY : C_GOLD);
    int_text(-5 - (int32_t)(cold * 25.0f + 0.5f), t); text_cat(t, "C");
    cv_text(235 - text_w(&FONT_T, t), 2, &FONT_T, t, C_INK);
    /* the compass: north, and an arrow to the nearest place not found yet (then home) */
    if (scene == SC_OUT) {
        int tgt = -1;
        float bd = 1e9f, a;
        oval(228, 30, 10, 10, C_DARK, 18, 0);
        cv_text(225, 19, &FONT_T, "N", RGB(255, 111, 111));
        for (i = 0; i < NPLACE; i++)
            if (!(discovered & (1u << i))) { float d = hyp(PLACES[i].cx - P.x / T, PLACES[i].cy - P.y / T); if (d < bd) { bd = d; tgt = i; } }
        if (tgt < 0) tgt = PL_IGLOO;
        a = fm_atan2f_approx(PLACES[tgt].cy - P.y / T, PLACES[tgt].cx - P.x / T);
        {
            uint16_t c = discovered == (1u << NPLACE) - 1u ? C_GOLD : C_SKY;
            int32_t x0 = 228 - (int32_t)(fm_cosf(a) * 4.0f), y0 = 30 - (int32_t)(fm_sinf(a) * 4.0f), x1 = 228 + (int32_t)(fm_cosf(a) * 7.0f), y1 = 30 + (int32_t)(fm_sinf(a) * 7.0f);
            line_a(x0, y0, x1, y1, c, 32); line_a(x0 + 1, y0, x1 + 1, y1, c, 32);
        }
    }
    /* the chord (or the chimes), the sound, the tempo */
    if (scene == SC_OUT && ctx_place < 0) cv_text(5, 227, &FONT_T, "CHIMES", C_GOLD);
    else cv_text(5, 227, &FONT_XS, chord_txt[0] ? chord_txt : "-", C_GOLD);
    t[0] = 0;
    if (cur_sound_name()[0]) { nice(cur_sound_name(), n); text_cat(t, n); text_cat(t, "  "); }
    int_text((int32_t)(bpm() + 0.5f), n); text_cat(t, n); text_cat(t, " BPM");
    cv_text(235 - text_w(&FONT_XS, t), 226, &FONT_XS, t, RGB(147, 167, 188));
    /* the hint, just above the bottom bar */
    {
        const char *h = hint();
        if (h[0] && !tuto_shown) text_box_c(120, 205, &FONT_XS, h, C_INK, 20, 7);
    }
    /* the banner never sits on what you are looking at: at home under the top bar, outside the other half */
    if (frame_ms < ban.until && ban.text[0]) {
        int home = scene == SC_IGLOO;
        const felucca_font_t *f = home ? &FONT_XS : &FONT_B;
        int32_t w = text_w(f, ban.text), by, hgt = (int32_t)f->h + 6;
        float a = fm_minf(1.0f, (float)(ban.until - frame_ms) / 400.0f);
        if (w > 214) { f = &FONT_XS; w = text_w(f, ban.text); hgt = (int32_t)f->h + 6; }
        by = home ? 17 : (P.y - cam_y < 145.0f ? 176 : 46);
        box_a(120 - w / 2 - 10, by, w + 20, hgt, C_DARK, A32(0.75f * a));
        if (a > 0.3f) cv_text(120 - w / 2, by + 3, f, ban.text, C_INK);
    }
    /* the first steps: how to walk */
    if (tuto_shown) {
        static const struct { int16_t x, y; const char *a, *l; } K[4] = {{120, 134, "^", "EDIT"}, {86, 155, "<", "SEQ"}, {154, 155, ">", "REC"}, {120, 176, "v", "PLAY"}};
        box_a(40, 112, 160, 104, C_DARK, 30);
        cv_text(120 - text_w(&FONT_XS, "Walk") / 2, 114, &FONT_XS, "Walk", C_INK);
        for (i = 0; i < 4; i++) {
            box(K[i].x - 15, K[i].y - 9, 30, 18, RGB(8, 16, 24));
            oring((float)K[i].x, (float)K[i].y, 15, 9, RGB(58, 74, 92), 32);
            cv_text(K[i].x - text_w(&FONT_T, K[i].l) / 2, K[i].y - 6, &FONT_T, K[i].l, C_INK);
        }
        cv_text(120 - text_w(&FONT_XS, "LFO = action") / 2, 194, &FONT_XS, "LFO = action", C_GOLD);
    }
}

/* ------------------------------------------------------------------ the frame --- */
void app_ui_init(void)
{
    int i;
    dist_init();
    for (i = 0; i < 90; i++) { flakes[i].x = frand() * 240.0f; flakes[i].y = frand() * 240.0f; flakes[i].s = 0.5f + frand(); }
    for (i = 0; i < 40; i++) { stars[i].x = (uint8_t)(frand() * 239.0f); stars[i].y = (uint8_t)(frand() * 119.0f); stars[i].p = frand() * 6.0f; }
    last_ms = plat_ms();
    lcd_fill(0, 0, 240, 240, RGB(10, 16, 24));
}

void app_frame(void)
{
    float time;
    input();
    update();
    frame_ms = plat_ms();
    time = tsec();
    pulse += (fm_minf(1.0f, eng_level() * 1.5f) - pulse) * 0.2f;
    phare_ang += 6.2831853f / (4.0f * beat()) * 0.35f * 0.016f;
    if (phare_ang > 6.2831853f) phare_ang -= 6.2831853f;
    cv_begin(240, 240, RGB(10, 16, 24));
    if (scene == SC_OUT) draw_outside(time);
    else draw_igloo_in(time);
    draw_hud();
    cv_blit(0, 0);
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "scene")) *got = scene;
    else if (!strcmp(what, "tx")) *got = (int)(P.x / T);
    else if (!strcmp(what, "ty")) *got = (int)(P.y / T);
    else if (!strcmp(what, "px")) *got = (int)P.x;
    else if (!strcmp(what, "py")) *got = (int)P.y;
    else if (!strcmp(what, "place")) *got = ctx_place;
    else if (!strcmp(what, "synth")) *got = synth_on;
    else if (!strcmp(what, "latched")) *got = latched;
    else if (!strcmp(what, "dog")) *got = DOG.found;
    else if (!strcmp(what, "found")) *got = discovered;
    else if (!strcmp(what, "level")) *got = (int)(syn_level * 100.0f);
    else return 0;
    return 1;
}
#endif
