/* Exploding Potatoes — compiled to WebAssembly with emscripten.
 *
 * You are a farmer. Fused potatoes keep spawning on the grid — step onto
 * one to defuse it (+1 point). Every potato you miss detonates, blasting
 * its 5x5 neighborhood: other potatoes in the blast are cleared, and if
 * you are in the blast you lose a heart. Three hearts, and the fuses get
 * shorter and the spawns faster as you score.
 *
 * Potato photos and the farmer sprite sheet are baked into sprites.h
 * (see assets/CREDITS.md).
 * Explosions drive a particle system (fire, smoke, sparks, potato chunks)
 * plus a shockwave, screen flash and screen shake.
 *
 * Game logic + rendering in C. Rendering writes an RGBA byte buffer; a
 * thin JS layer (index.html) pushes it to a <canvas> each frame. Player
 * movement is event-driven (_step), fuses/spawns/explosions run on the
 * wall clock passed to _frame.
 *
 * Exports: init_game() step(int d) set_potato(int x, int y, double fuse_ms)
 *          frame(double now_ms) get_score() get_hearts() get_dead()
 *          hold(int d) get_particles() set_reduced_motion(int on)
 *          game_buf() game_w() game_h()
 */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "sprites.h"

#define GRID 21
#define CELL 32
#define W (GRID * CELL)
#define H (GRID * CELL)

#define MAXPOT   9
#define MAXBLAST 9
#define MAXPART  2048
#define MAXHEARTS 3
#define BLAST_MS   450.0
#define FLASH_MS   130.0
#define SHAKE_MS   380.0
#define SHAKE_PX   11.0
#define STEP_MIN_MS 50.0
#define MOVE_MS    130.0     /* glide time for one cell; also the held-key pace */
#define HOLD_DELAY_MS 160.0  /* a held key starts repeating after this */
#define TIMER_R    26         /* countdown ring: outer radius and width, px */
#define TIMER_W    4
#define BLAST_R    2          /* chebyshev radius of a boom: 5x5 cells */

/* colors (24-bit) */
#define BGTOPC   0x000000    /* background gradient: black ... */
#define BGBOTC   0x1c3a63    /* ... down to brown */
#define GLOWC    0x96dcff    /* soft light around the farmer (const = BBGGRR, byte0 = R) */
#define PUTC1    0x47c5e8    /* fuse young: yellow */
#define PUTC2    0x397be0    /* fuse mid: orange */
#define PUTC3    0x4545d6    /* fuse short: red */
#define SPARKC   0xb0f2ff
#define TRACKC   0xffffff    /* burnt part of the countdown ring */
#define WAVEC    0x8ce1ff    /* shockwave */
#define FLASHC   0xa0dcff

static uint8_t pixels[W * H * 4];

static int px_ = GRID / 2, py_ = GRID / 2;   /* player cell */
static int dead = 0;
static int facing = 2;                       /* 0 up, 1 right, 2 down, 3 left */
static int stride = 0;                       /* alternates the two walk frames */
static double from_x = 0.0, from_y = 0.0;    /* glide start, in cells */
static double move_t0 = -1e9;                /* glide start time, game ms */
static int held = -1;                        /* direction being held, -1 none */
static double next_hold = 0.0;               /* game ms of the next held step */
static int hearts = MAXHEARTS;
static int score = 0;

static uint8_t p_on[MAXPOT];
static int     p_x[MAXPOT], p_y[MAXPOT];
static double  p_t0[MAXPOT], p_dead[MAXPOT];   /* spawn / fuse deadline (game ms) */

static uint8_t b_on[MAXBLAST];
static int     b_x[MAXBLAST], b_y[MAXBLAST];
static double  b_t0[MAXBLAST];

static int started = 0;                      /* has frame() primed the clock? */
static double start_ms = 0.0;
static double last_now = 0.0;
static double last_step_ms = -1e9;
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
    uint8_t *p = &pixels[(y * W + x) * 4];
    for (int k = 0; k < 3; k++) {
        int src = (c >> (8 * k)) & 0xff;
        p[k] = (uint8_t)((p[k] * (255 - a) + src * a) / 255);
    }
    p[3] = 255;
}
static void addpx(int x, int y, uint32_t c, int a) {        /* additive glow */
    x += ox; y += oy;
    if (x < 0 || x >= W || y < 0 || y >= H || a <= 0) return;
    if (a > 255) a = 255;
    uint8_t *p = &pixels[(y * W + x) * 4];
    for (int k = 0; k < 3; k++) {
        int v = p[k] + (int)((c >> (8 * k)) & 0xff) * a / 255;
        p[k] = (uint8_t)(v > 255 ? 255 : v);
    }
}
/* vertical gradient, with 2x2 ordered dithering to hide color banding */
static uint32_t bg_at(int x, int y) {
    static const int dither[4] = { 0, 2, 3, 1 };
    int d = dither[(x & 1) | ((y & 1) << 1)];
    y = y < 0 ? 0 : y >= H ? H - 1 : y;               /* screen shake looks past edges */
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) {
        int a = (BGTOPC >> (8 * k)) & 0xff, b = (BGBOTC >> (8 * k)) & 0xff;
        int v4 = a * 4 + (b - a) * 4 * y / (H - 1);   /* in quarter steps */
        c |= (uint32_t)((v4 + d) / 4) << (8 * k);
    }
    return c;
}

