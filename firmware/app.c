/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Obsimo (OSR Records) */
/* Banquise for the FM-1: a musical walk on an ice field. Part of the unity build (device/unity.c,
 * host/plat_host.c), after the platform: gfx.c is in scope. The synth is dsp/engine.c (OSR-1's), the world's
 * sounds dsp/world.c. A port of the browser prototype (index.html in the repository's root), redrawn for the
 * FM-1's screen (2.8 cm a side): the world is drawn at 120 x 120 and shown twice the size, Obsimo is 6 mm tall,
 * the ice field is 30 x 30 tiles (the browser's 96 x 96, packed: a place every screen or two), every inside
 * fits one screen, and there is very little text, all of it big.
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
static float fmod_pos(float x, float m) { return x - fm_floorf(x / m) * m; }
static float atan2_approx(float y, float x)   /* within 0.005 rad */
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
/* "SPLEEN ANGLAIS" -> "Spleen anglais" (n: room for 24) */
static void nice(const char *s, char *b)
{
    int first = 1, k = 0;
    for (; *s && k < 23; s++, k++) {
        char c = *s;
        if (!first && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        b[k] = c;
        first = 0;
    }
    b[k] = 0;
}

/* ------------------------------------------------------------------ the world (30 x 30 tiles of 20) --- */
#define T 20
#define MW 30
#define MH 30
#define VIEW 120                          /* the world on screen: 120 x 120, shown twice the size */
enum { SNOW, ICE, RIDGE, SEA, HOLE, OBJ, TREE, PITT };
static uint8_t map[MW * MH];
static inline int at(int x, int y) { return (x < 0 || y < 0 || x >= MW || y >= MH) ? SEA : map[y * MW + x]; }
static inline void put(int x, int y, int t) { if (x >= 0 && y >= 0 && x < MW && y < MH) map[y * MW + x] = (uint8_t)t; }
static inline int solid(int t) { return t == RIDGE || t == SEA || t == HOLE || t == OBJ || t == TREE; }

enum { SC_OUT, SC_IGLOO, SC_RIVER };
enum { PL_IGLOO, PL_TROU, PL_BATEAU, PL_GROTTE, PL_FORET, PL_PHARE, PL_PATIN, PL_PHOQUES, PL_FEU, NPLACE };
typedef struct {
    const char *name;                     /* in capitals: the big font has no lower case */
    float cx, cy, r;                      /* tiles */
    int8_t door_x, door_y;
    const char *grid;
    uint8_t key;
    float rev, cut;
    const char *var[5];
} place_t;
static const place_t PLACES[NPLACE] = {
    {"THE IGLOO", 15, 14, 3.5f, 15, 15, "REVERIE INDIE", 5, 0.3f, 0.62f, {"VELOURS", "CASSETTE", "WURLI", "TINES", "MELLOW"}},
    {"THE BIG HOLE", 9.5f, 20.5f, 4, -1, -1, "LYDIEN FLOTTANT", 2, 0.8f, 0.78f, {"GOUTTES", "KALIMBA", "CLOCHE", "MARIMBA", "VERRE"}},
    {"THE BOAT", 18, 3, 4, 18, 4, "SPLEEN ANGLAIS", 9, 0.66f, 0.52f, {"ORGUE NOYE", "ORGUE FANE", "HARMONIUM", "METAL DOUX", "BANDE USEE"}},
    {"THE ICE CAVE", 25, 22.5f, 4, 25, 24, "CIEL ETRANGE", 4, 0.92f, 0.82f, {"CRISTAUX", "VERRE", "AUBE", "SITAR FANTOME", "FLUTE DE VERRE"}},
    {"THE FOREST", 5.5f, 12, 5.5f, -1, -1, "DORIEN CHAUD", 2, 0.5f, 0.6f, {"KOTO", "MARIMBA", "GUITARE SECHE", "HARPE", "KALIMBA"}},
    {"THE LIGHTHOUSE", 26.5f, 5, 4, -1, -1, "CINEMA", 5, 0.72f, 0.55f, {"ORGUE FANE", "AUBE", "BRUME", "DERIVE", "VITRAIL"}},
    {"THE ICE RINK", 16, 24.5f, 4.5f, -1, -1, "DISCO", 0, 0.4f, 0.62f, {"RAVE LOINTAIN", "VITRAIL", "STAB HOUSE", "SIROP", "WURLI"}},
    {"THE SEALS", 6.5f, 3.5f, 4, -1, -1, "POP LUMINEUX", 7, 0.6f, 0.7f, {"CLOCHE", "KALIMBA", "VERRE", "MARIMBA", "GOUTTES"}},
    {"THE CAMPFIRE", 21, 17, 3.5f, -1, -1, "COUCHER DE SOLEIL", 7, 0.35f, 0.5f, {"GUITARE SECHE", "WURLI", "CASSETTE", "MELLOW", "HARMONIUM"}},
};
static const char *const IGLOO_BANK[10] = {"VELOURS", "TINES", "WURLI", "CASSETTE", "MELLOW", "SIROP", "VITRAIL", "HARPE",
                                           "CLAVECIN LUNAIRE", "PIANO JOUET"};
static const char *const IGLOO_GRIDS[10] = {"REVERIE INDIE", "PSYCHE REVEUR", "BALLADE", "INDIE PSYCHE", "COUCHER DE SOLEIL",
                                            "LYDIEN FLOTTANT", "SPLEEN ANGLAIS", "MELANCOLIE", "REVE 80S", "AMBIENT"};
#define FIRE_X 21
#define FIRE_Y 17
#define PIT_X 2                           /* the two little holes, and the river between them */
#define PIT_Y 19
#define PIT2_X 27
#define PIT2_Y 27
#define PHARE_TX 26
#define PHARE_TY 4
#define RINK_CX (16.0f * T)
#define RINK_CY (24.5f * T)
#define RINK_RX (3.6f * T)
#define RINK_RY (1.8f * T)
#define DOG_X0 22
#define DOG_Y0 11

typedef struct { const char *name; uint8_t key; float bpm, wind, rev; } weather_t;
static const weather_t WEATHER[3] = {{"DAY", 2, 84, 0.35f, 0.55f}, {"NIGHT", 9, 72, 0.22f, 0.7f}, {"STORM", 4, 96, 1.0f, 0.45f}};
static const char *const MODES[4] = {"DAY AND NIGHT", "DAY", "NIGHT", "STORM"};
#define DAY_LEN 480.0f

static int sound_idx(const char *n)
{
    int i;
    for (i = 0; i < NFAMILY * PER_FAMILY; i++) if (bq_eq(SOUND_NAME[i], n)) return i;
    return 0;
}
static int grid_idx(const char *n)
{
    int i;
    for (i = 0; i < NGRID; i++) if (bq_eq(GRID_DEF[i].name, n)) return i;
    return 0;
}

/* the map: the browser's geography (boat north, lighthouse north-east, forest west, cave south-east, rink
 * south), three times smaller */
static int32_t seed;
static float rnd(void)
{
    int32_t hi = seed / 127773, lo = seed % 127773, t = 16807 * lo - 2836 * hi;   /* Park-Miller, Schrage's way */
    if (t <= 0) t += 2147483647;
    seed = t;
    return (float)seed / 2147483647.0f;
}
static int near_any_place(int x, int y, float r)
{
    int i;
    for (i = 0; i < NPLACE; i++) if (hyp((float)x + 0.5f - PLACES[i].cx, (float)y + 0.5f - PLACES[i].cy) < r) return 1;
    return 0;
}
static void build_map(void)
{
    int x, y, i, k;
    seed = 23;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) {
            float n = fm_sinf((float)x * 0.55f) * fm_cosf((float)y * 0.5f) + fm_sinf((float)(x + y) * 0.27f)
                    + 0.5f * fm_sinf((float)x * 0.15f - (float)y * 0.2f);
            put(x, y, n > 1.1f ? ICE : SNOW);
        }
    /* the sea: the north shore (two bays: the seals, the boat), the edges */
    for (x = 0; x < MW; x++) {
        int d = 2 + (int)fm_floorf(0.6f * fm_sinf((float)x * 0.7f) + 0.5f);
        for (y = 0; y < d; y++) put(x, y, SEA);
    }
    for (y = 0; y < MH; y++) { put(0, y, SEA); put(MW - 1, y, SEA); if (fm_sinf((float)y * 0.6f) > 0.4f) { put(1, y, SEA); put(MW - 2, y, SEA); } }
    for (x = 0; x < MW; x++) { put(x, MH - 1, SEA); if (fm_sinf((float)x * 0.8f) > 0.5f) put(x, MH - 2, SEA); }
    /* a few snow ridges, away from the places */
    for (i = 0; i < 14; i++) {
        int px = 2 + (int)(rnd() * 26.0f), py = 4 + (int)(rnd() * 24.0f), dx = rnd() < 0.5f ? 1 : 0, len = 2 + (int)(rnd() * 3.0f);
        for (k = 0; k < len; k++) {
            if (!near_any_place(px, py, 4.5f) && at(px, py) != SEA) put(px, py, RIDGE);
            px += dx; py += 1 - dx;
        }
    }
    /* the forest: pines around a path and a small clearing */
    for (y = 7; y <= 17; y++)
        for (x = 1; x <= 11; x++) {
            float ex = ((float)x + 0.5f - 5.5f) / 5.0f, ey = ((float)y + 0.5f - 12.0f) / 5.0f, e = ex * ex + ey * ey;
            int path = fm_fabsf((float)y + 0.5f - (12.0f + 1.2f * fm_sinf((float)x * 0.8f))) < 0.9f;
            int clearing = hyp((float)x + 0.5f - 5.5f, (float)y + 0.5f - 12.0f) < 1.4f;
            if (e > 1.0f || at(x, y) == SEA) continue;
            put(x, y, (!path && !clearing && rnd() < 0.62f - e * 0.25f) ? TREE : SNOW);
        }
    /* the igloo (3 x 2, its door below), the boat's hull, the cave's mound, the lighthouse, the fire */
    for (y = 13; y <= 14; y++) for (x = 14; x <= 16; x++) put(x, y, OBJ);
    put(15, 15, SNOW); put(14, 15, SNOW); put(16, 15, SNOW);
    for (y = 2; y <= 3; y++) for (x = 15; x <= 21; x++) put(x, y, OBJ);
    put(18, 4, SNOW);
    for (y = 21; y <= 23; y++) for (x = 24; x <= 26; x++) put(x, y, OBJ);
    put(25, 24, SNOW);
    put(PHARE_TX, PHARE_TY, OBJ); put(PHARE_TX + 1, PHARE_TY, OBJ);
    for (y = PHARE_TY + 1; y <= PHARE_TY + 2; y++) for (x = PHARE_TX - 1; x <= PHARE_TX + 2; x++) if (at(x, y) == SEA) put(x, y, SNOW);
    put(FIRE_X, FIRE_Y, OBJ);
    /* the seals' rocks, on the shore */
    put(5, 2, OBJ); put(6, 2, OBJ); put(7, 2, OBJ); put(8, 2, OBJ);
    for (x = 4; x <= 9; x++) if (at(x, 3) == SEA) put(x, 3, SNOW);
    /* the big hole */
    for (y = 18; y <= 23; y++)
        for (x = 6; x <= 13; x++)
            if (hyp(((float)x + 0.5f - 9.5f) / 1.9f, ((float)y + 0.5f - 20.5f) / 1.5f) < 1.0f) put(x, y, HOLE);
    /* the rink */
    for (y = 21; y <= 28; y++)
        for (x = 11; x <= 21; x++) {
            float ex = ((float)x + 0.5f - 16.0f) / 3.6f, ey = ((float)y + 0.5f - 24.5f) / 1.8f, e = ex * ex + ey * ey;
            if (e < 1.0f) put(x, y, ICE);
            else if (e < 1.8f && at(x, y) == RIDGE) put(x, y, SNOW);
        }
    /* the two little holes */
    for (y = PIT_Y - 1; y <= PIT_Y + 1; y++) for (x = PIT_X - 1; x <= PIT_X + 2; x++) if (x > 0) put(x, y, SNOW);
    put(PIT_X, PIT_Y, PITT);
    for (y = PIT2_Y - 1; y <= PIT2_Y + 1; y++) for (x = PIT2_X - 1; x <= PIT2_X + 1; x++) put(x, y, SNOW);
    put(PIT2_X, PIT2_Y, PITT);
    /* room around Seito and the igloo's door */
    for (y = DOG_Y0 - 1; y <= DOG_Y0 + 1; y++) for (x = DOG_X0 - 1; x <= DOG_X0 + 1; x++) if (at(x, y) == RIDGE) put(x, y, SNOW);
    for (y = 15; y <= 17; y++) for (x = 13; x <= 17; x++) if (at(x, y) == RIDGE || at(x, y) == TREE) put(x, y, SNOW);
}

