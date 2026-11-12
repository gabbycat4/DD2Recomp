/*
 * dd2_smooth.h - presentation camera smoother for Double Dragon II.
 *
 * The game moves its camera in whole steps on an uneven rhythm (walking:
 * 2 px after 2,2,3,1 frames; running: 3 px after 3,1,2 frames). Fed the
 * camera's per-frame step, the smoother tracks its average speed and returns
 * how many pixels the playfield should be shifted so the camera appears to
 * move evenly. Pure arithmetic, no engine dependency: tools/smooth_sim.c runs
 * it over a --scroll-log file.
 */
#pragma once
#include <stdbool.h>

enum {
    DD2_SMOOTH_WINDOW = 24,     /* frames: whole cycles of both rhythms (8 and 6) */
    DD2_SMOOTH_MARGIN = 3,      /* default px either way; the crop grows by this per side */
    DD2_SMOOTH_MARGIN_MAX = 12,
};

typedef struct {
    /* settings (dd2_smooth_set_margin; kept by reset) */
    int    margin;                    /* largest shift, px */
    bool   ease;
    double accel;                     /* largest speed change, px/frame per frame */
    /* state */
    bool   have;
    int    n;                         /* frames in the windows, up to WINDOW */
    int    head;
    int    idle;                        /* frames since the camera last moved */
    double cam, s;                    /* actual (unwrapped) and smoothed camera */
    double vd;                        /* the smoothed camera's speed */
    double dx[DD2_SMOOTH_WINDOW], dx_sum;
    double lag[DD2_SMOOTH_WINDOW], lag_sum;
    int    shift;                     /* last result */
    double off;                       /* the same before rounding and the margin */
    double vel;                       /* the smoothed camera's speed, px/frame */
    bool   clamped;                   /* last result hit the margin */
} Dd2Smooth;

/* The margin (px, 1..DD2_SMOOTH_MARGIN_MAX) bounds the shift. With ease,
 * the smoothed camera's speed changes no faster than the margin can absorb
 * going from rest to running speed; without it (the default) the speed
 * follows the average at once. */
void dd2_smooth_set_margin(Dd2Smooth *sm, int margin, bool ease);
/* Forget the camera (keeps the settings). */
void dd2_smooth_reset(Dd2Smooth *sm);
/* One machine frame. visible: the playfield was drawn; dx: camera step since
 * the previous frame (px, signed). Returns the shift: draw native column x - shift
 * at screen column x. A picture a fraction p of a frame later (between
 * machine frames) uses off - vel * p. */
int  dd2_smooth_frame(Dd2Smooth *sm, bool visible, int dx);