static int potcount(void) {
    int i, n = 0;
    for (i = 0; i < MAXPOT; i++) n += p_on[i];
    return n;
}
static int maxpot(void)  { int m = 5 + score / 8;      return m > MAXPOT ? MAXPOT : m; }
static double spawnevery(void) { double v = 2000.0 - 30.0 * score; return v < 800.0 ? 800.0 : v; }
static double fuse_for(void)   { double v = 4500.0 - 120.0 * score; return v < 2200.0 ? 2200.0 : v; }
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
    const sprite_t *s = &BOMB_SPR[BOMB_FRAMES - 1];
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

static void burst(float cx, float cy) {
    emit(P_SMOKE, 40, cx, cy, 15.0f, 130.0f, 0.9f, 1.8f,  9.0f, 16.0f);
    emit(P_FIRE,  80, cx, cy, 20.0f, 290.0f, 0.3f, 0.8f,  6.0f, 15.0f);
    emit(P_CHUNK, 30, cx, cy, 80.0f, 340.0f, 0.8f, 1.4f,  3.0f,  9.0f);
    emit(P_SPARK, 90, cx, cy, 150.0f, 520.0f, 0.3f, 0.9f, 1.0f,  2.0f);
}

static void update_particles(double dt_ms) {
    float dt = (float)(dt_ms / 1000.0);
    float h = dt > 0.05f ? 0.05f : dt;       /* a stalled tab must not fling particles */
    for (int i = 0; i < MAXPART; i++) {
        part_t *q = &part[i];
        if (q->kind == P_NONE) continue;
        q->age += dt;
        if (q->age >= q->life) { q->kind = P_NONE; continue; }
        float drag = q->kind == P_FIRE ? 4.0f : q->kind == P_SPARK ? 3.0f
                   : q->kind == P_SMOKE ? 2.2f : 1.2f;
        float k = expf(-drag * h);
        q->vx *= k; q->vy *= k;
        if (q->kind == P_CHUNK)      q->vy += 520.0f * h;
        else if (q->kind == P_SPARK) q->vy += 200.0f * h;
        else if (q->kind == P_SMOKE) q->vy -= 45.0f * h;    /* smoke rises */
        q->x += q->vx * h; q->y += q->vy * h;
    }
}

/* soft round blob: alpha falls off from the center */
static void blob(float cx, float cy, float r, uint32_t col, int a, int additive) {
    int h = (int)r + 1, ix = (int)cx, iy = (int)cy;
    float r2 = r * r;
    for (int dy = -h; dy <= h; dy++)
        for (int dx = -h; dx <= h; dx++) {
            float d = (float)(dx * dx + dy * dy);
            if (d >= r2) continue;
            int aa = (int)((float)a * (1.0f - d / r2));
            if (additive) addpx(ix + dx, iy + dy, col, aa);
            else          blendpx(ix + dx, iy + dy, col, aa);
        }
}

