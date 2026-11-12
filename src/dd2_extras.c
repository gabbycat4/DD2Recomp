/*
 * dd2_extras.c - Double Dragon II's additions to the NESRecomp cycle host
 * (cyc_host_extras.h). Presentation and observation only: nothing here
 * changes what the emulated machine does.
 *
 *   Scroll log    --scroll-log FILE writes one line per machine frame: the
 *                 playfield X the PPU drew line --scroll-line (default 100)
 *                 with, its step, the HUD's X, the OAM extent of visible
 *                 sprites (as billy_x.lua), the smoother's X shift, the
 *                 playfield Y and its shift, and the first HUD line.
 *                 tools/scroll_pattern.py and tools/smooth_sim.c read it.
 *
 *   Overscan crop config.ini [Game] OverscanTop/Bottom/Left/Right (pixels,
 *                 default 0), or --overscan T,B,L,R for one run.
 *
 *   Smooth camera config.ini [Game] SmoothCamera, or --smooth N for one run:
 *                 0 off (default), 1 background (sprites stay where the game
 *                 drew them), 2 playfield (background and sprites together).
 *                 Shifts the playfield by up to SmoothMargin px (default
 *                 3, --smooth-margin N) in X and Y so the camera moves
 *                 evenly (dd2_smooth.c). SmoothEase = 1 (--smooth-ease 1,
 *                 default 0) also eases the camera's speed in and out,
 *                 more gently with a wider margin. The left, right and top crop grow by
 *                 the margin so no edge the game did not draw is shown; the
 *                 HUD is never shifted.
 *
 *   No sprite limit config.ini [Game] NoSpriteLimit (default 0), or
 *                 --no-sprite-limit 0|1: the NES draws at most 8 sprites per
 *                 line and DD2 takes turns when more share one (flicker).
 *                 DD2 also writes its sprite list backwards every other
 *                 frame, which swaps every overlap. This redraws the
 *                 playfield from the background and all sprites in the
 *                 same order on every frame (background capture).
 *
 *   HUD split fix config.ini [Game] HudSplitFix (default 1), --hud-split-fix
 *                 0|1: the game switches to the HUD partway along its last
 *                 playfield line, at a slightly different dot each frame, so
 *                 the right end of that line flickers black. The fix draws
 *                 that line from the playfield line above it.
 */
#include "cyc_host_extras.h"
#include "cyc_core.h"
#include "cyc_render.h"
#include "dd2_smooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    NES_W = 256,
    NES_H = 240,
    HUD_SAMPLE_LINE = 200,
    CROP_MAX = 64,
    SCROLL_H = 480,          /* playfield Y spans two nametables, 0-479 */
    SPLIT_HISTORY = 60,      /* frames the steady HUD line is taken over */
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- settings ---- */
typedef struct { int top, bottom, left, right; } Crop;
enum { SMOOTH_OFF, SMOOTH_BACKGROUND, SMOOTH_PLAYFIELD, SMOOTH_MODES };

static Crop s_crop_saved, s_crop;            /* config.ini / in effect */
static int  s_smooth_saved, s_smooth;
static int  s_split_saved = 1, s_split = 1;
enum { DEFAULT_MARGIN = DD2_SMOOTH_MARGIN };
static int  s_ease_saved, s_ease;
static int  s_unlimit_saved, s_unlimit;
static int  s_rate_saved, s_present_rate;          /* pictures per second, 0: one per frame */
static int  s_scale_saved = 4, s_scale = 4;        /* picture scale with PresentRate */
static int  s_margin_saved = DEFAULT_MARGIN, s_margin = DEFAULT_MARGIN;

