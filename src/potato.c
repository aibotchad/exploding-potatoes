/* Exploding Potatoes — compiled to WebAssembly with emscripten.
 *
 * You are a farmer. Fused potatoes keep spawning on the grid — step onto
 * one to defuse it. They come in three sizes: bigger ones score more (1, 3, 5)
 * and blast further (3x3, 5x5, 7x7). Every potato you miss detonates, blasting
 * its neighborhood: other potatoes in the blast are cleared, and if
 * you are in the blast you lose a heart. You have three hearts.
 *
 * The potato photo, farmer sprite sheet and field tiles are baked into
 * sprites.h (see assets/CREDITS.md).
 * Explosions drive a particle system (fire, smoke, sparks, potato chunks)
 * plus a shockwave, screen flash and screen shake.
 *
 * Game logic + rendering in C. Rendering writes an RGBA byte buffer; a
 * thin JS layer (index.html) pushes it to a <canvas> each frame. Movement,
 * fuses, spawns and explosions all run on the wall clock passed to _frame.
 *
 * The farmer walks freely while a direction is held (_move) and stops the
 * moment it is released. Nearby explosions knock him away from the blast.
 *
 * A round lasts one minute from the first frame: score as much as you can.
 * Potatoes arrive faster, in greater numbers and with shorter fuses as the
 * clock runs, on the same schedule for every player.
 *
 * Exports: init_game() step(int d) set_potato(int x, int y, double fuse_ms)
 *          frame(double now_ms) get_score() get_hearts() get_dead()
 *          move(double now_ms, int d) get_farmer_x() get_farmer_y()
 *          get_particles() set_reduced_motion(int on)
 *          get_time_left() get_over() get_potatoes()
 *          potato_cap(double ms) spawn_gap(double ms) fuse_length(double ms)
 *          set_sized_potato(x, y, fuse_ms, size) potato_size(i) get_defused()
 *          game_buf() game_w() game_h()
 */
#include <emscripten.h>
#include <wasm_simd128.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "sprites.h"

#define GRID 21
#define CELL 32
#define W (GRID * CELL)
#define H (GRID * CELL)

#define MAXPOT   20
#define MAXBLAST 20
#define ROUND_MS 60000.0
/* start of the round -> final second */
#define CAP_END    18         /* potatoes on the field at once (starts at 4) */
#define GAP_START  1400.0     /* ms between spawns */
#define GAP_END    300.0
#define FUSE_START 5500.0     /* ms from spawn to boom */
#define FUSE_END   3800.0
#define MAXPART  3072
#define MAXHEARTS 3
#define WALK_SPEED 320.0     /* px per second */
#define WALK_CAP_MS 100.0    /* one frame never walks further than this, even after a stall */
#define WALK_SUB    8.0      /* collision checks at least this often along the way, px */
#define STRIDE_PX  20.0      /* distance walked per animation frame */
#define KB_DECAY   12.0      /* knockback dies away at this rate, per second */
#define KB_MAX    180.0      /* furthest a pile of blasts can throw him, px */
#define FEET_UP     6        /* he is drawn this far above his position */
/* limits for his position that keep the whole sprite on screen */
#define MIN_X 24.0
#define MAX_X (W - 24.0)
#define MIN_Y 33.0
#define MAX_Y (H - 21.0)
#define SPAWN_CLEAR 44.0     /* potatoes never appear closer to him than this, px */
#define TIMER_W    4          /* countdown ring width, px */

/* colors (24-bit) */
#define PUTC1    0x47c5e8    /* fuse young: yellow (const = BBGGRR, byte0 = R) */
#define PUTC2    0x397be0    /* fuse mid: orange */
#define PUTC3    0x4545d6    /* fuse short: red */
#define SPARKC   0xb0f2ff
#define TRACKC   0x141928    /* dark outline and burnt part of the countdown ring */
#define WAVEC    0xf0fcff    /* shockwave */
#define FLASHC   0xa0dcff

/* Everything that changes with a potato's size. */
enum { SMALL, MEDIUM, BIG, SIZES };
typedef struct {
    int points, reach;                     /* score; blast = chebyshev radius */
    int ring;                              /* countdown ring radius, px */
    int smoke, fire, chunk, spark;         /* particles per explosion */
    float speed, grain;                    /* particle speed and size factors */
    double blast_ms, flash_ms, shake_ms;
    double shake_px;
    int flash_a;
    double touch;                          /* walking this close defuses it, px */
    double push;                           /* knockback distance next to the blast, px */
} potsize_t;
static const potsize_t SIZE_OF[SIZES] = {
    /*  pts reach ring  smoke fire chunk spark  speed grain  blast flash shake  px  alpha touch push */
    { 1, 1, 20,   18,  34, 12,  40,   0.62f, 0.75f,  340, 100, 260,  5.0,  70,  18,  48 },
    { 3, 2, 26,   40,  80, 30,  90,   1.00f, 1.00f,  450, 130, 380, 11.0, 120,  22,  90 },
    { 5, 3, 33,   65, 140, 55, 170,   1.38f, 1.35f,  650, 180, 560, 21.0, 150,  28, 144 },
};

static _Alignas(16) uint8_t pixels[W * H * 4];