static void draw_particles(int kind) {
    for (int i = 0; i < MAXPART; i++) {
        const part_t *q = &part[i];
        if (q->kind != kind) continue;
        float f = q->age / q->life;                   /* 0 new -> 1 gone */
        if (kind == P_SMOKE) {
            int a = (int)(150.0f * (1.0f - f) * (f < 0.2f ? f * 5.0f : 1.0f));
            blob(q->x, q->y, q->size * (1.0f + 1.6f * f), 0x585c62, a, 0);
        } else if (kind == P_FIRE) {                  /* white-hot -> orange -> red */
            uint32_t col = f < 0.35f ? blendc(0xc8f5ff, 0x1e8cff, (int)(f / 0.35f * 255))
                         : blendc(0x1e8cff, 0x0a1e96, (int)((f - 0.35f) / 0.65f * 255));
            blob(q->x, q->y, q->size * (1.0f + 0.9f * f), col, (int)(210.0f * (1.0f - f * f)), 1);
        } else if (kind == P_CHUNK) {
            int s = (int)q->size, a = f < 0.7f ? 255 : (int)(255.0f * (1.0f - f) / 0.3f);
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++) {
                    if (s > 3 && (dx == 0 || dx == s - 1) && (dy == 0 || dy == s - 1))
                        continue;                     /* knock off the corners */
                    blendpx((int)q->x + dx - s / 2, (int)q->y + dy - s / 2, q->col, a);
                }
        } else {                                      /* spark: streak along velocity */
            uint32_t col = blendc(0xdcffff, 0x1478ff, (int)(f * 255));
            int len = 2 + (int)(sqrtf(q->vx * q->vx + q->vy * q->vy) * 0.03f);
            for (int k = 0; k < len; k++) {
                float back = 0.03f * (float)k / (float)len;
                int x = (int)(q->x - q->vx * back), y = (int)(q->y - q->vy * back);
                int a = (int)(255.0f * (1.0f - f) * (1.0f - (float)k / (float)len));
                addpx(x, y, col, a);
                if (q->size > 1.5f) addpx(x + 1, y, col, a);
            }
        }
    }
}

static void explode(int i, double t) {
    int j, slot = 0, bx = p_x[i], by = p_y[i];
    p_on[i] = 0;
    for (j = 0; j < MAXBLAST; j++) {          /* free slot, else recycle the oldest */
        if (!b_on[j]) { slot = j; break; }
        if (b_t0[j] < b_t0[slot]) slot = j;
    }
    b_on[slot] = 1; b_x[slot] = bx; b_y[slot] = by; b_t0[slot] = t;
    burst((float)(bx * CELL + CELL / 2), (float)(by * CELL + CELL / 2));
    for (j = 0; j < MAXPOT; j++)
        if (j != i && p_on[j] && absv(p_x[j] - bx) <= BLAST_R && absv(p_y[j] - by) <= BLAST_R)
            p_on[j] = 0;
    if (!dead && absv(px_ - bx) <= BLAST_R && absv(py_ - by) <= BLAST_R) {
        hearts--;
        if (hearts <= 0) { hearts = 0; dead = 1; }
    }
}

/* ---------------------------------------------------------------- movement */