/* ---- per-frame state (frame_end) ---- */
static long      s_frame;                     /* machine frames since power on */
static CycHostExtras s_extras;                /* defined at the end */
static int       s_line = 100;                /* playfield line sampled */
static Dd2Smooth s_smx, s_smy;
static int       s_ox, s_oy;                  /* shifts for the last frame's picture */
static int       s_prev_x = -1, s_prev_y = -1;
static int       s_top = -1, s_hud = -1;      /* playfield rows [s_top, s_hud) */
static int       s_hud_steady = -1;           /* latest HUD line over SPLIT_HISTORY */
static int       s_hud_hist[SPLIT_HISTORY], s_hud_n, s_hud_head;

static uint32_t s_bg[NES_W * NES_H];
static uint8_t  s_sprite[NES_W * NES_H];     /* 1 where a sprite pixel shows in the native picture */
static uint8_t  s_extra_at[NES_W * NES_H];   /* dropped sprites: 1 front, 2 behind the background */
static uint32_t s_extra[NES_W * NES_H];
static uint8_t  s_layer_at[NES_W * NES_H];   /* all sprites: 1 front, 2 behind the background */
static uint32_t s_layer[NES_W * NES_H];

/* Line y continues the playfield drawn on line y - 1: same X, next Y. */
static bool continues(int y)
{
    if (!(cyc_render_line_mask(y) & 0x08) || !(cyc_render_line_mask(y - 1) & 0x08)) return false;
    if (cyc_render_line_scroll_x(y) != cyc_render_line_scroll_x(y - 1)) return false;
    return cyc_render_line_scroll_y(y) == (cyc_render_line_scroll_y(y - 1) + 1) % SCROLL_H;
}

/* The playfield around s_line: s_top..s_hud-1, or -1 when there is none. */
static void find_playfield(void)
{
    s_top = s_hud = -1;
    if (!(cyc_render_line_mask(s_line) & 0x08)) return;
    int t = s_line, h = s_line + 1;
    while (t > 0 && continues(t)) --t;
    while (h < NES_H && continues(h)) ++h;
    if (h >= NES_H) return;                     /* no HUD below: not gameplay */
    s_top = t;
    s_hud = h;
}

static void steady_hud(void)
{
    if (s_hud < 0) { s_hud_n = 0; s_hud_head = 0; s_hud_steady = -1; return; }
    s_hud_hist[s_hud_head] = s_hud;
    s_hud_head = (s_hud_head + 1) % SPLIT_HISTORY;
    if (s_hud_n < SPLIT_HISTORY) s_hud_n++;
    int m = 0;
    for (int i = 0; i < s_hud_n; ++i) if (s_hud_hist[i] > m) m = s_hud_hist[i];
    s_hud_steady = m - s_hud > 2 ? s_hud : m;   /* a real move of the HUD: follow it */
}

/* The sprite layer of playfield rows [y0, y1), from the frame's OAM as the
 * PPU drew it. s_sprite: 1 where one of the (at most 8, first in OAM order)
 * sprites the PPU drew on that line shows in the native picture: opaque
 * pattern pixels, the left-column clip, behind-background sprites only where
 * the background was transparent. Colour comparison cannot tell a sprite
 * pixel from a background pixel of the same colour; this can.
 * With dropped: s_extra / s_extra_at get the sprites past the 8th on each
 * line (what the game's flicker hides this frame), lower OAM slots on top,
 * 1 in front of the background, 2 behind it. Returns how many such pixels. */