static double fx = W / 2, fy = H / 2;        /* the farmer's position, px */
static double kx = 0.0, ky = 0.0;            /* knockback velocity, px per second */
static int dead = 0;                         /* out of hearts */
static int time_up = 0;                      /* the minute ran out */
static int facing = 2;                       /* 0 up, 1 right, 2 down, 3 left */
static int held = -1;                        /* direction being held, -1 none */
static double walked = 0.0;                  /* px since he set off, drives the walk cycle */
static int moving = 0;                       /* did his feet move last frame? */
static int defused = 0;                      /* potatoes defused this round */
static int hearts = MAXHEARTS;
static int score = 0;

static uint8_t p_on[MAXPOT];
static int     p_x[MAXPOT], p_y[MAXPOT];
static uint8_t p_size[MAXPOT];
static double  p_t0[MAXPOT], p_dead[MAXPOT];   /* spawn / fuse deadline (game ms) */

static uint8_t b_on[MAXBLAST];
static int     b_x[MAXBLAST], b_y[MAXBLAST];
static uint8_t b_size[MAXBLAST];
static double  b_t0[MAXBLAST];

static int started = 0;                      /* has frame() primed the clock? */
static double start_ms = 0.0;
static double last_now = 0.0;
static double next_spawn = 0.0;
static uint32_t rng_state = 0xb09f5eedu;
static uint32_t fx_state = 0x1234abcdu;     /* effects only: never disturbs spawns */

enum { P_NONE, P_SMOKE, P_FIRE, P_CHUNK, P_SPARK };
typedef struct {
    float x, y, vx, vy, age, life, size;     /* px, px/s, seconds, px */
    uint32_t col;
    uint8_t kind;
} part_t;
static part_t part[MAXPART];
static int part_next = 0;                    /* ring cursor: full pool recycles oldest */

static int reduced_motion = 0;               /* no shake / flash */
static int ox = 0, oy = 0;                   /* screen shake offset */
static double draw_t = 0.0;                  /* game time the next picture shows */
static int dirty = 1;                        /* state changed since the last draw */

/* State changes only mark the picture stale; game_buf() draws it. A key press
 * that advances, steps and re-paths therefore costs one draw, not several. */
static void invalidate(double t) { draw_t = t; dirty = 1; }

static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t blendc(uint32_t a, uint32_t b, int t) {   /* t: 0..255 toward b */
    uint32_t r = (uint32_t)((((a & 0xff) * (255 - t) + (b & 0xff) * t) / 255));
    uint32_t g = (uint32_t)((((a >> 8) & 0xff) * (255 - t) + ((b >> 8) & 0xff) * t) / 255);
    uint32_t bl = (uint32_t)((((a >> 16) & 0xff) * (255 - t) + ((b >> 16) & 0xff) * t) / 255);
    return r | (g << 8) | (bl << 16);
}

/* All scene drawing goes through here: applies shake, clips to the canvas. */
static void blendpx(int x, int y, uint32_t c, int a) {      /* a: 0..255 */
    x += ox; y += oy;
    if (x < 0 || x >= W || y < 0 || y >= H || a <= 0) return;
    if (a > 255) a = 255;
    uint8_t *p = &pixels[(y * W + x) * 4];     /* alpha byte is already 255 */
    int w = a + (a >> 7), v = 256 - w;         /* 0..256, so blending is a shift */
    p[0] = (uint8_t)((p[0] * v + (int)(c & 0xff) * w) >> 8);
    p[1] = (uint8_t)((p[1] * v + (int)((c >> 8) & 0xff) * w) >> 8);
    p[2] = (uint8_t)((p[2] * v + (int)((c >> 16) & 0xff) * w) >> 8);
}
static double dist_to_cell(int x, int y);
static void walk(double dist);
static void drift(double sec);

static int potcount(void) {
    int i, n = 0;
    for (i = 0; i < MAXPOT; i++) n += p_on[i];
    return n;
}
/* Difficulty depends only on the round clock, never on how well you play. */
static double progress(double t) { return t < 0 ? 0 : t > ROUND_MS ? 1 : t / ROUND_MS; }
EMSCRIPTEN_KEEPALIVE
int potato_cap(double t)     { return 4 + (int)(progress(t) * (CAP_END - 4)); }
EMSCRIPTEN_KEEPALIVE
double spawn_gap(double t)   { return GAP_START - (GAP_START - GAP_END) * progress(t); }
EMSCRIPTEN_KEEPALIVE
double fuse_length(double t) { return FUSE_START - (FUSE_START - FUSE_END) * progress(t); }
static int absv(int a) { return a < 0 ? -a : a; }

/* --------------------------------------------------------------- particles */

static float frand(float lo, float hi) {
    fx_state ^= fx_state << 13;
    fx_state ^= fx_state >> 17;
    fx_state ^= fx_state << 5;
    return lo + (hi - lo) * (float)(fx_state & 0xffff) / 65535.0f;
}

/* a random opaque pixel of the bomb photo, so debris looks like the potato */
static uint32_t chunk_color(void) {
    const sprite_t *s = &BOMB_SPR[BIG][BOMB_FRAMES - 1];
    for (int tries = 0; tries < 16; tries++) {
        int x = (int)frand(0, (float)s->w - 0.01f), y = (int)frand(0, (float)s->h - 0.01f);
        const uint8_t *p = &s->px[(y * s->w + x) * 4];
        if (p[3] > 220) return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
    }
    return 0x5a96c8;
}