/* how far the current glide has got: 0 just started .. 1 arrived */
static double glide(double t) {
    double a = (t - move_t0) / MOVE_MS;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

/* The rules move a whole cell at once; only the drawing glides after it. */
static void do_move(int d, double t) {
    int nx = px_, ny = py_;
    if (d == 0) ny--; else if (d == 1) nx++; else if (d == 2) ny++; else nx--;
    facing = d;
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID) return;   /* walls block, but you turn */
    double a = glide(t);                     /* start from wherever he is drawn now */
    from_x += (px_ - from_x) * a;
    from_y += (py_ - from_y) * a;
    move_t0 = t;
    px_ = nx; py_ = ny;
    stride ^= 1;
    for (int i = 0; i < MAXPOT; i++)
        if (p_on[i] && p_x[i] == px_ && p_y[i] == py_) {   /* defused! */
            p_on[i] = 0;
            score++;
            break;
        }
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

/* Countdown ring: a bright arc for the fuse time left, over a dim full track.
 * It drains clockwise from 12 o'clock; `burnt` runs 0 (new) -> 1 (boom). */
static void timer_ring(int cx, int cy, uint32_t col, double burnt) {
    for (int dy = -TIMER_R; dy <= TIMER_R; dy++)
        for (int dx = -TIMER_R; dx <= TIMER_R; dx++) {
            int d = dx * dx + dy * dy;
            if (d > TIMER_R * TIMER_R) continue;
            if (d < (TIMER_R - TIMER_W) * (TIMER_R - TIMER_W)) {
                blendpx(cx + dx, cy + dy, col, 45);             /* faint inner glow */
                continue;
            }
            double turn = atan2((double)dx, (double)-dy) / 6.283185307179586;
            if (turn < 0) turn += 1.0;
            if (turn >= burnt) blendpx(cx + dx, cy + dy, col, 255);
            else               blendpx(cx + dx, cy + dy, TRACKC, 70);
        }
}

static void render(double t) {
    int x, y, i;
    double shake = 0.0, flash = 0.0;

    for (i = 0; i < MAXBLAST; i++) {
        if (!b_on[i]) continue;
        double age = t - b_t0[i];
        if (age < 0 || age > BLAST_MS) { b_on[i] = 0; continue; }
        if (reduced_motion) continue;
        double s = 1.0 - age / SHAKE_MS, f = 1.0 - age / FLASH_MS;
        if (s > 0 && s * s > shake) shake = s * s;
        if (f > 0 && f * f > flash) flash = f * f;
    }
    ox = (int)lround(SHAKE_PX * shake * sin(t * 0.093));
    oy = (int)lround(SHAKE_PX * shake * sin(t * 0.121 + 1.7));

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t c = bg_at(x - ox, y - oy);
            uint8_t *p = &pixels[(y * W + x) * 4];
            p[0] = c & 0xff; p[1] = (c >> 8) & 0xff; p[2] = (c >> 16) & 0xff; p[3] = 255;
        }

    draw_particles(P_SMOKE);
    draw_particles(P_FIRE);

    /* shockwave rings */
    for (i = 0; i < MAXBLAST; i++) {
        if (!b_on[i]) continue;
        double e = (t - b_t0[i]) / BLAST_MS, left = 1.0 - e;
        int cx = b_x[i] * CELL + CELL / 2, cy = b_y[i] * CELL + CELL / 2;
        double r = 8.0 + (BLAST_R + 1.0) * CELL * (1.0 - left * left * left);
        double w = 2.0 + 9.0 * left;
        int h = (int)(r + w) + 1;
        double rin = (r - w) > 0 ? (r - w) * (r - w) : 0.0, rout = r * r;
        for (int dy = -h; dy <= h; dy++)
            for (int dx = -h; dx <= h; dx++) {
                double d = dx * dx + dy * dy;
                if (d >= rin && d <= rout) addpx(cx + dx, cy + dy, WAVEC, (int)(230.0 * left));
            }
    }

    /* fused potatoes */
    for (i = 0; i < MAXPOT; i++) {
        if (!p_on[i]) continue;
        int cx = p_x[i] * CELL + CELL / 2, cy = p_y[i] * CELL + CELL / 2;
        double total = p_dead[i] - p_t0[i];
        double p = t - p_t0[i];
        if (p < 0) p = 0;
        if (p > total) p = total;
        double f = p / total;                       /* 0 young → 1 about to boom */
        uint32_t col = f < 0.34 ? PUTC1 : (f < 0.67 ? PUTC2 : PUTC3);
        double pulse = 0.5 + 0.5 * sin((t + i * 97.0) * 0.015);
        int frame_no = (int)(pulse * 3.99) + (f > 0.66 ? BOMB_FRAMES - 4 : 0);
        const sprite_t *s = &BOMB_SPR[frame_no];
        uint32_t tint = PUTC3;
        int amt = f > 0.66 ? (int)(140.0 * (f - 0.66) / 0.34) : 0;
        if (f > 0.85 && ((int)(t / 90.0) & 1)) { tint = 0xffffff; amt = 150; }  /* blink */
        draw_sprite(s, cx, cy, tint, amt);
        timer_ring(cx, cy, col, f);
        /* fuse spark on the sprout, jittering */
        int jx = cx + s->w / 5 + ((int)(t * 0.53 + i) % 3) - 1, jy = cy - s->h / 2 + 1;
        blob((float)jx, (float)jy, 4.5f, SPARKC, 230, 1);
    }

    /* you */
    {
        static const int sheet_row[4] = { 1, 3, 0, 2 };   /* up, right, down, left */
        double a = glide(t);
        int cx = (int)lround((from_x + (px_ - from_x) * a) * CELL) + CELL / 2;
        int cy = (int)lround((from_y + (py_ - from_y) * a) * CELL) + CELL / 2;
        int col = (!dead && a < 0.7) ? 1 + stride : 0;        /* stride, then plant */
        const sprite_t *s = &FARMER_SPR[sheet_row[facing] * 3 + col];
        /* he is bigger than a cell: stand him a little high, but keep him on screen */
        int sx = cx, sy = cy - 6;
        if (sx < s->w / 2) sx = s->w / 2;
        if (sx > W - s->w / 2) sx = W - s->w / 2;
        if (sy < s->h / 2) sy = s->h / 2;
        if (sy > H - s->h / 2) sy = H - s->h / 2;
        blob((float)cx, (float)cy, 38.0f, GLOWC, 80, 0);
        draw_sprite(s, sx, sy, 0x101010, dead ? 160 : 0);     /* charred */
    }

    draw_particles(P_CHUNK);
    draw_particles(P_SPARK);

    if (flash > 0.0) {
        int a = (int)(120.0 * flash);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                blendpx(x - ox, y - oy, FLASHC, a);
    }
}