static int sprite_layers(int y0, int y1, bool dropped)
{
    memset(s_sprite + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    if (dropped) memset(s_extra_at + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    const uint8_t *oam = cyc_render_oam();
    const uint8_t *bg_opaque = cyc_frame_bg_opaque();
    int extra = 0;
    for (int py = y0; py < y1; ++py) {
        uint8_t mask = cyc_render_line_mask(py);
        if (!(mask & 0x10)) continue;
        bool tall = cyc_render_line_sprite16(py);
        int rows = tall ? 16 : 8, found = 0;
        uint16_t table = cyc_render_line_sprite_table(py);
        uint8_t *cov = s_sprite + py * NES_W, *at = s_extra_at + py * NES_W;
        uint32_t *ex = s_extra + py * NES_W;
        const uint8_t *opaque = bg_opaque + py * NES_W;
        for (int slot = 0; slot < 64; ++slot) {
            const uint8_t *o = oam + slot * 4;
            int top = o[0] + 1, r = py - top;
            if (o[0] >= 0xEF || r < 0 || r >= rows) continue;
            bool drawn = found++ < 8;
            if (!drawn && !dropped) break;
            unsigned attr = o[2];
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                                    : (uint16_t)(table + o[1] * 16);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8; ++c) {
                int px = o[3] + c;
                if (px >= NES_W) break;
                if (px < 8 && !(mask & 0x04)) continue;
                int bit = (attr & 0x40) ? c : 7 - c;
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) continue;
                if (drawn) {
                    if (!((attr & 0x20) && opaque[px])) cov[px] = 1;
                } else if (!at[px]) {
                    at[px] = (attr & 0x20) ? 2 : 1;
                    ex[px] = cyc_render_color(16 + (attr & 3) * 4 + v);
                    ++extra;
                }
            }
        }
    }
    return extra;
}

/* DD2 writes its sprite list forwards (from slot 0) and backwards (ending at
 * slot 63) on alternate frames, so the NES takes turns when more than 8
 * share a line, and every overlap swaps which sprite is on top. Which way
 * this frame's list runs: slot 0 empty with slot 63 used is backwards; a list
 * that fills both ends alternates with the previous frame. */
static bool s_oam_backwards;
static void oam_direction(void)
{
    const uint8_t *oam = cyc_render_oam();
    bool first = oam[0] < 0xEF, final = oam[63 * 4] < 0xEF;
    if (first != final) s_oam_backwards = final;
    else s_oam_backwards = !s_oam_backwards;
}

/* Every sprite of rows [y0, y1), no 8-per-line limit, read in the order of
 * the backward lists on every frame (the forward order puts Billy's dark
 * outline sprites over his skin): s_layer gets the colour of the top opaque sprite
 * pixel and s_layer_at 1 (in front of the background) or 2 (behind it). */
static void sprite_layer_all(int y0, int y1)
{
    memset(s_layer_at + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    const uint8_t *oam = cyc_render_oam();
    for (int py = y0; py < y1; ++py) {
        uint8_t mask = cyc_render_line_mask(py);
        if (!(mask & 0x10)) continue;
        bool tall = cyc_render_line_sprite16(py);
        int rows = tall ? 16 : 8;
        uint16_t table = cyc_render_line_sprite_table(py);
        uint8_t *at = s_layer_at + py * NES_W;
        uint32_t *col = s_layer + py * NES_W;
        for (int k = 0; k < 64; ++k) {
            int slot = s_oam_backwards ? k : 63 - k;   /* the backward list's order */
            const uint8_t *o = oam + slot * 4;
            int top = o[0] + 1, r = py - top;
            if (o[0] >= 0xEF || r < 0 || r >= rows) continue;
            unsigned attr = o[2];
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                                    : (uint16_t)(table + o[1] * 16);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8; ++c) {
                int px = o[3] + c;
                if (px >= NES_W) break;
                if (at[px] || (px < 8 && !(mask & 0x04))) continue;
                int bit = (attr & 0x40) ? c : 7 - c;
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) continue;
                at[px] = (attr & 0x20) ? 2 : 1;
                col[px] = cyc_render_color(16 + (attr & 3) * 4 + v);
            }
        }
    }
}

/* ---- picture ----
 * One renderer for every setting. Once per machine frame (prepare) it takes
 * the background alone (the engine's background capture) and a sprite layer:
 * every sprite in one fixed order with NoSpriteLimit, else the sprites the
 * NES drew, read from the native picture. Each picture (present) then draws
 * the playfield rows from those at the camera position for its moment,
 * scaled by s_scale so the camera can move in fractions of a native pixel;
 * other rows (the HUD) are the native picture. */