static void emit(int kind, int count, float cx, float cy,
                 float v0, float v1, float life0, float life1, float s0, float s1) {
    for (int n = 0; n < count; n++) {
        part_t *q = &part[part_next];
        part_next = (part_next + 1) % MAXPART;
        float a = frand(0.0f, 6.2831853f), v = frand(v0, v1);
        q->kind = (uint8_t)kind;
        q->x = cx + frand(-5.0f, 5.0f); q->y = cy + frand(-4.0f, 4.0f);
        q->vx = cosf(a) * v; q->vy = sinf(a) * v;
        if (kind == P_CHUNK) q->vy -= 120.0f;            /* debris is thrown upward */
        q->age = 0.0f; q->life = frand(life0, life1); q->size = frand(s0, s1);
        q->col = kind == P_CHUNK ? chunk_color() : 0;
    }
}

static void burst(float cx, float cy, const potsize_t *z) {
    float v = z->speed, g = z->grain;
    emit(P_SMOKE, z->smoke, cx, cy, 15 * v, 130 * v, 0.9f, 1.8f * g + 0.1f,  9 * g, 16 * g);
    emit(P_FIRE,  z->fire,  cx, cy, 20 * v, 290 * v, 0.3f, 0.8f * g,         6 * g, 15 * g);
    emit(P_CHUNK, z->chunk, cx, cy, 80 * v, 340 * v, 0.8f, 1.4f,             3 * g,  9 * g);
    emit(P_SPARK, z->spark, cx, cy, 150 * v, 520 * v, 0.3f, 0.9f,            1.0f,   2.0f);
}

static void update_particles(double dt_ms) {
    float dt = (float)(dt_ms / 1000.0);
    float h = dt > 0.05f ? 0.05f : dt;       /* a stalled tab must not fling particles */
    static const float drag[] = { [P_SMOKE] = 2.2f, [P_FIRE] = 4.0f,
                                  [P_CHUNK] = 1.2f, [P_SPARK] = 3.0f };
    float decay[P_SPARK + 1];
    for (int kind = P_SMOKE; kind <= P_SPARK; kind++) decay[kind] = expf(-drag[kind] * h);
    for (int i = 0; i < MAXPART; i++) {
        part_t *q = &part[i];
        if (q->kind == P_NONE) continue;
        q->age += dt;
        if (q->age >= q->life) { q->kind = P_NONE; continue; }
        float k = decay[q->kind];
        q->vx *= k; q->vy *= k;
        if (q->kind == P_CHUNK)      q->vy += 520.0f * h;
        else if (q->kind == P_SPARK) q->vy += 200.0f * h;
        else if (q->kind == P_SMOKE) q->vy -= 45.0f * h;    /* smoke rises */
        q->x += q->vx * h; q->y += q->vy * h;
    }
}

/* soft round blob: alpha falls off from the center. The hottest loop in a big
 * explosion, so it clips each row once and paints four pixels at a time. The
 * one-pixel tail does the same integer math, so both give identical results. */
static void blob(float cx, float cy, float r, uint32_t col, int a, int additive) {
    int h = (int)r + 1, ix = (int)cx, iy = (int)cy;
    float r2 = r * r, scale = (float)a / r2;
    int cr = col & 0xff, cg = (col >> 8) & 0xff, cb = (col >> 16) & 0xff;
    /* two RGBA pixels as 16-bit lanes; this alpha keeps the picture opaque */
    const v128_t rgba = wasm_u16x8_make(cr, cg, cb, additive ? 0 : 255,
                                        cr, cg, cb, additive ? 0 : 255);
    for (int dy = -h; dy <= h; dy++) {
        float left = r2 - (float)(dy * dy);
        int y = iy + dy + oy;
        if (left <= 0.0f || y < 0 || y >= H) continue;
        int span = (int)sqrtf(left), mid = ix + ox;
        int x = mid - span < 0 ? 0 : mid - span;
        int x1 = mid + span >= W ? W - 1 : mid + span;
        uint8_t *p = &pixels[(y * W + x) * 4];
        for (; x + 3 <= x1; x += 4, p += 16) {
            v128_t dx = wasm_i32x4_add(wasm_i32x4_splat(x - mid), wasm_i32x4_make(0, 1, 2, 3));
            v128_t fall = wasm_f32x4_sub(wasm_f32x4_splat(left),
                              wasm_f32x4_convert_i32x4(wasm_i32x4_mul(dx, dx)));
            v128_t aa = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_mul(wasm_f32x4_splat(scale), fall));
            aa = wasm_v128_and(aa, wasm_i32x4_gt(aa, wasm_i32x4_splat(3)));  /* too faint: 0 */
            aa = wasm_i32x4_min(aa, wasm_i32x4_splat(255));
            v128_t a16 = wasm_i16x8_narrow_i32x4(aa, aa);
            v128_t src = wasm_v128_load(p), out[2];
            for (int half = 0; half < 2; half++) {
                v128_t al = half ? wasm_i16x8_shuffle(a16, a16, 2, 2, 2, 2, 3, 3, 3, 3)
                                 : wasm_i16x8_shuffle(a16, a16, 0, 0, 0, 0, 1, 1, 1, 1);
                v128_t px = half ? wasm_u16x8_extend_high_u8x16(src)
                                 : wasm_u16x8_extend_low_u8x16(src);
                if (additive) {                        /* px + col * a / 255 */
                    v128_t m = wasm_i16x8_mul(rgba, al);
                    m = wasm_i16x8_add(wasm_i16x8_add(m, wasm_i16x8_splat(1)),
                                       wasm_u16x8_shr(m, 8));
                    out[half] = wasm_i16x8_add(px, wasm_u16x8_shr(m, 8));
                } else {                               /* (px * (256-w) + col * w) >> 8 */
                    v128_t w = wasm_i16x8_add(al, wasm_u16x8_shr(al, 7));
                    v128_t v = wasm_i16x8_sub(wasm_i16x8_splat(256), w);
                    out[half] = wasm_u16x8_shr(wasm_i16x8_add(wasm_i16x8_mul(px, v),
                                                   wasm_i16x8_mul(rgba, w)), 8);
                }
            }
            wasm_v128_store(p, wasm_u8x16_narrow_i16x8(out[0], out[1]));   /* saturates */
        }
        for (; x <= x1; x++, p += 4) {
            int dx = x - mid;
            int aa = (int)(scale * (left - (float)(dx * dx)));
            if (aa < 4) continue;                       /* too faint to see */
            if (aa > 255) aa = 255;
            if (additive) {
                int v0 = p[0] + cr * aa / 255, v1 = p[1] + cg * aa / 255,
                    v2 = p[2] + cb * aa / 255;
                p[0] = (uint8_t)(v0 > 255 ? 255 : v0);
                p[1] = (uint8_t)(v1 > 255 ? 255 : v1);
                p[2] = (uint8_t)(v2 > 255 ? 255 : v2);
            } else {
                int w = aa + (aa >> 7), v = 256 - w;
                p[0] = (uint8_t)((p[0] * v + cr * w) >> 8);
                p[1] = (uint8_t)((p[1] * v + cg * w) >> 8);
                p[2] = (uint8_t)((p[2] * v + cb * w) >> 8);
            }
        }
    }
}

