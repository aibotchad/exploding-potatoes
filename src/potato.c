/* Exploding Potatoes — compiled to WebAssembly with emscripten.
 *
 * You are a potato. Fused potatoes keep spawning on the grid — step onto
 * one to defuse it (+1 point). Every potato you miss detonates, blasting
 * its 5x5 neighborhood: other potatoes in the blast are cleared, and if
 * you are in the blast you lose a heart. Three hearts, and the fuses get
 * shorter and the spawns faster as you score.
 *
 * Game logic + rendering in C. Rendering writes an RGBA byte buffer; a
 * thin JS layer (index.html) pushes it to a <canvas> each frame. Player
 * movement is event-driven (_step), fuses/spawns/explosions run on the
 * wall clock passed to _frame.
 *
 * Exports: init_game() step(int d) set_potato(int x, int y, double fuse_ms)
 *          frame(double now_ms) get_score() get_hearts() get_dead()
 *          game_buf() game_w() game_h()
 */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define GRID 21
#define CELL 24
#define W (GRID * CELL)
#define H (GRID * CELL)

#define MAXPOT   9
#define MAXBLAST 9
#define MAXHEARTS 3
#define BLAST_MS   550.0
#define STEP_MIN_MS 50.0
#define BLAST_R    2          /* chebyshev radius of a boom: 5x5 cells */

/* colors (24-bit) */
#define BG1C     0x161927
#define BG2C     0x1b1f30
#define PLAYC    0x4cc9f2    /* your potato: golden (const = BBGGRR, byte0 = R) */
#define RIMC     0x1d5a8a
#define EYEC     0x18212b
#define PUTC1    0x47c5e8    /* fuse young: yellow */
#define PUTC2    0x397be0    /* fuse mid: orange */
#define PUTC3    0x4545d6    /* fuse short: red */
#define SPARKC   0xb0f2ff
#define BLAST_IN   0x2f7aff  /* blast interior */
#define BLAST_RING 0x66d1ff  /* blast rim */

static uint8_t pixels[W * H * 4];

static int px_ = GRID / 2, py_ = GRID / 2;   /* player cell */
static int dir = 1;
static int dead = 0;
static int hearts = MAXHEARTS;
static int score = 0;

static uint8_t p_on[MAXPOT];
static int     p_x[MAXPOT], p_y[MAXPOT];
static double  p_t0[MAXPOT], p_dead[MAXPOT];   /* spawn / fuse deadline (game ms) */

static uint8_t b_on[MAXBLAST];
static int     b_x[MAXBLAST], b_y[MAXBLAST];
static double  b_t0[MAXBLAST];

static double start_ms = -1.0;
static double last_now = 0.0;
static double last_step_ms = -1e9;
static double next_spawn = 0.0;
static uint32_t rng_state = 0xb09f5eedu;

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