static double s_phase;            /* fraction of a frame since the machine frame */
static long   s_prepared = -1;    /* s_frame the layers were made for */
static bool   s_have_layers;

static void prepare(const uint32_t *native)
{
    s_prepared = s_frame;
    s_have_layers = s_top >= 0 && s_hud - s_top >= 8 && cyc_render_background(s_bg);
    if (!s_have_layers) return;
    int last = s_hud - 2;
    if (s_unlimit) { sprite_layer_all(s_top, last + 1); return; }
    sprite_layers(s_top, last + 1, false);
    for (int y = s_top; y <= last; ++y)
        for (int x = 0; x < NES_W; ++x) {
            size_t i = (size_t)y * NES_W + (size_t)x;
            s_layer_at[i] = s_sprite[i];
            s_layer[i] = native[i];
        }
}

static int floordiv(int a, int k) { return a >= 0 ? a / k : -((-a + k - 1) / k); }

enum { SCALE_MAX = 6 };
static uint32_t s_big[NES_W * SCALE_MAX * NES_H * SCALE_MAX];
static int s_col_native[NES_W * SCALE_MAX], s_col_bg[NES_W * SCALE_MAX], s_col_moved[NES_W * SCALE_MAX];

static const uint32_t *present(void *ctx, int *width, int *height)
{
    (void)ctx;
    int w, h;
    const uint32_t *native = cyc_render_present(&w, &h);
    if (!native || w != NES_W || h != NES_H) return NULL;              /* a wider/HD picture: leave it */
    if (s_prepared != s_frame) prepare(native);

    int k = s_present_rate ? s_scale : 1;
    bool smooth = s_smooth != SMOOTH_OFF;
    Crop c = s_crop;
    if (smooth) { c.left += s_margin; c.right += s_margin; c.top += s_margin; }
    double fx = 0, fy = 0;
    if (smooth) {
        fx = s_smx.off - s_smx.vel * s_phase;
        fy = s_smy.off - s_smy.vel * s_phase;
        fx = fx < -s_margin ? -s_margin : fx > s_margin ? s_margin : fx;
        fy = fy < -s_margin ? -s_margin : fy > s_margin ? s_margin : fy;
    }
    int kx = (int)lround(fx * k), ky = (int)lround(fy * k);
    bool fix = s_split && s_hud_steady >= s_hud;
    bool layers = s_have_layers && (kx || ky || fix || s_unlimit);
    if (!layers && k == 1 && !c.top && !c.bottom && !c.left && !c.right) return NULL;   /* host default */

    int ow = (NES_W - c.left - c.right) * k, oh = (NES_H - c.top - c.bottom) * k;
    if (ow <= 0 || oh <= 0) return NULL;
    bool move = s_smooth == SMOOTH_PLAYFIELD;
    int last = s_hud - 2, y1 = fix ? s_hud_steady : s_hud;
    const uint8_t *opaque = cyc_frame_bg_opaque();
    for (int x = 0; x < ow; ++x) {
        int gx = c.left * k + x;
        s_col_native[x] = gx / k;
        s_col_moved[x] = floordiv(gx - kx, k);
        s_col_bg[x] = clampi(s_col_moved[x], 0, NES_W - 1);
    }
    for (int y = 0; y < oh; ++y) {
        int gy = c.top * k + y, yn = gy / k;
        uint32_t *out = s_big + (size_t)y * (size_t)ow;
        if (!layers || yn < s_top || yn >= y1) {
            const uint32_t *row = native + yn * NES_W;
            for (int x = 0; x < ow; ++x) out[x] = row[s_col_native[x]];
            continue;
        }
        int moved = floordiv(gy - ky, k);
        int by = clampi(moved, s_top, last);
        const uint32_t *bg = s_bg + by * NES_W;
        const uint8_t *op = opaque + by * NES_W;
        int sy = move ? moved : yn;
        bool sprites = yn <= last && sy >= s_top && sy <= last;
        const uint8_t *at = s_layer_at + (sprites ? sy : 0) * NES_W;
        const uint32_t *sc = s_layer + (sprites ? sy : 0) * NES_W;
        for (int x = 0; x < ow; ++x) {
            int bx = s_col_bg[x];
            uint32_t px = bg[bx];
            if (sprites) {
                int sx = move ? s_col_moved[x] : s_col_native[x];
                if (sx >= 0 && sx < NES_W && (at[sx] == 1 || (at[sx] == 2 && !op[bx]))) px = sc[sx];
            }
            out[x] = px;
        }
    }
    *width = ow;
    *height = oh;
    return s_big;
}