/* ------------------------------------------------------------------ the state, and its save --- */
#define SAVE_MAGIC 0x42514E32u            /* "BQN2": the small map */
typedef struct {
    uint32_t magic;
    float px, py, dog_x, dog_y, day_clock, play_time, cold;
    uint8_t dog_found, weather_mode, igloo_sound, igloo_grid;
    uint8_t kn[4];                        /* wind, drone, steps, synth: 0..20 (x 5 %) */
    uint8_t variant[NPLACE];
    uint8_t place_grid[NPLACE];           /* 255: the place's own */
    uint16_t discovered;
    uint8_t rsv[2];
} save_t;
static save_t sv;

enum { D_DOWN, D_UP, D_LEFT, D_RIGHT };
static struct { float x, y, vx, vy; uint8_t dir, moving; uint32_t step_anim; } P;
static struct { float x, y; uint8_t dir, found; } DOG;
static int scene;
static float out_x, out_y;               /* where to come back out */
static int weather, weather_mode;
static float day_clock = 40.0f, play_time, cold;
static float kn[4] = {0.6f, 0.55f, 0.65f, 0.75f};
static uint8_t variant[NPLACE], place_grid[NPLACE];
static uint16_t discovered = 1u << PL_IGLOO;
static int igloo_sound, igloo_grid, synth_on;
static uint32_t save_due;

#define TRAIL_N 40
static float trail_x[TRAIL_N], trail_y[TRAIL_N];
static int trail_n;
static void trail_push(float x, float y)
{
    if (trail_n == TRAIL_N) {
        int k;
        for (k = 0; k < TRAIL_N - 1; k++) { trail_x[k] = trail_x[k + 1]; trail_y[k] = trail_y[k + 1]; }
        trail_n--;
    }
    trail_x[trail_n] = x; trail_y[trail_n] = y; trail_n++;
}

#define PRINT_N 120
static struct { float x, y; uint32_t t, lit; float note; } prints[PRINT_N];
static int print_head, print_count;
#define LOG_N 16
static int step_log[LOG_N], log_n, echo_idx;

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
static float syn_level, syn_target, syn_tc = 0.5f;
static uint32_t hold_until, swap_at;
static int swap_pending;
static float next_chord_t, resume_t, music_t;
static int prog_idx;
static const int PROG[4] = {KEY_C4 + 0, KEY_C4 + 9, KEY_C4 + 5, KEY_C4 + 7};

static float bpm(void) { return scene == SC_IGLOO ? 84.0f : scene == SC_RIVER ? 76.0f : WEATHER[weather].bpm; }
static float beat(void) { return 60.0f / bpm(); }
static int drone_key(void)
{
    return scene == SC_OUT && ctx_place >= 0 ? PLACES[ctx_place].key : scene == SC_IGLOO ? st.key : scene == SC_RIVER ? 2 : WEATHER[weather].key;
}
static const char *cur_sound_name(void)
{
    if (scene == SC_IGLOO) return IGLOO_BANK[igloo_sound];
    if (scene == SC_RIVER) return "SOUS L'EAU";
    return ctx_place >= 0 ? PLACES[ctx_place].var[variant[ctx_place]] : "";
}
static void set_grid(void)
{
    if (scene == SC_IGLOO) { st.grid = grid_idx(IGLOO_GRIDS[igloo_grid]); st.key = 5; }
    else if (scene == SC_RIVER) { st.grid = grid_idx("LYDIEN FLOTTANT"); st.key = 2; }
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
    if (scene != SC_IGLOO) { s.atk = fm_maxf(s.atk, 0.45f); s.rel = fm_maxf(s.rel, 1.4f); }   /* outside it only swells and fades */
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
    float cut = scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].cut : 0.6f) : scene == SC_RIVER ? 0.7f : 0.66f;
    return 70.0f * fm_powf(18000.0f / 70.0f, cut - cold * 0.12f);
}
static float rev_amount(void)
{
    return scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].rev : WEATHER[weather].rev) : scene == SC_RIVER ? 0.85f : 0.22f;
}
static void apply_levels(void)
{
    const weather_t *W = &WEATHER[weather];
    int muffled = scene != SC_OUT, forest = scene == SC_OUT && ctx_place == PL_FORET;
    WP.wind = scene == SC_RIVER ? 0.0f : kn[0] * (0.25f + 0.75f * W->wind) * (muffled ? 0.25f : forest ? 0.6f : 1.0f);
    WP.wind_lp = muffled ? 380.0f : forest ? 1300.0f : 6000.0f;
    WP.wind_bp = W->wind > 0.8f ? 900.0f : 480.0f;
    WP.drone = scene == SC_IGLOO ? 0.0f : kn[1] * 0.3f;
    WP.drone_midi = (float)(60 + drone_key());
    WP.steps = kn[2];
    WP.bells = kn[3];
    WP.rev = scene == SC_OUT ? (ctx_place >= 0 ? PLACES[ctx_place].rev : W->rev) : scene == SC_RIVER ? 0.85f : 0.3f;
    WP.bpm = bpm();
    WP.cold = cold;
}
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
    w = scene == SC_OUT ? (ctx_place >= 0 ? smooth(near_w) * 0.85f : 0.0f) : scene == SC_RIVER ? 0.5f : 1.0f;
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

static const int PENTA[5] = {0, 2, 4, 7, 9};
static float chime_note(void)
{
    int k = drone_key();
    return (float)(72 + k + PENTA[(int)(frand() * 4.999f)] + (frand() < 0.3f ? 12 : 0) - (k > 6 ? 12 : 0));
}
static float tone_at(int k)              /* the chord's tones, up the octaves */
{
    int s[6], i, j;
    for (i = 0; i < n_tones; i++) s[i] = chord_tones[i];
    for (i = 1; i < n_tones; i++) for (j = i; j > 0 && s[j - 1] > s[j]; j--) { int t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    return (float)(72 + s[k % n_tones] + 12 * (k / n_tones));
}

/* ------------------------------------------------------------------ messages on screen --- */
static struct { char text[32]; uint32_t until; } ban;
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
    sv.dog_found = DOG.found;
    sv.weather_mode = (uint8_t)weather_mode;
    sv.igloo_sound = (uint8_t)igloo_sound; sv.igloo_grid = (uint8_t)igloo_grid;
    for (i = 0; i < 4; i++) sv.kn[i] = (uint8_t)(kn[i] * 20.0f + 0.5f);
    for (i = 0; i < NPLACE; i++) { sv.variant[i] = variant[i]; sv.place_grid[i] = place_grid[i]; }
    sv.discovered = discovered;
    if (plat_store_save(OBJ_SETTINGS, &sv, sizeof sv) == 0) save_due = 0;
    else save_due = plat_ms() + 5000u;
}
static int load_save(void)
{
    int i;
    if (plat_store_load(OBJ_SETTINGS, &sv, sizeof sv) != (int)sizeof sv || sv.magic != SAVE_MAGIC) return 0;
    if (!(sv.px > 0.0f && sv.px < MW * T && sv.py > 0.0f && sv.py < MH * T)) return 0;
    P.x = sv.px; P.y = sv.py;
    DOG.found = sv.dog_found ? 1 : 0;
    if (DOG.found) { DOG.x = P.x; DOG.y = P.y + 12.0f; }
    day_clock = sv.day_clock; play_time = sv.play_time; cold = clamp01(sv.cold);
    weather_mode = sv.weather_mode % 4u;
    igloo_sound = sv.igloo_sound % 10u; igloo_grid = sv.igloo_grid % 10u;
    for (i = 0; i < 4; i++) kn[i] = clamp01((float)sv.kn[i] * 0.05f);
    for (i = 0; i < NPLACE; i++) { variant[i] = sv.variant[i] % 5u; place_grid[i] = sv.place_grid[i] < NGRID ? sv.place_grid[i] : 255; }
    discovered = sv.discovered | (1u << PL_IGLOO);
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
        if (weather_mode == 0 && scene == SC_OUT) banner(eff ? "NIGHT FALLS" : "DAYBREAK");
    }
    if (scene == SC_OUT && na > 0.7f && music_t > aurora_at) {
        int k = chord_tones[(int)(frand() * (float)n_tones * 0.999f)];
        aurora_at = music_t + 8.0f * beat();
        world_pad((float)(84 + k - 12), 0.045f * na);
    }
}

/* ------------------------------------------------------------------ the insides: the igloo, the river --- */
/* home: one screen (120 x 120), a round room; the aquarium at the back, the groove box left, the synth in the
 * middle, the radio right, the door below */