static void setpx(int x, int y, uint32_t c) {
    int o = (y * W + x) * 4;
    pixels[o + 0] = (uint8_t)(c & 0xff);
    pixels[o + 1] = (uint8_t)((c >> 8) & 0xff);
    pixels[o + 2] = (uint8_t)((c >> 16) & 0xff);
    pixels[o + 3] = 255;
}
static uint32_t bg_at(int x, int y) {
    return (((x >> 1) ^ (y >> 1)) & 1) ? BG2C : BG1C;
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

static void explode(int i, double t) {
    int j, bx = p_x[i], by = p_y[i];
    p_on[i] = 0;
    for (j = 0; j < MAXBLAST; j++)
        if (!b_on[j]) { b_on[j] = 1; b_x[j] = bx; b_y[j] = by; b_t0[j] = t; break; }
    for (j = 0; j < MAXPOT; j++)
        if (j != i && p_on[j] && absv(p_x[j] - bx) <= BLAST_R && absv(p_y[j] - by) <= BLAST_R)
            p_on[j] = 0;
    if (!dead && absv(px_ - bx) <= BLAST_R && absv(py_ - by) <= BLAST_R) {
        hearts--;
        if (hearts <= 0) { hearts = 0; dead = 1; }
    }
}

/* ------------------------------------------------------------------ render */

static void render(double t) {
    int x, y, i, h;

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            setpx(x, y, bg_at(x, y));

    /* blast rings (drawn under entities so potatoes/player stay crisp) */
    for (i = 0; i < MAXBLAST; i++) {
        if (!b_on[i]) continue;
        double age = t - b_t0[i];
        if (age < 0 || age > BLAST_MS) { b_on[i] = 0; continue; }
        double e = age / BLAST_MS;
        int cx = b_x[i] * CELL + CELL / 2, cy = b_y[i] * CELL + CELL / 2;
        int r = (int)(6 + 20.0 * e);
        h = r + 4; if (h > 34) h = 34;
        int rin = r * r, rout = (r + 3) * (r + 3);
        for (y = cy - h; y <= cy + h; y++) {
            if (y < 0 || y >= H) continue;
            for (x = cx - h; x <= cx + h; x++) {
                if (x < 0 || x >= W) continue;
                int dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
                if (d >= rin && d <= rout)
                    setpx(x, y, BLAST_RING);
                else if (d < rin)
                    setpx(x, y, blendc(BLAST_IN, bg_at(x, y), (int)(e * 255)));
            }
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
        int r = 7 + (int)(3.0 * pulse + (f > 0.66 ? 3.0 : 0.0));
        int r2 = r * r, rim2 = (r + 2) * (r + 2);
        h = r + 2;
        for (y = cy - h; y <= cy + h; y++) {
            if (y < 0 || y >= H) continue;
            for (x = cx - h; x <= cx + h; x++) {
                if (x < 0 || x >= W) continue;
                int dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
                if (d <= r2) setpx(x, y, col);
                else if (d <= rim2) setpx(x, y, blendc(col, bg_at(x, y), 170));
            }
        }
        /* fuse spark, jittering */
        int jx = cx + 3 + ((int)(t * 0.53 + i) % 3) - 1, jy = cy - 5;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++) {
                int ax = jx + dx, ay = jy + dy;
                if (ax >= 0 && ax < W && ay >= 0 && ay < H) setpx(ax, ay, SPARKC);
            }
    }

    /* you */
    {
        int cx = px_ * CELL + CELL / 2, cy = py_ * CELL + CELL / 2;
        h = 12;
        for (y = cy - h; y <= cy + h; y++) {
            if (y < 0 || y >= H) continue;
            for (x = cx - h; x <= cx + h; x++) {
                if (x < 0 || x >= W) continue;
                int dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
                if (d <= 81) setpx(x, y, PLAYC);          /* r = 9 */
                else if (d <= 121) setpx(x, y, RIMC);     /* rim ring */
            }
        }
#define EYE(dx, dy) do { \
            int ax = cx + (dx), ay = cy + (dy); \
            for (int ey = 0; ey < 2; ey++) for (int ex = 0; ex < 2; ex++) \
                if (ax + ex >= 0 && ax + ex < W && ay + ey >= 0 && ay + ey < H) \
                    setpx(ax + ex, ay + ey, EYEC); } while (0)
        EYE(-4, -2); EYE(2, -2); EYE(-1, 3);              /* two eyes + snout */
#undef EYE
    }
}

/* ------------------------------------------------------------------ state */

EMSCRIPTEN_KEEPALIVE
void init_game(void) {
    px_ = GRID / 2; py_ = GRID / 2;
    dir = 1; dead = 0; hearts = MAXHEARTS; score = 0;
    memset(p_on, 0, sizeof p_on);
    memset(b_on, 0, sizeof b_on);
    start_ms = -1.0; last_now = 0.0; last_step_ms = -1e9;
    next_spawn = 700.0;
    render(0);
}

EMSCRIPTEN_KEEPALIVE
void frame(double now) {
    int i;
    if (start_ms < 0) {                       /* first call primes the game clock */
        start_ms = now; last_now = now; next_spawn = 700.0;
        render(0);
        return;
    }
    if (now < last_now) now = last_now;
    last_now = now;
    {
        double t = now - start_ms;
        if (!dead) {
            while (t >= next_spawn) {
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
            for (i = 0; i < MAXPOT; i++)
                if (p_on[i] && t >= p_dead[i]) explode(i, t);
        }
        render(t);
    }
}

EMSCRIPTEN_KEEPALIVE
int step(int d) {
    if (dead) return score;
    if (last_now - last_step_ms < STEP_MIN_MS) return score;
    last_step_ms = last_now;
    int nx = px_, ny = py_;
    if (d == 0) ny--; else if (d == 1) nx++; else if (d == 2) ny++; else if (d == 3) nx--;
    else return score;
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID) return score;   /* walls just block */
    px_ = nx; py_ = ny; dir = d;
    for (int i = 0; i < MAXPOT; i++)
        if (p_on[i] && p_x[i] == px_ && p_y[i] == py_) {   /* defused! */
            p_on[i] = 0;
            score++;
            break;
        }
    /* A step changed state; render it now so the buffer is never a stale frame. */
    render((start_ms >= 0) ? (double)(last_now - start_ms) : 0.0);
    return score;
}

EMSCRIPTEN_KEEPALIVE
void set_potato(int x, int y, double fuse_ms) {          /* test/debug hook */
    int s = 0;
    while (s < MAXPOT && p_on[s]) s++;
    if (s >= MAXPOT) s = 0;
    double t = (start_ms >= 0) ? (last_now - start_ms) : 0.0;
    p_on[s] = 1; p_x[s] = x; p_y[s] = y;
    p_t0[s] = t; p_dead[s] = t + fuse_ms;
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