static void present_phase(void *ctx, double phase)
{
    (void)ctx;
    s_phase = phase;
}

/* ---- settings ---- */
static bool parse_crop(const char *s, Crop *out)
{
    int t, b, l, r;
    char tail;
    if (!s || sscanf(s, "%d,%d,%d,%d%c", &t, &b, &l, &r, &tail) != 4) return false;
    if (t < 0 || b < 0 || l < 0 || r < 0 || t > CROP_MAX || b > CROP_MAX || l > CROP_MAX || r > CROP_MAX)
        return false;
    out->top = t; out->bottom = b; out->left = l; out->right = r;
    return true;
}

static void load_setting(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    int v = atoi(value);
    if      (!strcmp(key, "OverscanTop"))    s_crop_saved.top = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanBottom")) s_crop_saved.bottom = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanLeft"))   s_crop_saved.left = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanRight"))  s_crop_saved.right = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "SmoothCamera"))   s_smooth = s_smooth_saved = clampi(v, 0, SMOOTH_MODES - 1);
    else if (!strcmp(key, "HudSplitFix"))    s_split = s_split_saved = v != 0;
    else if (!strcmp(key, "PresentRate"))    s_present_rate = s_rate_saved = clampi(v, 0, 1000);
    else if (!strcmp(key, "PresentScale"))   s_scale = s_scale_saved = clampi(v, 1, 6);
    else if (!strcmp(key, "NoSpriteLimit"))  s_unlimit = s_unlimit_saved = v != 0;
    else if (!strcmp(key, "SmoothEase"))     s_ease = s_ease_saved = v != 0;
    else if (!strcmp(key, "SmoothMargin"))   s_margin = s_margin_saved = clampi(v, 1, DD2_SMOOTH_MARGIN_MAX);
    s_crop = s_crop_saved;
}

static void save_settings(void *ctx, FILE *f)
{
    (void)ctx;
    fprintf(f, "OverscanTop = %d\n", s_crop_saved.top);
    fprintf(f, "OverscanBottom = %d\n", s_crop_saved.bottom);
    fprintf(f, "OverscanLeft = %d\n", s_crop_saved.left);
    fprintf(f, "OverscanRight = %d\n", s_crop_saved.right);
    fprintf(f, "SmoothCamera = %d\n", s_smooth_saved);
    fprintf(f, "HudSplitFix = %d\n", s_split_saved);
    fprintf(f, "SmoothMargin = %d\n", s_margin_saved);
    fprintf(f, "SmoothEase = %d\n", s_ease_saved);
    fprintf(f, "NoSpriteLimit = %d\n", s_unlimit_saved);
    fprintf(f, "PresentRate = %d\n", s_rate_saved);
    fprintf(f, "PresentScale = %d\n", s_scale_saved);
}

/* ---- per frame ---- */
static FILE *s_log;
static FILE *s_oam_log;   /* --oam-log: the frame's 64 OAM entries */