#define ROOM_CX 60.0f
#define ROOM_CY 64.0f
#define ROOM_R 50.0f
typedef struct { int16_t x0, y0, x1, y1; const char *hint; int id; } spot_t;
enum { SP_SYNTH, SP_DRUMS, SP_FISH, SP_RADIO };
static const spot_t SPOTS[4] = {
    {45, 52, 75, 66, "LFO: SYNTH ON / OFF", SP_SYNTH},
    {12, 52, 31, 70, "SOMETHING IS MISSING", SP_DRUMS},
    {35, 16, 85, 38, "GO FISHING!", SP_FISH},
    {91, 52, 109, 68, "THE RADIO: SOON", SP_RADIO},
};
static int inside_solid(float px, float py)
{
    int i;
    if (py > 108.0f) return !(fm_fabsf(px - 60.0f) < 7.0f);
    if (hyp(px - ROOM_CX, py - ROOM_CY) > ROOM_R - 3.0f && py < 104.0f) return 1;
    for (i = 0; i < 4; i++)
        if (px > SPOTS[i].x0 && px < SPOTS[i].x1 && py > SPOTS[i].y0 && py < SPOTS[i].y1) return 1;
    return 0;
}
static const spot_t *igloo_spot(void)
{
    const spot_t *b = 0;
    float bd = 9.0f, px = P.x, py = P.y;
    int i;
    for (i = 0; i < 4; i++) {
        const spot_t *sp = &SPOTS[i];
        float dx = fm_maxf(fm_maxf((float)sp->x0 - px, 0.0f), px - (float)sp->x1);
        float dy = fm_maxf(fm_maxf((float)sp->y0 - py, 0.0f), py - (float)sp->y1);
        float d = hyp(dx, dy);
        if (d < bd) { bd = d; b = sp; }
    }
    return b;
}

/* the river under the ice, between the two little holes: you slide, the walls ring */
#define RIV_LEN 600.0f
static float riv_top(float x) { return 20.0f + 6.0f * fm_sinf(x * 0.022f) + 3.5f * fm_sinf(x * 0.074f + 1.0f); }
static float riv_bot(float x) { return 106.0f - 6.0f * fm_sinf(x * 0.026f + 1.0f) - 3.5f * fm_sinf(x * 0.058f); }
static int riv_entry;                    /* 0 the west hole, 1 the east one */
#define NRIPPLE 6
static struct { float x, y; uint32_t t; } ripples[NRIPPLE];
static int ripple_i;
static uint32_t wall_note_ms;
static void wall_hit(float x, float y, float imp, int top)
{
    int idx;
    uint32_t now = plat_ms();
    if (imp < 0.45f || now - wall_note_ms < 100u) return;
    wall_note_ms = now;
    idx = (int)(x / RIV_LEN * 11.0f);
    world_bell((float)(50 + PENTA[idx % 5] + 12 * (idx / 5) + (top ? 12 : 0)), fm_minf(0.22f, 0.07f + imp * 0.05f), 2.4f);
    ripples[ripple_i].x = x; ripples[ripple_i].y = y; ripples[ripple_i].t = now;
    ripple_i = (ripple_i + 1) % NRIPPLE;
}

/* ------------------------------------------------------------------ moving about --- */
static int free_at(float px, float py)
{
    static const float OX[4] = {-5, 5, -5, 5}, OY[4] = {-2, -2, 2, 2};
    int i;
    for (i = 0; i < 4; i++) {
        float x = px + OX[i], y = py + OY[i];
        if (scene == SC_OUT) { if (x < 0.0f || y < 0.0f || solid(at((int)(x / T), (int)(y / T)))) return 0; }
        else if (scene == SC_IGLOO && inside_solid(x, y)) return 0;
    }
    return 1;
}
static int on_rink(float x, float y)
{
    float ex = (x - RINK_CX) / RINK_RX, ey = (y - RINK_CY) / RINK_RY;
    return scene == SC_OUT && ex * ex + ey * ey < 1.0f;
}

static float cam_x, cam_y, look_x, look_y;
static int tuto_shown = 1;
static float tuto_x, tuto_y;
static int gliding;
static float glide_note_t, spin_t;
#define NBLADE 60
static struct { float x, y; uint32_t t; } blades[NBLADE];
static int blade_i;

static void enter_scene(int s)
{
    int prev = scene;
    if (s == scene) return;
    if (scene == SC_OUT && s == SC_IGLOO) { out_x = P.x; out_y = P.y + 8.0f; }
    scene = s;
    P.vx = P.vy = 0.0f;
    gliding = 0;
    WP.skate = 0.0f;
    WP.river = 0.0f;
    if (s == SC_OUT && prev == SC_RIVER) {
        /* up the rope: out by the other hole, or the same one */
        P.x = (riv_entry ? PIT2_X - 1 : PIT_X + 2) * T + T / 2; P.y = (riv_entry ? PIT2_Y : PIT_Y) * T + T / 2;
        P.dir = D_DOWN;
    } else if (s == SC_OUT) { P.x = out_x; P.y = out_y; P.dir = D_DOWN; }
    else if (s == SC_RIVER) {
        P.x = riv_entry ? RIV_LEN - 30.0f : 30.0f;
        P.y = (riv_top(P.x) + riv_bot(P.x)) * 0.5f;
        P.vx = riv_entry ? -0.7f : 0.7f; P.vy = 0.0f;
        P.dir = riv_entry ? D_LEFT : D_RIGHT;
    } else { P.x = 60.0f; P.y = 100.0f; P.dir = D_UP; }
    if (s == SC_IGLOO) { synth_on = 0; save_soon(); }
    held = -1; latched = -1; resume_t = 0.0f;
    banner(s == SC_IGLOO ? "HOME" : s == SC_RIVER ? "UNDER THE ICE" : "THE ICE FIELD");
    if (DOG.found) { DOG.x = P.x - 14.0f; DOG.y = P.y + 4.0f; }
    trail_n = 0;
    ctx_place = -1;
    near_w = s == SC_OUT ? 0.0f : 1.0f;
    look_x = look_y = 0.0f;
    setup_music(0.1f, 380u, 0);
}
static void go_home(void)
{
    if (scene == SC_IGLOO) { banner("ALREADY HOME"); return; }
    enter_scene(SC_IGLOO);
    out_x = 15.0f * T + T / 2; out_y = 15.0f * T + T / 2 + 6.0f;
    world_bell(79, 0.05f, 0.8f); world_bell(76, 0.05f, 0.8f); world_bell(72, 0.05f, 0.8f);
}
static void fall_in(int east)
{
    world_splash();
    world_bell(74, 0.07f, 2.0f); world_bell(71, 0.07f, 2.0f); world_bell(67, 0.07f, 2.0f);
    riv_entry = east;
    enter_scene(SC_RIVER);
}

static void toggle_synth(void)
{
    synth_on = !synth_on;
    if (!synth_on) { latched = -1; eng_stop(); banner("SYNTH OFF"); }
    else { banner("SYNTH ON"); latched = last_home_chord; play_idx(latched, 0.2f); }
}
static int near_hole(void) { return scene == SC_OUT && hyp(P.x / T - PLACES[PL_TROU].cx, P.y / T - PLACES[PL_TROU].cy) < 3.2f; }
static int near_fire(void) { return scene == SC_OUT && hyp(P.x / T - FIRE_X - 0.5f, P.y / T - FIRE_Y - 0.5f) < 1.8f; }
static int near_rope(void) { return scene == SC_RIVER && (P.x < 50.0f || P.x > RIV_LEN - 50.0f); }

static void pirouette(void)
{
    int i;
    spin_t = music_t;
    for (i = 0; i < 4; i++) world_bell(tone_at(i), 0.07f, 1.4f);
}
static void action(void)
{
    if (scene == SC_IGLOO) {
        const spot_t *sp = igloo_spot();
        if (!sp) return;
        if (sp->id == SP_SYNTH) toggle_synth();
        else banner(sp->hint);
        return;
    }
    if (scene == SC_RIVER) {
        if (near_rope()) { riv_entry = P.x > RIV_LEN * 0.5f; enter_scene(SC_OUT); }
        else banner("A ROPE AT EACH END");
        return;
    }
    if (on_rink(P.x, P.y)) { pirouette(); return; }
    if (near_hole()) { banner("FISHING: NEXT UPDATE"); return; }
    if (near_fire()) { banner("THE FIRE: NEXT UPDATE"); return; }
}
static const char *hint(void)
{
    if (scene == SC_IGLOO) { const spot_t *sp = igloo_spot(); return sp ? sp->hint : ""; }
    if (scene == SC_RIVER) return near_rope() ? "LFO: CLIMB OUT" : "";
    if (on_rink(P.x, P.y)) return "LFO: SPIN";
    if (near_place == PL_IGLOO && near_any_w > 0.6f) return "UP: GO INSIDE";
    return "";
}

/* ------------------------------------------------------------------ the keys --- */
static void press(int i)
{
    char t[24];
    if (scene == SC_IGLOO) {
        if (!synth_on) { banner("THE SYNTH IS OFF"); return; }
        if (latched == i) { latched = -1; eng_stop(); return; }
        latched = i; last_home_chord = i;
        play_idx(i, 0.25f);
        t[0] = 0; text_cat(t, chord_txt); banner(t);
        return;
    }
    if (scene == SC_RIVER) { world_bell((float)(62 + PENTA[i % 5] + 12 * ((i % 10) / 5)), 0.12f, 2.4f); return; }
    if (near_place == PL_IGLOO && near_any_w > 0.3f && ctx_place < 0) { banner("PLAY INSIDE"); return; }
    if (ctx_place < 0) { banner("WALK TO A PLACE"); return; }
    held = i;
    play_idx(i, 0.2f);
    user_here = 1;
    eng_target();
}
static void release(int i)
{
    if (scene != SC_OUT || held != i) return;
    held = -1;
    resume_t = music_t + 4.0f * beat();
    next_chord_t = resume_t;
    eng_target();
}
static void say_name(const char *s) { char t[24]; nice(s, t); { int k; for (k = 0; t[k]; k++) if (t[k] >= 'a' && t[k] <= 'z') t[k] = (char)(t[k] - 32); } banner(t); }
static void step_grid(void)
{
    if (scene == SC_IGLOO) igloo_grid = (igloo_grid + 1) % 10;
    else if (scene == SC_OUT && ctx_place >= 0) place_grid[ctx_place] = (uint8_t)(((place_grid[ctx_place] != 255 ? place_grid[ctx_place] : grid_idx(PLACES[ctx_place].grid)) + 1) % NGRID);
    else { banner("WALK TO A PLACE"); return; }
    set_grid();
    say_name(GRID_DEF[st.grid].name);
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
    if (scene == SC_IGLOO) {
        igloo_sound = (igloo_sound + d + 10) % 10;
        say_name(IGLOO_BANK[igloo_sound]);
        save_soon();
        setup_music(0.05f, 160u, 0);
        return;
    }
    if (scene != SC_OUT || ctx_place < 0) { banner("WALK TO A PLACE"); return; }
    variant[ctx_place] = (uint8_t)((variant[ctx_place] + d + 5) % 5);
    say_name(PLACES[ctx_place].var[variant[ctx_place]]);
    setup_music(0.25f, 600u, 0);
}
static void knob(int k, int32_t e)
{
    static const char *const NAME[4] = {"WIND", "DRONE", "STEPS", "SYNTH"};
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
static uint8_t dirs[4];
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
    while (last_steps != s) {
        last_steps++;
        P.step_anim++;
        if (scene == SC_OUT) {
            float note = 0.0f;
            int i = print_head;
            if (near_w < 0.3f && frand() < 0.3f) { note = chime_note(); world_bell(note, 0.07f * (1.0f - near_w / 0.3f), 2.2f); }
            prints[i].x = P.x + ((P.step_anim & 1u) ? 3.0f : -3.0f);
            prints[i].y = P.y + 2.0f;
            prints[i].t = now; prints[i].lit = 0; prints[i].note = note;
            print_head = (print_head + 1) % PRINT_N;
            if (print_count < PRINT_N) print_count++;
            if (log_n == LOG_N) { int k; for (k = 0; k < LOG_N - 1; k++) step_log[k] = step_log[k + 1]; log_n--; }
            step_log[log_n++] = i;
        }
    }
    while (last_ticks != tk) {
        last_ticks++;
        if (scene == SC_OUT && !P.moving && !gliding && log_n >= 4 && music_t - idle_since_t > 4.0f * beat()) {
            int pi = step_log[echo_idx % log_n];
            if (prints[pi].note > 0.0f) { world_bell(prints[pi].note, 0.045f * (1.0f - near_w), 1.2f); prints[pi].lit = now; }
            echo_idx++;
        }
    }
}