static void draw_particles(int kind) {
    for (int i = 0; i < MAXPART; i++) {
        const part_t *q = &part[i];
        if (q->kind != kind) continue;
        float f = q->age / q->life;                   /* 0 new -> 1 gone */
        if (kind == P_SMOKE) {
            int a = (int)(185.0f * (1.0f - f) * (f < 0.2f ? f * 5.0f : 1.0f));
            blob(q->x, q->y, q->size * (1.0f + 1.6f * f), 0x26282e, a, 0);
        } else if (kind == P_CHUNK) {
            int s = (int)q->size, a = f < 0.7f ? 255 : (int)(255.0f * (1.0f - f) / 0.3f);
            for (int dy = -1; dy <= s; dy++)                  /* dark rim */
                for (int dx = -1; dx <= s; dx++)
                    if ((dx < 0 || dx == s) != (dy < 0 || dy == s))
                        blendpx((int)q->x + dx - s / 2, (int)q->y + dy - s / 2, TRACKC, a);
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++) {
                    blendpx((int)q->x + dx - s / 2, (int)q->y + dy - s / 2, q->col, a);
                }
        } else {                                      /* spark: streak along velocity */
            uint32_t col = blendc(0xe6ffff, 0x00c8ff, (int)(f * 255));
            int len = 2 + (int)(sqrtf(q->vx * q->vx + q->vy * q->vy) * 0.03f);
            for (int k = 0; k < len; k++) {
                float back = 0.03f * (float)k / (float)len;
                int x = (int)(q->x - q->vx * back), y = (int)(q->y - q->vy * back);
                int a = (int)(255.0f * (1.0f - f) * (1.0f - (float)k / (float)len));
                blendpx(x, y, col, a);
                if (q->size > 1.5f) blendpx(x + 1, y, col, a);
            }
        }
    }
}

/* Fire in two layers, every flame's body before any flame's core, so the ball
 * has a white-yellow heart inside orange and red edges. */
static void draw_fire(void) {
    for (int layer = 0; layer < 2; layer++)
        for (int i = 0; i < MAXPART; i++) {
            const part_t *q = &part[i];
            if (q->kind != P_FIRE) continue;
            float f = q->age / q->life;
            float r = q->size * (1.0f + 0.9f * f);
            if (!layer) {                                    /* orange -> deep red */
                uint32_t col = f < 0.45f
                    ? blendc(0x0a6eff, 0x0a1eaa, (int)(f / 0.45f * 255))        /* to red */
                    : blendc(0x0a1eaa, 0x26282e, (int)((f - 0.45f) / 0.55f * 255)); /* to soot */
                blob(q->x, q->y, r * 1.1f, col, (int)(255.0f * (1.0f - f * f)), 0);
            } else if (f < 0.65f) {                          /* white-yellow -> orange */
                float g = f / 0.65f;
                blob(q->x, q->y, r * 0.72f, blendc(0xc8ffff, 0x14aaff, (int)(g * 255)),
                     (int)(255.0f * (1.0f - g * g)), 0);
            }
        }
}

static int farmer_col(void);
static int farmer_row(void);
static void knock_back(double bx, double by, const potsize_t *z);

static void explode(int i, double t) {
    int j, slot = 0, bx = p_x[i], by = p_y[i];
    p_on[i] = 0;
    for (j = 0; j < MAXBLAST; j++) {          /* free slot, else recycle the oldest */
        if (!b_on[j]) { slot = j; break; }
        if (b_t0[j] < b_t0[slot]) slot = j;
    }
    const potsize_t *z = &SIZE_OF[p_size[i]];
    b_on[slot] = 1; b_x[slot] = bx; b_y[slot] = by; b_t0[slot] = t; b_size[slot] = p_size[i];
    burst((float)(bx * CELL + CELL / 2), (float)(by * CELL + CELL / 2), z);
    for (j = 0; j < MAXPOT; j++)
        if (j != i && p_on[j] && absv(p_x[j] - bx) <= z->reach && absv(p_y[j] - by) <= z->reach)
            p_on[j] = 0;
    if (!dead && absv(farmer_col() - bx) <= z->reach && absv(farmer_row() - by) <= z->reach) {
        hearts--;
        if (hearts <= 0) { hearts = 0; dead = 1; }
    }
    knock_back(bx * CELL + CELL / 2, by * CELL + CELL / 2, z);
}