static void restart(void)
{
    s_prev_x = s_prev_y = -1;
    dd2_smooth_set_margin(&s_smx, s_margin, s_ease);
    dd2_smooth_set_margin(&s_smy, s_margin, s_ease);
    dd2_smooth_reset(&s_smx);
    dd2_smooth_reset(&s_smy);
    s_ox = s_oy = 0;
    s_hud_n = s_hud_head = 0;
    s_hud_steady = -1;
}

static void power_on(void *ctx)
{
    (void)ctx;
    s_frame = 0;
    restart();
    if (s_log) { fprintf(s_log, "# power on\n"); fflush(s_log); }
}

static void state_loaded(void *ctx)
{
    (void)ctx;
    restart();
    if (s_log) { fprintf(s_log, "# state loaded at frame %ld\n", s_frame); fflush(s_log); }
}

static int wrap_step(int d, int span) { d %= span; if (d < 0) d += span; return d >= span / 2 ? d - span : d; }

static void frame_end(void *ctx)
{
    (void)ctx;
    long f = s_frame++;

    static bool capture_on;
    s_phase = 0;
    s_extras.present_rate = (unsigned)s_present_rate;
    oam_direction();
    if (!capture_on) {
        cyc_render_capture_background(true);   /* whole from the next frame */
        capture_on = true;
    }

    find_playfield();
    steady_hud();
    bool on = s_top >= 0;
    int x = cyc_render_line_scroll_x(s_line);
    int y = cyc_render_line_scroll_y(s_line) - (s_line - s_top);   /* the playfield's top row */
    if (on) y = ((y % SCROLL_H) + SCROLL_H) % SCROLL_H;

    /* DD2 never sets the nametable X bit: X wraps 255 -> 0 in one nametable. */
    int dx = on && s_prev_x >= 0 ? wrap_step(x - s_prev_x, 256) : 0;
    int dy = on && s_prev_y >= 0 ? wrap_step(y - s_prev_y, SCROLL_H) : 0;
    s_prev_x = on ? x : -1;
    s_prev_y = on ? y : -1;
    s_ox = dd2_smooth_frame(&s_smx, on, dx);
    s_oy = dd2_smooth_frame(&s_smy, on, dy);

    if (s_oam_log) {
        const uint8_t *oam = cyc_render_oam();
        fprintf(s_oam_log, "%ld", f);
        for (int i = 0; i < 64; ++i)
            fprintf(s_oam_log, " %02x%02x%02x%02x", oam[i * 4], oam[i * 4 + 1], oam[i * 4 + 2], oam[i * 4 + 3]);
        fputc('\n', s_oam_log);
    }
    if (!s_log) return;
    if (!on) { fprintf(s_log, "%6ld  off\n", f); fflush(s_log); return; }

    /* OAM extent, as billy_x.lua: Y < $EF counts as on screen. */
    const uint8_t *oam = cyc_render_oam();
    int n = 0, minx = 255, maxx = 0;
    for (int i = 0; i < 64; ++i) {
        if (oam[i * 4] >= 0xEF) continue;
        int sx = oam[i * 4 + 3];
        ++n;
        if (sx < minx) minx = sx;
        if (sx > maxx) maxx = sx;
    }
    if (!n) { minx = -1; maxx = -1; }
    /* Pixels of sprites the 8-per-line limit dropped this frame (what
     * NoSpriteLimit draws back). */
    int dropped = s_hud - s_top >= 8 ? sprite_layers(s_top, s_hud - 1, true) : 0;
    fprintf(s_log, "%6ld  %4d  %+3d  %4d  %3d  %4d  %4d  %+2d  %3d  %+3d  %+2d  %3d  %4d  %d\n",
            f, x, dx, cyc_render_line_scroll_x(HUD_SAMPLE_LINE), n, minx, maxx, s_ox, y, dy, s_oy, s_hud,
            dropped, s_unlimit);
    fflush(s_log);
}