/* the rink: you keep sliding, the blades hiss, and gliding fast plays the chord, low to the left, high to the right */
static void glide(int dx, int dy, float dt)
{
    float fk = dt * 60.0f, fr, sp, ox = P.x, oy = P.y;
    if (!gliding) { gliding = 1; P.vx = (float)dx * 0.55f; P.vy = (float)dy * 0.55f; glide_note_t = 0.0f; }
    P.vx += (float)dx * 0.05f * fk; P.vy += (float)dy * 0.05f * fk;
    fr = fm_powf(0.988f, fk); P.vx *= fr; P.vy *= fr;
    sp = hyp(P.vx, P.vy);
    if (sp > 1.4f) { P.vx *= 1.4f / sp; P.vy *= 1.4f / sp; sp = 1.4f; }
    P.x += P.vx * fk; P.y += P.vy * fk;
    if (!free_at(P.x, P.y)) { P.x = ox; P.y = oy; P.vx *= -0.4f; P.vy *= -0.4f; }
    if (dx || dy) P.dir = dx && !dy ? (dx > 0 ? D_RIGHT : D_LEFT) : (dy > 0 ? D_DOWN : D_UP);
    if (music_t - spin_t < 0.8f) P.dir = (uint8_t)((int)((music_t - spin_t) * 10.0f) % 4);
    P.moving = 0;
    if (sp > 0.15f) { blades[blade_i].x = P.x; blades[blade_i].y = P.y + 3.0f; blades[blade_i].t = plat_ms(); blade_i = (blade_i + 1) % NBLADE; }
    WP.skate = fm_minf(0.09f, sp * 0.06f);
    WP.skate_f = 1600.0f + sp * 1800.0f;
    if (sp > 0.4f) {
        float e8 = beat() * 0.5f;
        if (glide_note_t < music_t) glide_note_t = music_t + 0.05f;
        if (music_t >= glide_note_t) {
            float pos = clamp01((P.x - (RINK_CX - RINK_RX)) / (2.0f * RINK_RX));
            world_bell(tone_at((int)(pos * (float)n_tones * 2.0f)), fm_minf(0.09f, 0.03f + sp * 0.04f), 1.8f);
            glide_note_t += e8;
        }
    }
}
static void glide_end(void) { if (gliding) { gliding = 0; P.vx = P.vy = 0.0f; WP.skate = 0.0f; } }

static void river_move(int dx, int dy, float dt)
{
    float fk = dt * 60.0f, fr, sp, tp, bt;
    P.vx += (float)dx * 0.045f * fk; P.vy += (float)dy * 0.045f * fk;
    fr = fm_powf(0.993f, fk); P.vx *= fr; P.vy *= fr;
    sp = hyp(P.vx, P.vy);
    if (sp > 1.6f) { P.vx *= 1.6f / sp; P.vy *= 1.6f / sp; sp = 1.6f; }
    P.x += P.vx * fk; P.y += P.vy * fk;
    tp = riv_top(P.x); bt = riv_bot(P.x);
    if (P.y - 12.0f < tp) { P.y = tp + 12.0f; if (P.vy < 0.0f) { wall_hit(P.x, tp, -P.vy * 2.0f, 1); P.vy = -P.vy * 0.55f; } }
    if (P.y + 3.0f > bt) { P.y = bt - 3.0f; if (P.vy > 0.0f) { wall_hit(P.x, bt, P.vy * 2.0f, 0); P.vy = -P.vy * 0.55f; } }
    if (P.x < 12.0f) { P.x = 12.0f; if (P.vx < 0.0f) { wall_hit(P.x, P.y, -P.vx * 2.0f, 0); P.vx = -P.vx * 0.5f; } }
    if (P.x > RIV_LEN - 12.0f) { P.x = RIV_LEN - 12.0f; if (P.vx > 0.0f) { wall_hit(P.x, P.y, P.vx * 2.0f, 1); P.vx = -P.vx * 0.5f; } }
    if (dx || dy) P.dir = dx && !dy ? (dx > 0 ? D_RIGHT : D_LEFT) : (dy > 0 ? D_DOWN : D_UP);
    P.moving = 0;
    WP.river = 0.05f + fm_minf(0.04f, sp * 0.025f);
}