/* ---------------------------------------------------------------- movement */

static double limit_to(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* distance from the farmer to a cell, measured to the nearest spot he can stand
 * on, so potatoes in the edge rows are as easy to reach as any other */
static double dist_to_cell(int x, int y) {
    double tx = limit_to(x * CELL + CELL / 2, MIN_X, MAX_X);
    double ty = limit_to(y * CELL + CELL / 2, MIN_Y, MAX_Y);
    return hypot(fx - tx, fy - ty);
}

/* the grid cell he is standing in: blasts are judged cell by cell */
static int farmer_col(void) { return (int)(fx / CELL); }
static int farmer_row(void) { return (int)(fy / CELL); }

static void defuse_touching(void) {
    for (int i = 0; i < MAXPOT; i++)
        if (p_on[i] && dist_to_cell(p_x[i], p_y[i]) <= SIZE_OF[p_size[i]].touch) {
            p_on[i] = 0;
            score += SIZE_OF[p_size[i]].points;
            defused++;
        }
}

/* walk `dist` px the way he faces, defusing whatever he reaches */
static void walk(double dist) {
    while (dist > 1e-9) {
        double s = dist > WALK_SUB ? WALK_SUB : dist, was_x = fx, was_y = fy;
        dist -= s;
        if (facing == 0) fy -= s; else if (facing == 1) fx += s;
        else if (facing == 2) fy += s; else fx -= s;
        fx = limit_to(fx, MIN_X, MAX_X);
        fy = limit_to(fy, MIN_Y, MAX_Y);
        double gone = fabs(fx - was_x) + fabs(fy - was_y);
        if (gone > 0) { walked += gone; moving = 1; }
        defuse_touching();
    }
}

/* knockback over `sec` seconds: exact for any frame rate. Being thrown over a
 * potato does not defuse it; only walking does. */
static void drift(double sec) {
    if (kx == 0.0 && ky == 0.0) return;
    double left = exp(-KB_DECAY * sec), travel = (1.0 - left) / KB_DECAY;
    fx = limit_to(fx + kx * travel, MIN_X, MAX_X);
    fy = limit_to(fy + ky * travel, MIN_Y, MAX_Y);
    kx *= left; ky *= left;
    if (hypot(kx, ky) < 4.0) kx = ky = 0.0;
}

/* throw him away from a blast at (bx, by) px; harder the closer he is. It
 * reaches two cells beyond the damage area, so near misses are felt too. */
static void knock_back(double bx, double by, const potsize_t *z) {
    double dx = fx - bx, dy = fy - by, d = hypot(dx, dy);
    double range = (z->reach + 2.0) * CELL;
    if (d >= range) return;
    if (d < 1.0) {                           /* dead centre: thrown backwards */
        dx = facing == 1 ? -1 : facing == 3 ? 1 : 0;
        dy = facing == 2 ? -1 : facing == 0 ? 1 : 0;
    } else {
        dx /= d; dy /= d;
    }
    double near = 1.0 - hypot(fx - bx, fy - by) / range;
    double speed = z->push * (0.25 + 0.75 * near) * KB_DECAY;
    kx += dx * speed; ky += dy * speed;
    double total = hypot(kx, ky), most = KB_MAX * KB_DECAY;
    if (total > most) { kx *= most / total; ky *= most / total; }
}

/* ------------------------------------------------------------------ render */

/* photo centered on (cx, cy), optionally tinted toward `tint` by `amt` */
static void draw_sprite(const sprite_t *s, int cx, int cy, uint32_t tint, int amt) {
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const uint8_t *p = &s->px[(y * s->w + x) * 4];
            if (!p[3]) continue;
            uint32_t c = p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
            if (amt) c = blendc(c, tint, amt);
            blendpx(cx - s->w / 2 + x, cy - s->h / 2 + y, c, p[3]);
        }
}

/* Countdown ring geometry, worked out once per size: every pixel of the ring,
 * whether it is rim, and how far clockwise from 12 o'clock it sits (0..1). */
#define RING_MAX_R 33                                     /* the BIG ring */
#define RING_BOX (2 * (RING_MAX_R + 1) + 1)
static struct { int8_t dx, dy; uint8_t rim; double turn; } ring[SIZES][RING_BOX * RING_BOX];
static int ring_len[SIZES];

static void build_rings(void) {
    for (int z = 0; z < SIZES; z++) {
        int r = SIZE_OF[z].ring, outer = r + 1, inner = r - TIMER_W - 1, n = 0;
        for (int dy = -outer; dy <= outer; dy++)
            for (int dx = -outer; dx <= outer; dx++) {
                int d = dx * dx + dy * dy;
                if (d > outer * outer || d < inner * inner) continue;
                double turn = atan2((double)dx, (double)-dy) / 6.283185307179586;
                ring[z][n].dx = (int8_t)dx;
                ring[z][n].dy = (int8_t)dy;
                ring[z][n].rim = d > r * r || d < (r - TIMER_W) * (r - TIMER_W);
                ring[z][n].turn = turn < 0 ? turn + 1.0 : turn;
                n++;
            }
        ring_len[z] = n;
    }
}