/* ---- options ---- */
static const CycHostOption s_options[] = {
    { "--scroll-log",    true, "FILE: log the playfield scroll and sprite extent every frame" },
    { "--oam-log",       true, "FILE: the frame's 64 sprite entries (Y tile attr X, hex) every frame" },
    { "--scroll-line",   true, "N: playfield line the scroll log samples (default 100)" },
    { "--overscan",      true, "T,B,L,R: crop this many pixels for this run (overrides config.ini)" },
    { "--smooth",        true, "0|1|2: camera smoothing off / background / playfield for this run" },
    { "--hud-split-fix", true, "0|1: steady the HUD split line for this run" },
    { "--smooth-margin", true, "N: smoothing margin in px, 1-12 (default 3; crops this much more)" },
    { "--smooth-ease",   true, "0|1: ease the camera's speed in and out (default 0)" },
    { "--present-rate",  true, "N: pictures per second, e.g. 120 or 240 (0: one per machine frame)" },
    { "--present-scale", true, "K: picture scale 1-6 with --present-rate (default 4)" },
    { "--no-sprite-limit", true, "0|1: also draw the sprites the 8-per-line limit drops (no flicker)" },
};

static bool option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--scroll-log")) {
        if (s_log) fclose(s_log);
        s_log = fopen(value, "w");
        if (!s_log) { perror(value); return false; }
        fprintf(s_log, "# frame  scrollX  dx  hudX  sprites  left  right  shiftX  scrollY  dy  shiftY  hudLine  droppedPx  noLimit\n");
        fflush(s_log);
        return true;
    }
    if (!strcmp(name, "--oam-log")) {
        if (s_oam_log) fclose(s_oam_log);
        s_oam_log = fopen(value, "w");
        if (!s_oam_log) { perror(value); return false; }
        return true;
    }
    if (!strcmp(name, "--scroll-line")) {
        char *end;
        long v = strtol(value, &end, 10);
        if (*end || v < 1 || v >= NES_H - 1) return false;
        s_line = (int)v;
        return true;
    }
    if (!strcmp(name, "--overscan")) return parse_crop(value, &s_crop);
    if (!strcmp(name, "--present-rate") || !strcmp(name, "--present-scale")) {
        char *end;
        long v = strtol(value, &end, 10);
        bool rate = name[10] == 'r';
        if (*end || v < (rate ? 0 : 1) || v > (rate ? 1000 : 6)) return false;
        if (rate) s_present_rate = (int)v;
        else s_scale = (int)v;
        s_extras.present_rate = (unsigned)s_present_rate;
        return true;
    }
    if (!strcmp(name, "--smooth-margin")) {
        char *end;
        long v = strtol(value, &end, 10);
        if (*end || v < 1 || v > DD2_SMOOTH_MARGIN_MAX) return false;
        s_margin = (int)v;
        return true;
    }
    if (!strcmp(name, "--smooth-ease") || !strcmp(name, "--no-sprite-limit")) {
        if (strcmp(value, "0") && strcmp(value, "1")) return false;
        if (name[2] == 's') s_ease = value[0] == '1';
        else s_unlimit = value[0] == '1';
        return true;
    }
    if (!strcmp(name, "--smooth") || !strcmp(name, "--hud-split-fix")) {
        bool smooth = name[2] == 's';
        int max = smooth ? SMOOTH_MODES - 1 : 1;
        if (strlen(value) != 1 || value[0] < '0' || value[0] > '0' + max) return false;
        if (smooth) s_smooth = value[0] - '0';
        else s_split = value[0] - '0';
        return true;
    }
    return false;
}

static CycHostExtras s_extras = {
    .present       = present,
    .load_setting  = load_setting,
    .save_settings = save_settings,
    .power_on      = power_on,
    .frame_end     = frame_end,
    .options       = s_options,
    .option_count  = sizeof(s_options) / sizeof(s_options[0]),
    .option        = option,
    .state_loaded  = state_loaded,
    .present_phase = present_phase,
};

const CycHostExtras *cyc_host_extras(void) { return &s_extras; }
