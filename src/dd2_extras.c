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
static int  s_margin_saved = DEFAULT_MARGIN, s_margin = DEFAULT_MARGIN;

/* ---- per-frame state (frame_end) ---- */
static int       s_line = 100;                /* playfield line sampled */
static Dd2Smooth s_smx, s_smy;
static int       s_ox, s_oy;                  /* shifts for the last frame's picture */
static int       s_prev_x = -1, s_prev_y = -1;
static int       s_top = -1, s_hud = -1;      /* playfield rows [s_top, s_hud) */
static int       s_hud_steady = -1;           /* latest HUD line over SPLIT_HISTORY */
static int       s_hud_hist[SPLIT_HISTORY], s_hud_n, s_hud_head;

static uint32_t s_full[NES_W * NES_H], s_bg[NES_W * NES_H], s_out[NES_W * NES_H];
static uint8_t  s_sprite[NES_W * NES_H];     /* 1 where a sprite pixel shows in the native picture */

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

/* ---- picture ---- */
/* Which pixels of rows [y0, y1) of the native picture are sprite pixels,
 * from the frame's OAM as the PPU drew it: at most 8 sprites per line (the
 * first 8 in OAM order: the game's flicker), opaque pattern pixels only, the
 * left-column clip, and behind-background sprites only where the background
 * was transparent. Colour comparison cannot tell a sprite pixel from a
 * background pixel of the same colour; this can. */
static void sprite_coverage(int y0, int y1)
{
    memset(s_sprite + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    const uint8_t *oam = cyc_render_oam();
    const uint8_t *bg_opaque = cyc_frame_bg_opaque();
    for (int py = y0; py < y1; ++py) {
        uint8_t mask = cyc_render_line_mask(py);
        if (!(mask & 0x10)) continue;
        bool tall = cyc_render_line_sprite16(py);
        int rows = tall ? 16 : 8, found = 0;
        uint16_t table = cyc_render_line_sprite_table(py);
        uint8_t *cov = s_sprite + py * NES_W;
        const uint8_t *opaque = bg_opaque + py * NES_W;
        for (int slot = 0; slot < 64 && found < 8; ++slot) {
            const uint8_t *o = oam + slot * 4;
            int top = o[0] + 1, r = py - top;
            if (o[0] >= 0xEF || r < 0 || r >= rows) continue;
            ++found;
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
                if (!(((lo >> bit) & 1) | ((hi >> bit) & 1))) continue;
                if ((attr & 0x20) && opaque[px]) continue;
                cov[px] = 1;
            }
        }
    }
}

static const uint32_t *compose(const uint32_t *native)
{
    if (s_top < 0 || s_hud - s_top < 8) return NULL;
    int ox = s_smooth ? s_ox : 0, oy = s_smooth ? s_oy : 0;
    bool fix = s_split && s_hud_steady >= s_hud;
    if (!ox && !oy && !fix) return NULL;
    bool bg = s_smooth == SMOOTH_BACKGROUND && (ox || oy) && cyc_render_background(s_bg);

    memcpy(s_full, native, sizeof(s_full));
    int last = s_hud - 2;                        /* last whole playfield line */
    int y0 = (ox || oy) ? s_top : s_hud - 1;
    int y1 = fix ? s_hud_steady : s_hud;         /* exclusive */
    if (bg) sprite_coverage(y0, y1 < s_hud ? y1 : s_hud);
    for (int y = y0; y < y1; ++y) {
        int sy = clampi(y - oy, s_top, last);
        const uint32_t *src = (bg ? s_bg : native) + sy * NES_W;
        const uint32_t *n = native + y * NES_W;
        const uint8_t *cov = s_sprite + y * NES_W;
        uint32_t *out = s_full + y * NES_W;
        bool sprites_here = bg && y <= last;     /* keep sprites where the game drew them */
        for (int x = 0; x < NES_W; ++x) {
            if (sprites_here && cov[x]) { out[x] = n[x]; continue; }
            out[x] = src[clampi(x - ox, 0, NES_W - 1)];
        }
    }
    return s_full;
}

static const uint32_t *present(void *ctx, int *width, int *height)
{
    (void)ctx;
    int w, h;
    const uint32_t *src = cyc_render_present(&w, &h);
    if (!src || w != NES_W || h != NES_H) return NULL;              /* a wider/HD picture: leave it */
    Crop c = s_crop;
    if (s_smooth != SMOOTH_OFF) {
        c.left += s_margin;
        c.right += s_margin;
        c.top += s_margin;
    }
    const uint32_t *composed = compose(src);
    if (!composed && !c.top && !c.bottom && !c.left && !c.right) return NULL;   /* host default */
    if (composed) src = composed;
    int ow = w - c.left - c.right, oh = h - c.top - c.bottom;
    if (ow <= 0 || oh <= 0) return NULL;
    for (int y = 0; y < oh; ++y)
        memcpy(&s_out[y * ow], &src[(y + c.top) * w + c.left], (size_t)ow * sizeof(uint32_t));
    *width = ow;
    *height = oh;
    return s_out;
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
}

/* ---- per frame ---- */
static FILE *s_log;
static long  s_frame;

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
    if (s_smooth == SMOOTH_BACKGROUND && !capture_on) {
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
    fprintf(s_log, "%6ld  %4d  %+3d  %4d  %3d  %4d  %4d  %+2d  %3d  %+3d  %+2d  %3d\n",
            f, x, dx, cyc_render_line_scroll_x(HUD_SAMPLE_LINE), n, minx, maxx, s_ox, y, dy, s_oy, s_hud);
    fflush(s_log);
}

/* ---- options ---- */
static const CycHostOption s_options[] = {
    { "--scroll-log",    true, "FILE: log the playfield scroll and sprite extent every frame" },
    { "--scroll-line",   true, "N: playfield line the scroll log samples (default 100)" },
    { "--overscan",      true, "T,B,L,R: crop this many pixels for this run (overrides config.ini)" },
    { "--smooth",        true, "0|1|2: camera smoothing off / background / playfield for this run" },
    { "--hud-split-fix", true, "0|1: steady the HUD split line for this run" },
    { "--smooth-margin", true, "N: smoothing margin in px, 1-12 (default 3; crops this much more)" },
    { "--smooth-ease",   true, "0|1: ease the camera's speed in and out (default 0)" },
};

static bool option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--scroll-log")) {
        if (s_log) fclose(s_log);
        s_log = fopen(value, "w");
        if (!s_log) { perror(value); return false; }
        fprintf(s_log, "# frame  scrollX  dx  hudX  sprites  left  right  shiftX  scrollY  dy  shiftY  hudLine\n");
        fflush(s_log);
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
    if (!strcmp(name, "--smooth-margin")) {
        char *end;
        long v = strtol(value, &end, 10);
        if (*end || v < 1 || v > DD2_SMOOTH_MARGIN_MAX) return false;
        s_margin = (int)v;
        return true;
    }
    if (!strcmp(name, "--smooth-ease")) {
        if (strcmp(value, "0") && strcmp(value, "1")) return false;
        s_ease = value[0] == '1';
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

static const CycHostExtras s_extras = {
    .present       = present,
    .load_setting  = load_setting,
    .save_settings = save_settings,
    .power_on      = power_on,
    .frame_end     = frame_end,
    .options       = s_options,
    .option_count  = sizeof(s_options) / sizeof(s_options[0]),
    .option        = option,
    .state_loaded  = state_loaded,
};

const CycHostExtras *cyc_host_extras(void) { return &s_extras; }