static void update(void)
{
    uint32_t now = plat_ms();
    float dt = (float)(now - last_ms) * 0.001f, sp;
    int dx, dy, i;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    play_time += dt;
    music_t += dt;
    dx = (dirs[3] ? 1 : 0) - (dirs[2] ? 1 : 0);
    dy = (dirs[1] ? 1 : 0) - (dirs[0] ? 1 : 0);
    P.moving = (uint8_t)(dx || dy);
    if (scene == SC_RIVER) river_move(dx, dy, dt);
    else if (on_rink(P.x, P.y)) glide(dx, dy, dt);
    else {
        glide_end();
        if (P.moving) {
            sp = (scene == SC_OUT ? 58.0f : 44.0f) * dt * (scene == SC_OUT && WEATHER[weather].wind > 0.8f ? 0.85f : 1.0f) / (dx && dy ? 1.41421356f : 1.0f);
            if (dx) {
                if (free_at(P.x + (float)dx * sp, P.y)) P.x += (float)dx * sp;
                else if (!dy) { int o; for (o = -1; o <= 1; o += 2) if (free_at(P.x + (float)dx * sp, P.y + (float)o * 3.0f) && free_at(P.x, P.y + (float)o)) { P.y += (float)o * 0.6f; break; } }
            }
            if (dy) {
                if (free_at(P.x, P.y + (float)dy * sp)) P.y += (float)dy * sp;
                else if (!dx) { int o; for (o = -1; o <= 1; o += 2) if (free_at(P.x + (float)o * 3.0f, P.y + (float)dy * sp) && free_at(P.x + (float)o, P.y)) { P.x += (float)o * 0.6f; break; } }
            }
            P.dir = dx && !dy ? (dx > 0 ? D_RIGHT : D_LEFT) : (dy > 0 ? D_DOWN : dy < 0 ? D_UP : P.dir);
        }
    }
    if (P.moving || gliding) trail_push(P.x, P.y);
    if (tuto_shown && hyp(P.x - tuto_x, P.y - tuto_y) > 16.0f) tuto_shown = 0;
    if (P.moving) { if (idle) { idle = 0; echo_idx = 0; } }
    else if (!idle) { idle = 1; idle_since_t = music_t; }
    /* doors, holes */
    if (scene == SC_OUT) {
        int tx = (int)(P.x / T), ty = (int)(P.y / T);
        for (i = 0; i < NPLACE; i++) {
            const place_t *p = &PLACES[i];
            if (p->door_x < 0 || dy >= 0) continue;
            if ((tx > p->door_x ? tx - p->door_x : p->door_x - tx) <= (i == PL_IGLOO ? 1 : 0) && ty == p->door_y && P.y < p->door_y * T + 12) {
                if (i == PL_IGLOO) { enter_scene(SC_IGLOO); return; }
                if (now > ban.until) banner("FROZEN SHUT. SOON!");
            }
        }
        if (at(tx, ty) == PITT && hyp(P.x - (tx * T + T / 2), P.y - (ty * T + T / 2)) < 8.0f) { fall_in(tx == PIT2_X); return; }
    } else if (scene == SC_IGLOO && P.y > 112.0f && dy > 0) { enter_scene(SC_OUT); return; }
    {
        float rate = near_fire() ? -1.0f / 15.0f : scene == SC_OUT ? (WEATHER[weather].wind > 0.8f ? 1.0f / 90.0f : 1.0f / 220.0f)
                   : scene == SC_IGLOO ? -1.0f / 8.0f : 1.0f / 300.0f;
        cold = clamp01(cold + rate * dt);
        WP.cold = cold;
    }
    if (scene == SC_OUT) {
        int best = -1, best_m = -1, next_ctx;
        float bw = 0.0f, bwm = 0.0f;
        for (i = 0; i < NPLACE; i++) {
            const place_t *p = &PLACES[i];
            float d = hyp(P.x / T - p->cx, P.y / T - p->cy), w = clamp01((p->r - d) / 2.5f);
            if (w > bw) { bw = w; best = i; }
            if (i != PL_IGLOO && w > bwm) { bwm = w; best_m = i; }
            if (d < p->r - 0.5f && !(discovered & (1u << i))) { discovered |= (uint16_t)(1u << i); banner(p->name); save_soon(); }
        }
        near_place = best; near_any_w = bw; near_w = bwm;
        next_ctx = near_w > 0.5f ? best_m : near_w < 0.3f ? -1 : ctx_place;
        if (next_ctx != ctx_place) {
            ctx_place = next_ctx; user_here = 0;
            if (ctx_place >= 0) { if (DOG.found) world_woof(); if (now > ban.until) banner(PLACES[ctx_place].name); }
            setup_music(0, 0, 0);
        }
        if (!DOG.found && hyp(P.x - DOG.x, P.y - DOG.y) < 26.0f) { DOG.found = 1; banner("SEITO!"); world_woof(); save_soon(); }
        if (DOG.found) dog_follow(16);
    } else if (DOG.found && scene == SC_IGLOO) dog_follow(12);
    WP.dog = (uint8_t)(DOG.found && scene == SC_OUT);
    WP.moving = P.moving && scene != SC_RIVER && !gliding;
    WP.ground = scene == SC_IGLOO ? GROUND_FUR : at((int)(P.x / T), (int)(P.y / T)) == ICE ? GROUND_ICE : GROUND_SNOW;
    steps_tick();
    if (scene == SC_OUT && !P.moving && !gliding && near_w <= 0.3f && music_t > idle_chime_t) {
        if (frand() < 0.35f && !(log_n >= 4 && idle && music_t - idle_since_t > 4.0f * beat())) world_bell(chime_note(), 0.05f, 2.6f);
        idle_chime_t = music_t + 1.6f + frand() * 2.0f;
    }
    cycle_tick(dt);
    if (swap_pending && now >= swap_at) swap_now();
    eng_target();
    syn_level += (syn_target - syn_level) * (1.0f - fm_expf(-dt / fm_maxf(0.01f, syn_tc)));
    music_tick();
    if (save_due && now >= save_due && syn_level < 0.02f && !P.moving && !gliding) save_now();
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
    memset(place_grid, 255, sizeof place_grid);
    P.x = 15.0f * T + T / 2; P.y = 16.0f * T + 12.0f; P.dir = D_UP;
    DOG.x = DOG_X0 * T + T / 2; DOG.y = DOG_Y0 * T + T / 2; DOG.dir = D_LEFT;
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
#define C_SNOW2 RGB(225, 234, 242)
#define C_INK RGB(234, 242, 250)
#define C_DARK RGB(8, 14, 22)
#define C_GOLD RGB(255, 207, 138)
#define C_ORANGE RGB(255, 154, 74)
#define C_SHADOW RGB(40, 70, 100)

static uint16_t lo[VIEW * VIEW] PLAT_POOL;   /* the world, drawn small */
static float pulse;
static uint32_t frame_ms;

/* ------------------------------------------------------------------ the sprites (one pixel = 2 on screen) --- */
static const char *const OB_DOWN[28] = {
    "......HHHHHHHH......", "....HHHHHHHHHHHH....", "...HHHHHHHHHHHHHH...", "..HHHHHHHHHHHHHHHH..", "..HHhHHHHHHHHHHhHH..",
    "..HHSSSSSSSSSSSSHH..", "..HSSSSSSSSSSSSSSH..", "..HSSEESSSSSSEESSH..", "..HSSEESSSSSSEESSH..", "...SSSSSSSSSSSSSS...",
    "...SSSSSSssSSSSSS...", "....SSSSSSSSSSSS....", ".....SSSSssSSSS.....", "...ggGGGGGGGGGGgg...", "..gGGGGGGGGGGGGGGg..",
    ".SgGGGGGGwwGGGGGGgS.", ".SGGGGGGGwwGGGGGGGS.", ".SGGGGGGGwwGGGGGGGS.", ".SGGGGGGGwwGGGGGGGS.", "..gGGGGGGwwGGGGGGg..",
    "..gggggggggggggggg..", "...BBBBBBBBBBBBBB...", "...BBBBBBBBBBBBBB...", "...BBBBBB..BBBBBB...", "...BBBBB....BBBBB...",
    "...BBBBB....BBBBB...", "..KKKKKK....KKKKKK..", "..KKKKKK....KKKKKK.."};
static const char *const OB_UP[28] = {
    "......HHHHHHHH......", "....HHHHHHHHHHHH....", "...HHHHHHHHHHHHHH...", "..HHHHHHHHHHHHHHHH..", "..HHHHHHHHHHHHHHHH..",
    "..HHHHHHHHHHHHHHHH..", "..HHHHHHHHHHHHHHHH..", "..HHHHHHHHHHHHHHHH..", "..hHHHHHHHHHHHHHHh..", "...hHHHHHHHHHHHHh...",
    "...SSSSSSSSSSSSSS...", "....SSSSSSSSSSSS....", ".....SSSSSSSSSS.....", "...ggGGGGGGGGGGgg...", "..gGGGGGGGGGGGGGGg..",
    ".SgGGGGGGGGGGGGGGgS.", ".SGGGGGGGGGGGGGGGGS.", ".SGGGGGGGGGGGGGGGGS.", ".SGGGGGGGGGGGGGGGGS.", "..gGGGGGGGGGGGGGGg..",
    "..gggggggggggggggg..", "...BBBBBBBBBBBBBB...", "...BBBBBBBBBBBBBB...", "...BBBBBB..BBBBBB...", "...BBBBB....BBBBB...",
    "...BBBBB....BBBBB...", "..KKKKKK....KKKKKK..", "..KKKKKK....KKKKKK.."};
static const char *const OB_SIDE[28] = {   /* facing right */
    "......HHHHHHHH......", "....HHHHHHHHHHHH....", "...HHHHHHHHHHHHHH...", "..HHHHHHHHHHHHHHHH..", "..HHHHHHHHHHHHHHH...",
    "..HHHHHHHHSSSSSSH...", "..HHHHHHSSSSSSSSS...", "..HHHHHSSSSSSSEESS..", "..HHHHHSSSSSSSEESS..", "...HHHSSSSSSSSSSSS..",
    "...HHHSSSSSSSSSsss..", "....HHSSSSSSSSSSS...", ".....SSSSSSssSSS....", "......SSSSSSSS......", "....gGGGGGGGGGGg....",
    "...gGGGGGGGGGGGGg...", "...gGGGGGGGGGGGGS...", "...GGGGGGGGGGGGGS...", "...GGGGGGGGGGGGGS...", "...GGGGGGGGGGGGG....",
    "...gGGGGGGGGGGGg....", "...gggggggggggggg...", "....BBBBBBBBBBBB....", "....BBBBBBBBBBBB....", "....BBBBB..BBBBB....",
    "....BBBBB..BBBBB....", "...KKKKKK..KKKKKK...", "...KKKKKK..KKKKKK..."};
static const char *const DOG_ROWS[13] = {
    "..................CC..", ".................CCCN.", "................CCCCCN", "C..............CCCCCC.", ".C.....CCCCCCCCCCCC...",
    "..C..CCCCCCCCCCCCCC...", "..CCCCCCCCCCCCCCCC....", "...CCCCCCCCCCCCCCC....", "...CcCCCCCCCCCCcCC....", "...Cc.CC.....CC.cC....",
    "...Cc.CC.....CC.cC....", "...cc.cc.....cc.cc....", "......................"};
static const pal_t PAL[13] = {{'H', RGB(243, 211, 107)}, {'h', RGB(201, 166, 64)}, {'S', RGB(242, 198, 160)}, {'s', RGB(217, 164, 127)},
    {'E', RGB(31, 26, 36)}, {'G', RGB(63, 174, 90)}, {'g', RGB(47, 138, 71)}, {'w', RGB(223, 244, 255)}, {'B', RGB(43, 58, 103)},
    {'K', RGB(27, 23, 32)}, {'C', RGB(138, 90, 60)}, {'c', RGB(110, 70, 48)}, {'N', RGB(42, 29, 24)}};

static void draw_player(int32_t sx, int32_t sy)        /* sx, sy: the feet */
{
    const char *rows[28];
    const char *const *src = P.dir == D_LEFT || P.dir == D_RIGHT ? OB_SIDE : P.dir == D_UP ? OB_UP : OB_DOWN;
    int walk = P.moving && (P.step_anim & 1u), side = P.dir == D_LEFT || P.dir == D_RIGHT, i;
    int shiver = cold > 0.6f && ((frame_ms / 55u) & 1u) ? 1 : 0;
    for (i = 0; i < 28; i++) rows[i] = src[i];
    if (walk) {
        if (side) { rows[24] = "...BBBBB....BBBBB..."; rows[25] = "..BBBBB......BBBBB.."; rows[26] = ".KKKKKK......KKKKKK."; rows[27] = ".KKKKKK......KKKKKK."; }
        else rows[27] = (P.step_anim & 2u) ? "..KKKKKK............" : "............KKKKKK..";
    }
    box_a(sx - 9, sy - 1, 18, 3, C_SHADOW, 8);
    sprite(rows, 28, sx - 10 + shiver, sy - 27 - (walk ? 1 : 0), P.dir == D_LEFT, PAL, 13);
}
static void draw_dog(int32_t sx, int32_t sy, float time)
{
    const char *rows[13];
    int i;
    for (i = 0; i < 13; i++) rows[i] = DOG_ROWS[i];
    if ((int)(time * (DOG.found ? 8.0f : 3.0f)) & 1) { rows[3] = "...............CCCCCC."; rows[4] = "CC.....CCCCCCCCCCCC..."; }
    box_a(sx - 9, sy - 1, 18, 2, C_SHADOW, 8);
    sprite(rows, 13, sx - 11, sy - 12, DOG.dir == D_LEFT, PAL, 13);
}

/* ------------------------------------------------------------------ the tiles --- */
static void tile(int t, int x, int y, int32_t sx, int32_t sy, float time)
{
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    switch (t) {
    case ICE:
        box(sx, sy, T, T, ((x + y) & 1) ? RGB(191, 224, 242) : RGB(186, 220, 239));
        if (h % 3u == 0u) box_a(sx + (int32_t)(h % 12u), sy + (int32_t)(h % 10u) + 3, 6, 1, RGB(255, 255, 255), 18);
        if (h % 5u == 0u) box(sx + 3, sy + (int32_t)(h % 15u) + 2, 11, 1, RGB(156, 200, 226));
        break;
    case RIDGE:
        box(sx, sy, T, T, C_SNOW2);
        box(sx + 1, sy + 5, 18, 14, RGB(198, 216, 232));
        box(sx + 2, sy + 4, 12, 5, RGB(247, 251, 255));
        box(sx + 1, sy + 17, 18, 2, RGB(169, 195, 217));
        break;
    case SEA: {
        float w = fm_sinf(time * 1.5f + (float)x * 0.8f + (float)y) * 3.0f;
        box(sx, sy, T, T, ((x + y) & 1) ? RGB(29, 74, 112) : RGB(26, 68, 102));
        box_a(sx + 5 + (int32_t)w, sy + 7 + (int32_t)(h % 6u), 8, 1, RGB(140, 200, 240), 13);
        if (h % 4u == 0u) box(sx + (int32_t)(h % 11u), sy + (int32_t)(h % 9u), 5, 3, C_SNOW1);
        break;
    }
    case PITT:
        box(sx, sy, T, T, C_SNOW2);
        oval((float)sx + 10.0f, (float)sy + 11.0f, 8.0f, 6.0f, RGB(195, 212, 227), 32, 0);
        oval((float)sx + 10.0f, (float)sy + 11.0f, 6.0f, 4.0f, RGB(11, 20, 32), 32, 0);
        break;
    default:
        box(sx, sy, T, T, ((x + y) & 1) ? C_SNOW1 : C_SNOW2);
        if (h % 3u == 0u) box(sx + (int32_t)(h % 14u), sy + (int32_t)(h % 13u) + 3, 4, 1, RGB(211, 224, 236));
        if (h % 7u == 0u) dot(sx + (int32_t)(h % 17u), sy + (int32_t)(h % 15u), RGB(255, 255, 255));
        break;
    }
}

/* ------------------------------------------------------------------ the monuments (world pixels, then the camera) --- */
static void draw_tree(int32_t sx, int32_t sy)        /* sx, sy: the tile's corner */
{
    static const struct { int8_t dy, w, h; uint16_t c; } L[3] = {{0, 20, 10, RGB(44, 88, 71)}, {-7, 15, 9, RGB(51, 102, 80)}, {-13, 10, 8, RGB(60, 115, 90)}};
    int i;
    box_a(sx + 2, sy + 17, 16, 3, C_SHADOW, 7);
    box(sx + 9, sy + 13, 3, 6, RGB(90, 59, 42));
    for (i = 0; i < 3; i++) {
        float cx = (float)sx + 10.0f, by = (float)(sy + 14 + L[i].dy), w = (float)L[i].w, h = (float)L[i].h;
        tri(cx - w / 2.0f, by, cx + w / 2.0f, by, cx, by - h, L[i].c, 32);
        box((int32_t)(cx - w / 4.0f), (int32_t)(by - h + 3.0f), (int32_t)(w / 2.0f), 2, RGB(242, 248, 252));
    }
}
static void draw_igloo_out(int32_t cx, int32_t by, int night)   /* cx: the middle, by: the dome's foot */
{
    int r;
    oval((float)cx, (float)by, 33, 32, RGB(200, 217, 231), 32, 1);
    oval((float)cx, (float)by - 1.0f, 31, 30, RGB(242, 247, 251), 32, 1);
    for (r = 6; r < 30; r += 7) {
        float d = (float)r / 30.0f, w = 31.0f * fm_sqrtf(fm_maxf(0.0f, 1.0f - d * d));
        span(cx - (int32_t)w + 1, cx + (int32_t)w, by - r, RGB(200, 214, 228), 32);
    }
    box(cx - 9, by - 12, 18, 12, RGB(230, 238, 245));
    oval((float)cx, (float)by, 6, 10, night ? RGB(255, 190, 110) : RGB(43, 58, 76), night ? A32(0.85f + pulse * 0.15f) : 32, 1);
    if (night) box_a(cx - 7, by, 14, 12, RGB(255, 190, 110), 7);
}
static void draw_boat(int32_t cx, int32_t by, float time)    /* by: the hull's foot */
{
    int i;
    for (i = -72; i < 72; i += 4) {
        int32_t lift = (int32_t)((float)i * 0.08f);
        box(cx + i, by - 22 - lift, 4, 16, RGB(122, 47, 42));
        box(cx + i, by - 6 - lift, 4, 5, RGB(91, 33, 30));
        box(cx + i, by - 24 - lift, 4, 3, RGB(244, 248, 252));
    }
    box(cx - 16, by - 46, 40, 22, RGB(233, 225, 212));
    for (i = 0; i < 3; i++) box(cx - 11 + i * 12, by - 41, 7, 7, RGB(61, 90, 115));
    box(cx - 18, by - 48, 44, 3, RGB(244, 248, 252));
    box(cx - 48, by - 76, 3, 52, RGB(74, 58, 48));
    box(cx + 38, by - 66, 3, 42, RGB(74, 58, 48));
    box(cx - 47 + (int32_t)(fm_sinf(time * 1.3f) * 3.0f), by - 72, 1, 30, RGB(139, 122, 102));
    box(cx - 4, by - 2, 2, 16, RGB(74, 58, 48));      /* the ladder down to the door */
    box(cx + 4, by - 2, 2, 16, RGB(74, 58, 48));
    for (i = 0; i < 14; i += 4) box(cx - 4, by + i, 10, 1, RGB(74, 58, 48));
}
static void draw_cave(int32_t cx, int32_t by)
{
    int i;
    oval((float)cx, (float)by, 36, 44, RGB(156, 196, 220), 32, 1);
    oval((float)cx - 4.0f, (float)by - 4.0f, 28, 34, RGB(196, 226, 242), 32, 1);
    oval((float)cx, (float)by, 9, 16, RGB(13, 29, 44), 32, 1);
    for (i = 0; i < 5; i++) box(cx - 24 + i * 12, by - 34 + (i % 2) * 6, 3, 8, RGB(233, 247, 255));
}
static void draw_fire(int32_t sx, int32_t sy, float time)    /* sy: the logs */
{
    int i;
    for (i = 0; i < 7; i++) {
        float a = (float)i / 7.0f * 6.2831853f;
        box(sx + (int32_t)(fm_cosf(a) * 9.0f) - 2, sy + (int32_t)(fm_sinf(a) * 4.0f) - 1, 4, 3, (i & 1) ? RGB(139, 152, 163) : RGB(111, 124, 136));
    }
    box(sx - 7, sy - 1, 14, 3, RGB(90, 59, 42));
    box(sx - 2, sy - 4, 4, 7, RGB(74, 47, 34));
    {
        float h = 11.0f + fm_sinf(time * 11.0f) * 2.0f + fm_sinf(time * 17.0f) * 1.5f + pulse * 6.0f;
        static const struct { uint16_t c; float w, k; } F[3] = {{RGB(214, 69, 42), 7, 1}, {RGB(255, 138, 58), 5, 0.78f}, {RGB(255, 215, 106), 3, 0.5f}};
        for (i = 0; i < 3; i++) {
            float sway = fm_sinf(time * 6.0f + (float)i) * 1.5f;
            tri((float)sx - F[i].w, (float)sy, (float)sx + F[i].w, (float)sy, (float)sx + sway * 1.5f, (float)sy - h * F[i].k, F[i].c, 32);
        }
    }
}
static float phare_ang;
static void draw_phare(int32_t cx, int32_t by)
{
    int i;
    oval((float)cx, (float)by, 18, 6, RGB(125, 138, 150), 32, 0);
    oval((float)cx - 2.0f, (float)by - 2.0f, 13, 4, RGB(167, 179, 190), 32, 0);
    for (i = 0; i < 6; i++) {
        float w = 12.0f - (float)i * 0.8f;
        box(cx - (int32_t)(w / 2.0f), by - 4 - (i + 1) * 7, (int32_t)w, 7, (i & 1) ? RGB(244, 241, 234) : RGB(200, 70, 58));
    }
    box(cx - 8, by - 50, 16, 2, RGB(42, 36, 48));
    glow(cx, by - 54, 16, RGB(255, 236, 160), 0.25f);
    box_a(cx - 5, by - 58, 10, 8, RGB(255, 232, 150), 10);
    box(cx - 6, by - 61, 12, 3, RGB(42, 36, 48));
    box(cx - 1, by - 64, 2, 3, RGB(42, 36, 48));
}
static void draw_beams(int32_t lx, int32_t ly, int night)
{
    int b, seg;
    float alpha = 0.1f * (night ? 1.5f : 0.85f);
    uint16_t col = night ? RGB(255, 240, 180) : RGB(255, 196, 90);
    if (lx < -200 || lx > 320 || ly < -200 || ly > 320) return;
    for (b = 0; b < 2; b++) {
        float a = phare_ang + (b ? 3.14159265f : 0.0f), L = 170.0f, w = 0.14f;
        for (seg = 0; seg < 4; seg++) {
            float r0 = L * (float)seg / 4.0f, r1 = L * (float)(seg + 1) / 4.0f, k = 1.0f - (float)seg / 4.0f;
            tri((float)lx + fm_cosf(a - w) * r0, (float)ly + fm_sinf(a - w) * r0 * 0.6f, (float)lx + fm_cosf(a - w) * r1, (float)ly + fm_sinf(a - w) * r1 * 0.6f,
                (float)lx + fm_cosf(a + w) * r1, (float)ly + fm_sinf(a + w) * r1 * 0.6f, col, A32(alpha * k));
            tri((float)lx + fm_cosf(a - w) * r0, (float)ly + fm_sinf(a - w) * r0 * 0.6f, (float)lx + fm_cosf(a + w) * r0, (float)ly + fm_sinf(a + w) * r0 * 0.6f,
                (float)lx + fm_cosf(a + w) * r1, (float)ly + fm_sinf(a + w) * r1 * 0.6f, col, A32(alpha * k));
        }
    }
}
static void draw_seals(int32_t sx, int32_t sy, float time)   /* sx, sy: the rocks' left foot */
{
    int i;
    for (i = 0; i < 4; i++) oval((float)(sx + 10 + i * 20), (float)sy - 6.0f, 12, 8, (i & 1) ? RGB(125, 138, 150) : RGB(139, 152, 163), 32, 0);
    for (i = 0; i < 3; i++) {
        int32_t x = sx + 18 + i * 22, y = sy - 14 - (int32_t)(fm_fabsf(fm_sinf(time * 1.2f + (float)i * 2.0f)) * 2.0f);
        oval((float)x, (float)y, 7, 4, RGB(96, 112, 128), 32, 0);
        oval((float)x + 5.0f, (float)y - 3.0f, 3.5f, 3, RGB(96, 112, 128), 32, 0);
        dot(x + 6, y - 4, RGB(20, 24, 30));
    }
}
static void draw_hole(int32_t cx, int32_t cy, float time)
{
    int i;
    oval((float)cx, (float)cy, 42, 33, RGB(184, 208, 226), 32, 0);
    oval((float)cx, (float)cy, 38, 29, RGB(18, 58, 92), 32, 0);
    for (i = 0; i < 6; i++)
        box_a(cx - 26 + (i % 3) * 20 + (int32_t)(fm_sinf(time + (float)i) * 3.0f), cy - 14 + (i / 3) * 16, 8, 1, RGB(120, 190, 240), A32(0.25f + pulse * 0.3f));
}
static void draw_rink(int32_t cx, int32_t cy)
{
    int i;
    for (i = 0; i < 3; i++) oring((float)cx, (float)cy, RINK_RX + 2.0f + (float)i, RINK_RY + 2.0f + (float)i, RGB(247, 251, 255), 32);
    oring((float)cx, (float)cy, RINK_RX + 5.0f, RINK_RY + 5.0f, RGB(160, 190, 215), 22);
    oring((float)cx, (float)cy, RINK_RX * 0.5f, RINK_RY * 0.5f, RGB(255, 255, 255), 12);
}

/* ------------------------------------------------------------------ outside --- */
typedef struct { int32_t y; uint8_t kind; int16_t a, b; } obj_t;
enum { O_BOAT, O_IGLOO, O_CAVE, O_FIRE, O_PHARE, O_SEALS, O_DOG, O_PLAYER, O_TREE };
#define NOBJ 64
static obj_t objs[NOBJ];
static int nobj;
static void obj_add(int32_t y, int kind, int a, int b)
{
    if (nobj < NOBJ) { objs[nobj].y = y; objs[nobj].kind = (uint8_t)kind; objs[nobj].a = (int16_t)a; objs[nobj].b = (int16_t)b; nobj++; }
}
static struct { float x, y, s; } flakes[60];
static struct { uint8_t x, y; float p; } stars[30];

static void camera(void)
{
    float tx = 0.0f, ty = 0.0f;
    if (P.moving || gliding) {                /* look ahead, where you are going */
        tx = P.dir == D_RIGHT ? 16.0f : P.dir == D_LEFT ? -16.0f : 0.0f;
        ty = P.dir == D_DOWN ? 14.0f : P.dir == D_UP ? -14.0f : 0.0f;
    }
    look_x += (tx - look_x) * 0.04f;
    look_y += (ty - look_y) * 0.04f;
    if (scene == SC_RIVER) { cam_x = fm_clampf(P.x - 60.0f + look_x, 0.0f, RIV_LEN - VIEW); cam_y = 0.0f; return; }
    cam_x = fm_clampf(P.x - 60.0f + look_x, 0.0f, (float)(MW * T - VIEW));
    cam_y = fm_clampf(P.y - 72.0f + look_y, 0.0f, (float)(MH * T - VIEW));
}

static void draw_outside(float time)
{
    float na = weather_mode == 3 ? 0.0f : night_amt();
    int night = na > 0.5f, storm = weather == 2;
    int32_t cx = (int32_t)cam_x, cy = (int32_t)cam_y, x, y, i;
    int tx0 = cx / T, ty0 = cy / T;
    for (y = ty0; y <= ty0 + VIEW / T; y++)
        for (x = tx0; x <= tx0 + VIEW / T; x++) tile(at(x, y), x, y, x * T - cx, y * T - cy, time);
    {   /* footprints; lit when the echo plays them */
        uint32_t life = storm ? 9000u : 45000u;
        for (i = 0; i < print_count; i++) {
            int k = (print_head - 1 - i + PRINT_N) % PRINT_N;
            uint32_t age = frame_ms - prints[k].t;
            if (age >= life) continue;
            if (prints[k].lit && frame_ms - prints[k].lit < 250u) box((int32_t)prints[k].x - cx - 1, (int32_t)prints[k].y - cy, 3, 2, C_GOLD);
            else box_a((int32_t)prints[k].x - cx - 1, (int32_t)prints[k].y - cy, 3, 2, RGB(150, 175, 200), A32(0.6f * (1.0f - (float)age / (float)life)));
        }
        for (i = 0; i < NBLADE; i++) {
            uint32_t age = frame_ms - blades[i].t;
            if (blades[i].t && age < 20000u) dot_a((int32_t)blades[i].x - cx, (int32_t)blades[i].y - cy, RGB(255, 255, 255), A32(0.8f * (1.0f - (float)age / 20000.0f)));
        }
    }
    draw_hole((int32_t)(9.5f * T) - cx, (int32_t)(20.5f * T) - cy, time);
    draw_rink((int32_t)RINK_CX - cx, (int32_t)RINK_CY - cy);
    nobj = 0;
    obj_add(4 * T, O_BOAT, 0, 0);
    obj_add(15 * T, O_IGLOO, 0, 0);
    obj_add(24 * T, O_CAVE, 0, 0);
    obj_add(FIRE_Y * T + 14, O_FIRE, 0, 0);
    obj_add((PHARE_TY + 1) * T, O_PHARE, 0, 0);
    obj_add(3 * T, O_SEALS, 0, 0);
    obj_add((int32_t)DOG.y, O_DOG, 0, 0);
    obj_add((int32_t)P.y, O_PLAYER, 0, 0);
    for (y = ty0 - 1; y <= ty0 + VIEW / T + 1; y++)
        for (x = tx0; x <= tx0 + VIEW / T; x++)
            if (at(x, y) == TREE) obj_add(y * T + 18, O_TREE, x, y);
    for (i = 1; i < nobj; i++) {
        obj_t o = objs[i];
        int j = i;
        while (j > 0 && objs[j - 1].y > o.y) { objs[j] = objs[j - 1]; j--; }
        objs[j] = o;
    }
    for (i = 0; i < nobj; i++) {
        obj_t *o = &objs[i];
        switch (o->kind) {
        case O_BOAT: draw_boat(18 * T + T / 2 - cx, 4 * T - cy, time); break;
        case O_IGLOO: draw_igloo_out(15 * T + T / 2 - cx, 15 * T - cy, night); break;
        case O_CAVE: draw_cave(25 * T + T / 2 - cx, 24 * T - cy); break;
        case O_FIRE: draw_fire(FIRE_X * T + T / 2 - cx, FIRE_Y * T + 14 - cy, time); break;
        case O_PHARE: draw_phare(PHARE_TX * T + T - cx, (PHARE_TY + 1) * T - cy); break;
        case O_SEALS: draw_seals(5 * T - cx, 3 * T - cy, time); break;
        case O_DOG: draw_dog((int32_t)DOG.x - cx, (int32_t)DOG.y - cy, time); break;
        case O_PLAYER: draw_player((int32_t)P.x - cx, (int32_t)P.y - cy); break;
        case O_TREE: draw_tree(o->a * T - cx, o->b * T - cy); break;
        default: break;
        }
    }
    if (na > 0.02f) {
        box_a(0, 0, VIEW, VIEW, RGB(6, 12, 38), A32(0.55f * na));
        if (na > 0.5f && na < 0.95f) box_a(0, 0, VIEW, VIEW, RGB(255, 140, 90), A32(0.12f * (1.0f - fm_fabsf(na - 0.7f) * 4.0f)));
        for (i = 0; i < 30; i++) dot_a(stars[i].x, stars[i].y, RGB(255, 255, 255), A32((0.3f + 0.5f * fm_fabsf(fm_sinf(time + stars[i].p))) * na));
        {
            int band;
            for (band = 0; band < 3; band++)
                for (x = 0; x < VIEW; x++) {
                    float yy = 6.0f + (float)band * 7.0f + fm_sinf((float)x * 0.06f + time * 0.4f + (float)band) * 5.0f + fm_sinf((float)x * 0.022f - time * 0.2f) * 4.0f;
                    int32_t hh = 8 + (int32_t)(fm_sinf((float)x * 0.1f + time) * 3.0f);
                    box_a(x, (int32_t)yy, 1, hh, band == 1 ? RGB(255, 120, 200) : RGB(90, 255, 170), A32((band == 1 ? 0.09f + pulse * 0.12f : 0.13f + pulse * 0.16f) * na));
                }
        }
    }
    if (storm) box_a(0, 0, VIEW, VIEW, RGB(225, 235, 245), 11);
    {
        int32_t fx = FIRE_X * T + T / 2 - cx, fy = FIRE_Y * T + 10 - cy;
        if (fx > -60 && fx < 180 && fy > -60 && fy < 180) {
            glow(fx, fy, 34 + (int32_t)(pulse * 6.0f + fm_sinf(time * 9.0f) * 1.5f), RGB(255, 170, 80), night ? 0.45f : 0.22f);
            if (night) draw_fire(fx, fy + 4, time);
        }
    }
    {   /* the halo: the world fades at the edges, more in a storm and when cold */
        float inner = storm ? 26.0f : 48.0f - cold * 12.0f, amax = 0.55f + cold * 0.25f + na * 0.15f;
        uint16_t hc = RGB((int)(236.0f + (8.0f - 236.0f) * na), (int)(244.0f + (14.0f - 244.0f) * na), (int)(250.0f + (34.0f - 250.0f) * na));
        uint8_t lut[128];
        int32_t d;
        for (d = 0; d < 128; d++) lut[d] = (uint8_t)A32(amax * clamp01(((float)d - inner) / (88.0f - inner)));
        for (y = 0; y < VIEW; y++)
            for (x = 0; x < VIEW; x++) {
                uint32_t a = lut[dist_at(x - 60, y - 60) & 127];
                if (a) { uint16_t *p = &FB[y * VIEW + x]; *p = swap16(blend565(swap16(*p), hc, a)); }
            }
    }
    draw_beams(PHARE_TX * T + T - cx, (PHARE_TY + 1) * T - 54 - cy, night);
}

/* ------------------------------------------------------------------ home: one screen --- */
static void draw_igloo_in(float time)
{
    int lit = latched >= 0 && synth_on, i, k;
    int32_t x, y;
    box(0, 0, VIEW, VIEW, RGB(23, 20, 28));
    oval(ROOM_CX, ROOM_CY, ROOM_R + 7.0f, ROOM_R + 7.0f, RGB(185, 203, 218), 32, 0);
    oval(ROOM_CX, ROOM_CY, ROOM_R + 3.0f, ROOM_R + 3.0f, RGB(213, 226, 236), 32, 0);
    for (y = (int32_t)(ROOM_CY - ROOM_R); y <= (int32_t)(ROOM_CY + ROOM_R); y++) {
        float d = ((float)y - ROOM_CY) / ROOM_R;
        int32_t w, kk = y / 8;
        if (d * d > 1.0f) continue;
        w = (int32_t)(ROOM_R * fm_sqrtf(1.0f - d * d));
        span((int32_t)ROOM_CX - w, (int32_t)ROOM_CX + w, y, (kk & 1) ? RGB(167, 125, 92) : RGB(176, 133, 98), 32);
        if ((y % 8) == 7) span((int32_t)ROOM_CX - w, (int32_t)ROOM_CX + w, y, RGB(125, 90, 66), 32);
    }
    for (y = 14; y < 116; y++)                       /* a warm light in the middle */
        for (x = 10; x < 110; x++) {
            int32_t d = dist_at(x - 60, y - 66);
            if (d < 44 && (x - 60) * (x - 60) + (y - 64) * (y - 64) < 50 * 50)
                dot_a(x, y, RGB(255, 205, 140), A32((0.22f + pulse * 0.15f) * (1.0f - (float)d / 44.0f)));
        }
    /* the aquarium, at the back: decoration */
    box(35, 16, 50, 22, RGB(106, 74, 56));
    box(38, 19, 44, 16, RGB(134, 205, 239));
    box(38, 28, 44, 7, RGB(47, 111, 158));
    box(38, 33, 44, 2, RGB(217, 199, 154));
    for (i = 0; i < 3; i++) {
        float fx = 42.0f + fmod_pos(time * (4.0f + (float)i * 1.5f) + (float)i * 13.0f, 36.0f), fy = 22.0f + (float)(i * 4) + fm_sinf(time + (float)i) * 1.0f;
        uint16_t fc = i == 0 ? RGB(255, 176, 122) : i == 1 ? RGB(255, 213, 106) : RGB(255, 122, 138);
        box((int32_t)fx - 2, (int32_t)fy, 5, 2, fc); box((int32_t)fx + 3, (int32_t)fy - 1, 1, 4, fc);
    }
    box_a(39, 20, 10, 1, RGB(255, 255, 255), 18);
    /* the synth in the middle: a small piece of furniture; its keys light on the chord */
    box(45, 52, 30, 14, RGB(94, 66, 50));
    box(47, 54, 26, 10, RGB(42, 36, 48));
    box(49, 55, 8, 2, lit ? RGB(127, 224, 200) : synth_on ? RGB(43, 74, 70) : RGB(14, 27, 28));
    box(68, 55, 3, 2, synth_on ? RGB(255, 154, 92) : RGB(58, 44, 34));
    {
        static const int WH[7] = {0, 2, 4, 5, 7, 9, 11};
        for (i = 0; i < 7; i++) {
            int on = 0;
            for (k = 0; lit && k < n_tones; k++) if (chord_tones[k] == WH[i]) on = 1;
            box(49 + i * 3, 58, 2, 5, on ? RGB(255, 201, 138) : RGB(255, 255, 255));
        }
    }
    if (lit && ((frame_ms / 300u) & 1u)) { box(58 + (int32_t)((frame_ms / 600u) % 3u) * 3, 46 - (int32_t)((frame_ms / 100u) % 6u), 2, 2, C_GOLD); }
    /* the groove box (left): something is missing */
    box(12, 52, 19, 18, RGB(106, 74, 56));
    box(14, 54, 15, 12, RGB(42, 36, 48));
    box(14, 54, 15, 2, RGB(214, 69, 42));
    for (i = 0; i < 4; i++) box(15 + (i % 2) * 7, 58 + (i / 2) * 4, 5, 3, RGB(90, 82, 98));
    /* the radio (right) */
    box(91, 52, 18, 16, RGB(106, 74, 56));
    oval(98, 60, 5, 5, RGB(217, 199, 168), 32, 0);
    oring(98, 60, 2.5f, 2.5f, RGB(138, 106, 74), 32);
    box(105, 55, 2, 2, RGB(58, 44, 34));
    /* the door */
    box(53, 108, 14, 12, RGB(15, 13, 20));
    box(51, 106, 2, 10, RGB(185, 203, 218)); box(67, 106, 2, 10, RGB(185, 203, 218));
    if (DOG.found) draw_dog((int32_t)DOG.x, (int32_t)DOG.y, time);
    draw_player((int32_t)P.x, (int32_t)P.y);
}

/* ------------------------------------------------------------------ under the ice --- */
static void draw_river(float time)
{
    int32_t cx = (int32_t)cam_x, x, k;
    for (x = 0; x < VIEW; x++) {
        float wx = (float)(cx + x), tp = riv_top(wx), bt = riv_bot(wx);
        int32_t t = (int32_t)tp, b = (int32_t)bt;
        span(x, x + 1, 0, RGB(111, 166, 204), 32); box(x, 0, 1, t, RGB(111, 166, 204));
        box(x, t - 1, 1, 1, RGB(205, 238, 255));
        box(x, t, 1, b - t, RGB(22, 63, 96));
        box(x, b, 1, VIEW - b, RGB(90, 143, 182));
        box(x, b, 1, 1, RGB(183, 226, 247));
    }
    for (k = cx / 20; k <= (cx + VIEW) / 20; k++) {         /* icicles above, bubbles of ice below */
        uint32_t h = (uint32_t)k * 2654435761u;
        int32_t sx = k * 20 + (int32_t)(h % 15u) - cx;
        box_a(sx, (int32_t)riv_top((float)(k * 20)) - 8 - (int32_t)(h % 8u), 1, 4, RGB(220, 250, 255), 18);
        box_a(sx + 4, (int32_t)riv_bot((float)(k * 20)) + 5 + (int32_t)(h % 7u), 1, 3, RGB(220, 250, 255), 18);
    }
    for (k = 0; k < 24; k++) {                              /* the current */
        float wx = fmod_pos((float)(k * 53) + time * (15.0f + (float)(k % 3) * 6.0f), RIV_LEN);
        int32_t sx = (int32_t)wx - cx;
        float tp = riv_top(wx), bt = riv_bot(wx);
        if (sx < -6 || sx > VIEW) continue;
        box_a(sx, (int32_t)(tp + 4.0f + fmod_pos((float)(k * 37), fm_maxf(8.0f, bt - tp - 8.0f))), 5, 1, RGB(150, 210, 255), 7);
    }
    for (k = 0; k < NRIPPLE; k++) {
        float a = (float)(frame_ms - ripples[k].t) / 1200.0f;
        if (!ripples[k].t || a >= 1.0f) continue;
        oring(ripples[k].x - (float)cx, ripples[k].y, 2.0f + a * 9.0f, 1.0f + a * 3.0f, RGB(200, 245, 255), A32(0.8f * (1.0f - a)));
    }
    for (k = 0; k < 2; k++) {                               /* the ropes, up to the two holes */
        float wx = k ? RIV_LEN - 25.0f : 25.0f;
        int32_t lx = (int32_t)wx - cx, top = (int32_t)riv_top(wx) + 16, yy;
        if (lx < -20 || lx > VIEW + 20) continue;
        tri((float)lx - 3.0f, 0.0f, (float)lx + 3.0f, 0.0f, (float)lx + 10.0f, 100.0f, RGB(255, 255, 230), 4);
        tri((float)lx - 3.0f, 0.0f, (float)lx - 10.0f, 100.0f, (float)lx + 10.0f, 100.0f, RGB(255, 255, 230), 4);
        box(lx, 0, 1, top, RGB(201, 168, 106));
        for (yy = 4; yy < top; yy += 5) box(lx - 1, yy, 3, 1, RGB(168, 134, 74));
    }
    draw_player((int32_t)P.x - cx, (int32_t)P.y);
}

/* ------------------------------------------------------------------ the screen: the world twice the size, then big text --- */
static void upscale(void)
{
    int32_t x, y;
    for (y = 0; y < VIEW; y++) {
        const uint16_t *s = &lo[y * VIEW];
        uint16_t *d0 = &cv_px[(2 * y) * 240], *d1 = d0 + 240;
        for (x = 0; x < VIEW; x++) { uint16_t c = s[x]; d0[2 * x] = d0[2 * x + 1] = d1[2 * x] = d1[2 * x + 1] = c; }
    }
}
static void big_text(int32_t y, const char *s, uint16_t ink)
{
    const felucca_font_t *f = &FONT_M;
    int32_t w = text_w(f, s);
    if (w > 226) { f = &FONT_B; w = text_w(f, s); }
    box_a(120 - w / 2 - 8, y, w + 16, (int32_t)f->h + 4, C_DARK, 22);
    cv_text(120 - w / 2, y + 2, f, s, ink);
}
/* an arrow at the screen's edge, toward the next place to find */
static void edge_arrow(void)
{
    int i, tgt = -1;
    float bd = 1e9f, a, ex, ey, k;
    int32_t ax, ay;
    if (scene != SC_OUT) return;
    for (i = 0; i < NPLACE; i++)
        if (!(discovered & (1u << i))) { float d = hyp(PLACES[i].cx - P.x / T, PLACES[i].cy - P.y / T); if (d < bd) { bd = d; tgt = i; } }
    if (tgt < 0 || bd < 3.5f) return;
    a = atan2_approx(PLACES[tgt].cy * T - (cam_y + 60.0f), PLACES[tgt].cx * T - (cam_x + 60.0f));
    ex = fm_cosf(a); ey = fm_sinf(a);
    k = fm_minf(102.0f / fm_maxf(0.001f, fm_fabsf(ex)), 102.0f / fm_maxf(0.001f, fm_fabsf(ey)));
    ax = 120 + (int32_t)(ex * k); ay = 120 + (int32_t)(ey * k);
    if (ay < 46) ay = 46;
    if (ay > 196) ay = 196;
    {
        float px = -ey, py = ex, bob = fm_sinf((float)frame_ms * 0.006f) * 2.0f;
        float tx = (float)ax + ex * (10.0f + bob), ty = (float)ay + ey * (10.0f + bob);
        surface(cv_px, 240, 240);
        (void)tx; (void)ty;
        /* a fat arrow: a dark edge, then orange */
        tri((float)ax + ex * 15.0f, (float)ay + ey * 15.0f, (float)ax - ex * 9.0f + px * 14.0f, (float)ay - ey * 9.0f + py * 14.0f,
            (float)ax - ex * 9.0f - px * 14.0f, (float)ay - ey * 9.0f - py * 14.0f, C_DARK, 26);
        tri((float)ax + ex * (11.0f + bob), (float)ay + ey * (11.0f + bob), (float)ax - ex * 6.0f + px * 10.0f, (float)ay - ey * 6.0f + py * 10.0f,
            (float)ax - ex * 6.0f - px * 10.0f, (float)ay - ey * 6.0f - py * 10.0f, C_ORANGE, 32);
    }
}
static void draw_screen(void)
{
    const char *h;
    int i;
    upscale();
    surface(cv_px, 240, 240);
    if (scene == SC_OUT) {                    /* the snow falls on the whole screen, finer than the world */
        int storm = weather == 2;
        for (i = 0; i < (storm ? 60 : 24); i++) {
            if (storm) { flakes[i].x -= 3.2f * flakes[i].s; flakes[i].y += 1.1f * flakes[i].s; }
            else { flakes[i].y += 0.3f * flakes[i].s; flakes[i].x += fm_sinf((float)frame_ms * 0.001f + flakes[i].s * 9.0f) * 0.25f; }
            if (flakes[i].x < 0.0f) flakes[i].x += 240.0f;
            if (flakes[i].y > 240.0f) flakes[i].y -= 240.0f;
            box_a((int32_t)flakes[i].x, (int32_t)flakes[i].y, storm ? 3 : 2, 2, RGB(255, 255, 255), 28);
        }
    }
    edge_arrow();
    if ((held >= 0 && scene == SC_OUT) || (latched >= 0 && synth_on && scene == SC_IGLOO)) {
        int32_t w = text_w(&FONT_L, chord_txt);
        box_a(120 - w / 2 - 10, 196, w + 20, 36, C_DARK, 20);
        cv_text(120 - w / 2, 198, &FONT_L, chord_txt, C_GOLD);
    }
    else if ((h = hint())[0] && !tuto_shown) big_text(204, h, C_GOLD);
    if (frame_ms < ban.until && ban.text[0]) big_text(8, ban.text, C_INK);
    if (tuto_shown) {                         /* the first steps: how to walk, above and below Obsimo */
        big_text(6, "EDIT UP   PLAY DOWN", C_INK);
        big_text(36, "SEQ LEFT   REC RIGHT", C_INK);
        big_text(204, "LFO: ACTION", C_GOLD);
    }
}

/* ------------------------------------------------------------------ the frame --- */
void app_ui_init(void)
{
    int i;
    dist_init();
    for (i = 0; i < 60; i++) { flakes[i].x = frand() * 240.0f; flakes[i].y = frand() * 240.0f; flakes[i].s = 0.5f + frand(); }
    for (i = 0; i < 30; i++) { stars[i].x = (uint8_t)(frand() * 119.0f); stars[i].y = (uint8_t)(frand() * 50.0f); stars[i].p = frand() * 6.0f; }
    last_ms = plat_ms();
    lcd_fill(0, 0, 240, 240, RGB(10, 16, 24));
}

void app_frame(void)
{
    float time;
    input();
    update();
    frame_ms = plat_ms();
    time = (float)frame_ms * 0.001f;
    pulse += (fm_minf(1.0f, eng_level() * 1.5f) - pulse) * 0.2f;
    phare_ang += 6.2831853f / (4.0f * beat()) * 0.35f * 0.016f;
    if (phare_ang > 6.2831853f) phare_ang -= 6.2831853f;
    camera();
    {   /* the canvas is the whole screen and every frame covers all of it: no need to clear it, only to wait
         * for the last transfer to be done with it */
        static int begun;
        if (!begun) { cv_begin(240, 240, RGB(10, 16, 24)); begun = 1; }
        else lcd_sync();
    }
    surface(lo, VIEW, VIEW);
    if (scene == SC_OUT) draw_outside(time);
    else if (scene == SC_RIVER) draw_river(time);
    else draw_igloo_in(time);
    draw_screen();
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
    else if (!strcmp(what, "gliding")) *got = gliding;
    else if (!strcmp(what, "level")) *got = (int)(syn_level * 100.0f);
    else if (!strcmp(what, "map")) {
        int x, y;
        for (y = 0; y < MH; y++) { for (x = 0; x < MW; x++) fputc(" .=~o#T*"[at(x, y)], stderr); fputc('\n', stderr); }
        *got = 0;
    }
    else return 0;
    return 1;
}
#endif