/* A bright arc for the fuse time left, the rest dark, with a dark rim so it reads
 * on the light field. It drains clockwise; `burnt` runs 0 (new) -> 1 (boom). */
static void timer_ring(int cx, int cy, int z, uint32_t col, double burnt) {
    for (int i = 0; i < ring_len[z]; i++) {
        int x = cx + ring[z][i].dx, y = cy + ring[z][i].dy;
        if (ring[z][i].rim)                 blendpx(x, y, TRACKC, 200);
        else if (ring[z][i].turn >= burnt)  blendpx(x, y, col, 255);
        else                                blendpx(x, y, TRACKC, 220);
    }
}

/* ------------------------------------------------------------------- field */

static uint32_t field[W * H];                /* the ground never changes: built once */
static int field_ready = 0;

static void stamp_tile(int gx, int gy, int tile) {
    const sprite_t *s = &TILE_SPR[tile];
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const uint8_t *p = &s->px[(y * s->w + x) * 4];
            if (p[3] < 128) continue;                     /* pixel art: on or off */
            field[(gy * CELL + y) * W + gx * CELL + x] =
                0xff000000u | p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
        }
}

/* a fully ploughed field: soil rows on every line inside a grass border, with
 * some scenery round the edge */
static void build_field(void) {
    static const struct { int x, y, tile; } scenery[] = {
        { 0, 0, 3 }, { 20, 0, 4 }, { 0, 20, 5 }, { 20, 20, 3 }, { 0, 7, 6 },
        { 20, 12, 9 }, { 8, 0, 7 }, { 13, 20, 7 }, { 0, 14, 8 }, { 20, 5, 5 },
    };
    const uint8_t *g = TILE_SPR[0].px;       /* corner of a row tile is plain grass */
    uint32_t grass = 0xff000000u | g[0] | (g[1] << 8) | ((uint32_t)g[2] << 16);
    for (int i = 0; i < W * H; i++) field[i] = grass;
    for (int gy = 1; gy < GRID - 1; gy++)
        for (int gx = 1; gx < GRID - 1; gx++)
            stamp_tile(gx, gy, gx == 1 ? 0 : gx == GRID - 2 ? 2 : 1);
    for (unsigned i = 0; i < sizeof scenery / sizeof scenery[0]; i++)
        stamp_tile(scenery[i].x, scenery[i].y, scenery[i].tile);
    build_rings();
    field_ready = 1;
}

/* tint every pixel toward FLASHC: one flat loop, so it vectorises */
static void flash_screen(int a) {
    if (a <= 0) return;
    uint16_t w = (uint16_t)(a + (a >> 7)), keep[4], add[4];
    for (int k = 0; k < 4; k++) {
        keep[k] = k < 3 ? (uint16_t)(256 - w) : 256;          /* alpha unchanged */
        add[k] = k < 3 ? (uint16_t)(((FLASHC >> (8 * k)) & 0xff) * w) : 0;
    }
    for (int i = 0; i < W * H * 4; i += 4)
        for (int k = 0; k < 4; k++)
            pixels[i + k] = (uint8_t)((uint16_t)(pixels[i + k] * keep[k] + add[k]) >> 8);
}