/* ------------------------------------------------------------------ state */

EMSCRIPTEN_KEEPALIVE
void init_game(void) {
    px_ = GRID / 2; py_ = GRID / 2;
    dead = 0; hearts = MAXHEARTS; score = 0;
    facing = 2; stride = 0; held = -1; next_hold = 0.0;
    from_x = px_; from_y = py_; move_t0 = -1e9;
    memset(p_on, 0, sizeof p_on);
    memset(b_on, 0, sizeof b_on);
    memset(part, 0, sizeof part);
    part_next = 0;
    started = 0; start_ms = 0.0; last_now = 0.0; last_step_ms = -1e9;
    next_spawn = 700.0;
    render(0);
}

EMSCRIPTEN_KEEPALIVE
void frame(double now) {
    int i;
    if (!isfinite(now)) return;
    if (!started) {                           /* first call primes the game clock */
        started = 1; start_ms = now; last_now = now; next_spawn = 700.0;
        render(0);
        return;
    }
    if (now < last_now) now = last_now;
    update_particles(now - last_now);         /* before new blasts emit theirs */
    last_now = now;
    {
        double t = now - start_ms;
        if (!dead) {
            /* bounded catch-up: after a long stall the board can only fill once */
            for (int n = 0; n < MAXPOT && t >= next_spawn; n++) {
                if (potcount() < maxpot()) {
                    int tries = 0, j, cx, cy;
                    while (tries++ < 40) {
                        cx = (int)(rng() % GRID);
                        cy = (int)(rng() % GRID);
                        if (cx == px_ && cy == py_) continue;
                        {
                            int clash = 0;
                            for (j = 0; j < MAXPOT; j++)
                                if (p_on[j] && p_x[j] == cx && p_y[j] == cy) { clash = 1; break; }
                            if (!clash) {
                                int s = 0;
                                while (s < MAXPOT && p_on[s]) s++;
                                if (s < MAXPOT) {
                                    p_on[s] = 1; p_x[s] = cx; p_y[s] = cy;
                                    p_t0[s] = t; p_dead[s] = t + fuse_for();
                                    break;
                                }
                            }
                        }
                    }
                }
                next_spawn += spawnevery();
            }
            if (t >= next_spawn) next_spawn = t + spawnevery();
            for (i = 0; i < MAXPOT; i++)
                if (p_on[i] && t >= p_dead[i]) explode(i, t);
        }
        if (!dead && held >= 0 && t >= next_hold) {       /* after blasts: expired fuses first */
            /* keep the schedule so the walk is seamless, but never burst after a stall */
            double at = t - next_hold > MOVE_MS ? t : next_hold;
            do_move(held, at);
            last_step_ms = last_now;
            next_hold = at + MOVE_MS;
        }
        render(t);
    }
}

EMSCRIPTEN_KEEPALIVE
int step(int d) {
    if (dead || d < 0 || d > 3) return score;
    if (last_now - last_step_ms < STEP_MIN_MS) return score;
    last_step_ms = last_now;
    double t = started ? last_now - start_ms : 0.0;
    do_move(d, t);
    /* A step changed state; render it now so the buffer is never a stale frame. */
    render(t);
    return score;
}

/* A key is being held (d = 0..3) or released (anything else). frame() then keeps
 * walking that way; the first step is the caller's step(). */
EMSCRIPTEN_KEEPALIVE
void hold(int d) {
    held = (d >= 0 && d <= 3) ? d : -1;
    next_hold = (started ? last_now - start_ms : 0.0) + HOLD_DELAY_MS;
}

EMSCRIPTEN_KEEPALIVE
void set_potato(int x, int y, double fuse_ms) {          /* test/debug hook */
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) return;
    if (!isfinite(fuse_ms) || fuse_ms < 1.0) fuse_ms = 1.0;
    int s = 0;
    while (s < MAXPOT && p_on[s]) s++;
    if (s >= MAXPOT) s = 0;
    double t = started ? last_now - start_ms : 0.0;
    p_on[s] = 1; p_x[s] = x; p_y[s] = y;
    p_t0[s] = t; p_dead[s] = t + fuse_ms;
}

EMSCRIPTEN_KEEPALIVE
void set_reduced_motion(int on) { reduced_motion = on != 0; }
EMSCRIPTEN_KEEPALIVE
int get_particles(void) {
    int i, n = 0;
    for (i = 0; i < MAXPART; i++) n += part[i].kind != P_NONE;
    return n;
}
EMSCRIPTEN_KEEPALIVE
const uint8_t *game_buf(void) { return pixels; }
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