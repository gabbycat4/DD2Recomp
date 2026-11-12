/*
 * dd2_extras.c - Double Dragon II's additions to the NESRecomp cycle host
 * (cyc_host_extras.h). Presentation and observation only: nothing here
 * changes what the emulated machine does.
 *
 *   Scroll log    --scroll-log FILE writes one line per machine frame with the
 *                 background X the PPU actually drew the playfield with
 *                 (cyc_render_line_scroll_x), its change from the previous
 *                 frame, the HUD's X below the split, and the OAM extent of
 *                 visible sprites (the same numbers billy_x.lua logs in Mesen).
 *                 --scroll-line N picks the playfield line sampled (default 100;
 *                 must be above the HUD split at 185).
 *                 tools/scroll_pattern.py summarises the log.
 *
 *   Overscan crop config.ini [Game] OverscanTop/Bottom/Left/Right (pixels,
 *                 default 0 = stock picture), or --overscan T,B,L,R for one
 *                 run. The window and screenshots get the cropped picture; the
 *                 machine's 256x240 frame and headless hashes are unchanged.
 */
#include "cyc_host_extras.h"
#include "cyc_core.h"
#include "cyc_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NES_W = 256,
    NES_H = 240,
    HUD_SPLIT_LINE = 185,   /* notes.txt: HUD split at scanline 185 */
    HUD_SAMPLE_LINE = 200,
    CROP_MAX = 64,
};

/* ---- overscan ---- */
typedef struct { int top, bottom, left, right; } Crop;
static Crop s_crop_saved;     /* config.ini */
static Crop s_crop;           /* in effect (config, or --overscan) */
static uint32_t s_crop_buf[NES_W * NES_H];

static int clamp_crop(int v) { return v < 0 ? 0 : v > CROP_MAX ? CROP_MAX : v; }

static const uint32_t *present(void *ctx, int *width, int *height)
{
    (void)ctx;
    Crop c = s_crop;
    if (!c.top && !c.bottom && !c.left && !c.right) return NULL;   /* host default */
    int w, h;
    const uint32_t *src = cyc_render_present(&w, &h);
    if (!src || w != NES_W || h != NES_H) return NULL;              /* a wider/HD picture: leave it */
    int ow = w - c.left - c.right, oh = h - c.top - c.bottom;
    if (ow <= 0 || oh <= 0) return NULL;
    for (int y = 0; y < oh; ++y)
        memcpy(&s_crop_buf[y * ow], &src[(y + c.top) * w + c.left], (size_t)ow * sizeof(uint32_t));
    *width = ow;
    *height = oh;
    return s_crop_buf;
}

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
    int v = clamp_crop(atoi(value));
    if      (!strcmp(key, "OverscanTop"))    s_crop_saved.top = v;
    else if (!strcmp(key, "OverscanBottom")) s_crop_saved.bottom = v;
    else if (!strcmp(key, "OverscanLeft"))   s_crop_saved.left = v;
    else if (!strcmp(key, "OverscanRight"))  s_crop_saved.right = v;
    else return;
    s_crop = s_crop_saved;
}

static void save_settings(void *ctx, FILE *f)
{
    (void)ctx;
    fprintf(f, "OverscanTop = %d\n", s_crop_saved.top);
    fprintf(f, "OverscanBottom = %d\n", s_crop_saved.bottom);
    fprintf(f, "OverscanLeft = %d\n", s_crop_saved.left);
    fprintf(f, "OverscanRight = %d\n", s_crop_saved.right);
}

/* ---- scroll log ---- */
static FILE *s_log;
static int   s_line = 100;
static long  s_frame;
static int   s_prev_x = -1;

static void log_header(void)
{
    fprintf(s_log, "# frame  scrollX  dx  hudX  sprites  left  right   (scrollX at line %d, 0-511)\n", s_line);
    fflush(s_log);
}

static void power_on(void *ctx)
{
    (void)ctx;
    s_frame = 0;
    s_prev_x = -1;
    if (s_log) { fprintf(s_log, "# power on\n"); fflush(s_log); }
}

static void state_loaded(void *ctx)
{
    (void)ctx;
    s_prev_x = -1;
    if (s_log) { fprintf(s_log, "# state loaded at frame %ld\n", s_frame); fflush(s_log); }
}

static void frame_end(void *ctx)
{
    (void)ctx;
    long f = s_frame++;
    if (!s_log) return;

    bool bg_on = (cyc_render_line_mask(s_line) & 0x08) != 0;
    int x = cyc_render_line_scroll_x(s_line);
    int hud = cyc_render_line_scroll_x(HUD_SAMPLE_LINE);

    int dx = 0;
    if (bg_on && s_prev_x >= 0) {
        /* DD2 never sets the nametable X bit: its playfield X wraps 255 -> 0
         * inside one nametable, so a step is measured modulo 256. */
        dx = (x - s_prev_x) & 255;
        if (dx >= 128) dx -= 256;          /* leftward */
    }
    s_prev_x = bg_on ? x : -1;

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

    if (bg_on)
        fprintf(s_log, "%6ld  %4d  %+3d  %4d  %3d  %4d  %4d\n", f, x, dx, hud, n, minx, maxx);
    else
        fprintf(s_log, "%6ld  off\n", f);
    fflush(s_log);
}

/* ---- options ---- */
static const CycHostOption s_options[] = {
    { "--scroll-log",  true, "FILE: log the playfield scroll and sprite extent every frame" },
    { "--scroll-line", true, "N: playfield line the scroll log samples (default 100, < 185)" },
    { "--overscan",    true, "T,B,L,R: crop this many pixels for this run (overrides config.ini)" },
};

static bool option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--scroll-log")) {
        if (s_log) fclose(s_log);
        s_log = fopen(value, "w");
        if (!s_log) { perror(value); return false; }
        log_header();
        return true;
    }
    if (!strcmp(name, "--scroll-line")) {
        char *end;
        long v = strtol(value, &end, 10);
        if (*end || v < 0 || v >= HUD_SPLIT_LINE) return false;
        s_line = (int)v;
        if (s_log) fprintf(s_log, "# sampling line %d\n", s_line);
        return true;
    }
    if (!strcmp(name, "--overscan")) return parse_crop(value, &s_crop);
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