static void render(double t) {
    int x, y, i;
    double shake = 0.0, flash = 0.0;        /* shake in px, flash in alpha */

    for (i = 0; i < MAXBLAST; i++) {
        if (!b_on[i]) continue;
        const potsize_t *z = &SIZE_OF[b_size[i]];
        double age = t - b_t0[i];
        if (age < 0 || age > z->blast_ms) { b_on[i] = 0; continue; }
        if (reduced_motion) continue;
        double s = 1.0 - age / z->shake_ms, f = 1.0 - age / z->flash_ms;
        if (s > 0 && z->shake_px * s * s > shake) shake = z->shake_px * s * s;
        if (f > 0 && z->flash_a * f * f > flash) flash = z->flash_a * f * f;
    }
    ox = (int)lround(shake * sin(t * 0.093));
    oy = (int)lround(shake * sin(t * 0.121 + 1.7));

    if (!field_ready) build_field();
    if (!ox && !oy) {
        memcpy(pixels, field, sizeof pixels);         /* RGBA, little-endian */
    } else {
        for (y = 0; y < H; y++) {             /* shake looks past the edges */
            int fy = y - oy;
            fy = fy < 0 ? 0 : fy >= H ? H - 1 : fy;
            const uint32_t *src = &field[fy * W];
            uint32_t *dst = (uint32_t *)(void *)&pixels[y * W * 4];
            int lead = ox > 0 ? ox : 0, tail = ox < 0 ? -ox : 0;
            for (x = 0; x < lead; x++) dst[x] = src[0];
            memcpy(dst + lead, src + lead - ox, (size_t)(W - lead - tail) * 4);
            for (x = W - tail; x < W; x++) dst[x] = src[W - 1];
        }
    }

    draw_particles(P_SMOKE);
    draw_fire();

    /* shockwave rings */
    for (i = 0; i < MAXBLAST; i++) {
        if (!b_on[i]) continue;
        const potsize_t *z = &SIZE_OF[b_size[i]];
        double e = (t - b_t0[i]) / z->blast_ms, left = 1.0 - e;
        int cx = b_x[i] * CELL + CELL / 2, cy = b_y[i] * CELL + CELL / 2;
        double r = 8.0 + (z->reach + 1.0) * CELL * (1.0 - left * left * left);
        double w = 2.0 + 9.0 * z->grain * left;
        int h = (int)(r + w) + 1;
        double rin = (r - w) > 0 ? (r - w) * (r - w) : 0.0, rout = r * r;
        double lim = (r + 3) * (r + 3);
        for (int dy = -h; dy <= h; dy++) {
            double dy2 = dy * dy;
            if (dy2 > lim) continue;
            int far = (int)sqrt(lim - dy2) + 1;
            int hole = dy2 < rin ? (int)sqrt(rin - dy2) - 1 : 0;   /* empty middle */
            if (far > h) far = h;
            for (int dx = -far; dx <= far; dx++) {
                if (hole > 0 && dx == 1 - hole) dx = hole;
                double d = dx * dx + dy2;
                if (d >= rin && d <= rout) blendpx(cx + dx, cy + dy, WAVEC, (int)(245.0 * left));
                else if (d > rout && d <= (r + 3) * (r + 3))
                    blendpx(cx + dx, cy + dy, TRACKC, (int)(150.0 * left));   /* dark edge */
            }
        }
    }

    /* fused potatoes */
    for (int size = BIG; size >= SMALL; size--)
    for (i = 0; i < MAXPOT; i++) {
        if (!p_on[i] || p_size[i] != size) continue;
        int cx = p_x[i] * CELL + CELL / 2, cy = p_y[i] * CELL + CELL / 2;
        double total = p_dead[i] - p_t0[i];
        double p = t - p_t0[i];
        if (p < 0) p = 0;
        if (p > total) p = total;
        double f = p / total;                       /* 0 young → 1 about to boom */
        uint32_t col = f < 0.34 ? PUTC1 : (f < 0.67 ? PUTC2 : PUTC3);
        double pulse = 0.5 + 0.5 * sin((t + i * 97.0) * 0.015);
        int frame_no = (int)(pulse * 3.99) + (f > 0.66 ? BOMB_FRAMES - 4 : 0);
        const sprite_t *s = &BOMB_SPR[size][frame_no];
        uint32_t tint = PUTC3;
        int amt = f > 0.66 ? (int)(140.0 * (f - 0.66) / 0.34) : 0;
        if (f > 0.85 && ((int)(t / 90.0) & 1)) { tint = 0xffffff; amt = 150; }  /* blink */
        draw_sprite(s, cx, cy, tint, amt);
        timer_ring(cx, cy, size, col, f);
        /* fuse spark on the sprout, jittering */
        int jx = cx + s->w / 5 + ((int)(t * 0.53 + i) % 3) - 1, jy = cy - s->h / 2 + 1;
        blob((float)jx, (float)jy, 4.5f, SPARKC, 230, 1);
    }

    /* you */
    {
        static const int sheet_row[4] = { 1, 3, 0, 2 };   /* up, right, down, left */
        static const int cycle[4] = { 1, 0, 2, 0 };          /* step, plant, step, plant */
        int col = (moving && !dead && !time_up) ? cycle[(int)(walked / STRIDE_PX) % 4] : 0;
        const sprite_t *s = &FARMER_SPR[sheet_row[facing] * 3 + col];
        /* the step frames sit one art pixel lower; lift them so his head glides level */
        draw_sprite(s, (int)lround(fx), (int)lround(fy) - FEET_UP - (col ? s->h / 18 : 0),
                    0x101010, dead ? 160 : 0);               /* charred */
    }

    draw_particles(P_CHUNK);
    draw_particles(P_SPARK);

    if (flash > 0.0) flash_screen((int)flash);
}

/* ------------------------------------------------------------------ state */

EMSCRIPTEN_KEEPALIVE
void init_game(void) {
    fx = W / 2; fy = H / 2; kx = ky = 0.0;
    dead = 0; time_up = 0; hearts = MAXHEARTS; score = 0; defused = 0;
    facing = 2; held = -1; walked = 0.0; moving = 0;
    memset(p_on, 0, sizeof p_on);
    memset(b_on, 0, sizeof b_on);
    memset(part, 0, sizeof part);
    part_next = 0;
    started = 0; start_ms = 0.0; last_now = 0.0;
    next_spawn = 700.0;
    invalidate(0);
}

EMSCRIPTEN_KEEPALIVE
void frame(double now) {
    int i;
    if (!isfinite(now)) return;
    if (!started) {                           /* first call primes the game clock */
        started = 1; start_ms = now; last_now = now; next_spawn = 700.0;
        invalidate(0);
        return;
    }
    if (now < last_now) now = last_now;
    double dt = now - last_now;
    update_particles(dt);                     /* before new blasts emit theirs */
    last_now = now;
    {
        double t = now - start_ms;
        if (!dead && !time_up) {
            /* fuses that ran out before the buzzer still count */
            double limit = t < ROUND_MS ? t : ROUND_MS;
            /* bounded catch-up: after a long stall the board can only fill once */
            for (int n = 0; n < MAXPOT && t < ROUND_MS && t >= next_spawn; n++) {
                if (potcount() < potato_cap(t)) {
                    int tries = 0, j, cx, cy;
                    while (tries++ < 40) {
                        cx = (int)(rng() % GRID);
                        cy = (int)(rng() % GRID);
                        if (dist_to_cell(cx, cy) < SPAWN_CLEAR) continue;
                        {
                            int clash = 0;
                            for (j = 0; j < MAXPOT; j++)
                                if (p_on[j] && p_x[j] == cx && p_y[j] == cy) { clash = 1; break; }
                            if (!clash) {
                                int s = 0;
                                while (s < MAXPOT && p_on[s]) s++;
                                if (s < MAXPOT) {
                                    p_on[s] = 1; p_x[s] = cx; p_y[s] = cy;
                                    int roll = (int)(rng() % 100);   /* 50 / 35 / 15 % */
                                    p_size[s] = roll < 50 ? SMALL : roll < 85 ? MEDIUM : BIG;
                                    p_t0[s] = t; p_dead[s] = t + fuse_length(t);
                                    break;
                                }
                            }
                        }
                    }
                }
                next_spawn += spawn_gap(t);
            }
            if (t >= next_spawn) next_spawn = t + spawn_gap(t);
            for (i = 0; i < MAXPOT; i++)
                if (p_on[i] && limit >= p_dead[i]) explode(i, t);
            if (t >= ROUND_MS && !dead) {         /* buzzer: the field goes quiet */
                time_up = 1;
                held = -1; moving = 0;
                memset(p_on, 0, sizeof p_on);
            }
        }
        /* movement comes after the blasts: an expired fuse cannot be walked off */
        if (dt > 0) {
            double sec = (dt > WALK_CAP_MS ? WALK_CAP_MS : dt) / 1000.0;
            drift(sec);
            moving = 0;
            if (held >= 0 && !dead && !time_up) walk(WALK_SPEED * sec);
        }
        invalidate(t);
    }
}

/* Hold direction d (0 up, 1 right, 2 down, 3 left), or release (anything else),
 * at time `now`. */
EMSCRIPTEN_KEEPALIVE
void move(double now, int d) {
    frame(now);                 /* the old direction applies right up to this instant */
    if (d < 0 || d > 3) {
        held = -1; moving = 0;
    } else if (!dead && !time_up) {
        if (held < 0) walked = 0.0;
        held = facing = d;
        moving = 1;             /* show the first stride immediately */
    }
    invalidate(started ? last_now - start_ms : 0.0);
}

EMSCRIPTEN_KEEPALIVE
int step(int d) {               /* test/debug hook: walk exactly one cell at once */
    if (dead || time_up || d < 0 || d > 3) return score;
    facing = d;
    walk(CELL);
    moving = 0;
    invalidate(started ? last_now - start_ms : 0.0);
    return score;
}

EMSCRIPTEN_KEEPALIVE
double get_farmer_x(void) { return fx; }
EMSCRIPTEN_KEEPALIVE
double get_farmer_y(void) { return fy; }

EMSCRIPTEN_KEEPALIVE
void set_sized_potato(int x, int y, double fuse_ms, int size) {   /* test/debug hook */
    if (x < 0 || x >= GRID || y < 0 || y >= GRID || size < SMALL || size > BIG) return;
    if (!isfinite(fuse_ms) || fuse_ms < 1.0) fuse_ms = 1.0;
    int s = 0;
    while (s < MAXPOT && p_on[s]) s++;
    if (s >= MAXPOT) s = 0;
    double t = started ? last_now - start_ms : 0.0;
    p_on[s] = 1; p_x[s] = x; p_y[s] = y; p_size[s] = (uint8_t)size;
    p_t0[s] = t; p_dead[s] = t + fuse_ms;
}

EMSCRIPTEN_KEEPALIVE
void set_potato(int x, int y, double fuse_ms) {          /* a medium one */
    set_sized_potato(x, y, fuse_ms, MEDIUM);
}

EMSCRIPTEN_KEEPALIVE
int potato_size(int i) { return i >= 0 && i < MAXPOT && p_on[i] ? p_size[i] : -1; }
EMSCRIPTEN_KEEPALIVE
int get_defused(void) { return defused; }
EMSCRIPTEN_KEEPALIVE
void set_reduced_motion(int on) { reduced_motion = on != 0; dirty = 1; }
EMSCRIPTEN_KEEPALIVE
int get_particles(void) {
    int i, n = 0;
    for (i = 0; i < MAXPART; i++) n += part[i].kind != P_NONE;
    return n;
}
EMSCRIPTEN_KEEPALIVE
const uint8_t *game_buf(void) {              /* up-to-date picture, RGBA */
    if (dirty) { render(draw_t); dirty = 0; }
    return pixels;
}
EMSCRIPTEN_KEEPALIVE
int game_w(void) { return W; }
EMSCRIPTEN_KEEPALIVE
int game_h(void) { return H; }
EMSCRIPTEN_KEEPALIVE
int get_score(void) { return score; }
EMSCRIPTEN_KEEPALIVE
int get_hearts(void) { return hearts; }
EMSCRIPTEN_KEEPALIVE
int get_dead(void) { return dead; }
EMSCRIPTEN_KEEPALIVE
int get_over(void) { return dead || time_up; }
EMSCRIPTEN_KEEPALIVE
int get_potatoes(void) { return potcount(); }
EMSCRIPTEN_KEEPALIVE
int get_time_left(void) {                    /* ms left in the round */
    double left = started ? ROUND_MS - (last_now - start_ms) : ROUND_MS;
    return left < 0 ? 0 : (int)ceil(left);
}