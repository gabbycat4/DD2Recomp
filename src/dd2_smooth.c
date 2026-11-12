/* dd2_smooth.c - see dd2_smooth.h. */
#include "dd2_smooth.h"

#include <math.h>
#include <string.h>

enum { SNAP_STEP = 16, RESTART_IDLE = 8, PRIME = 8 };
#define RUN_SPEED 1.5                 /* px/frame: the fastest steady camera */

void dd2_smooth_set_margin(Dd2Smooth *sm, int margin, bool ease)
{
    sm->ease = ease;
    if (margin < 1) margin = 1;
    if (margin > DD2_SMOOTH_MARGIN_MAX) margin = DD2_SMOOTH_MARGIN_MAX;
    sm->margin = margin;
    /* Rest to running speed costs RUN_SPEED^2 / (2 accel) px of lag. */
    sm->accel = ease ? RUN_SPEED * RUN_SPEED / (2.0 * margin) : 1e9;
}

void dd2_smooth_reset(Dd2Smooth *sm)
{
    int margin = sm->margin;
    bool ease = sm->ease;
    memset(sm, 0, sizeof(*sm));
    dd2_smooth_set_margin(sm, margin ? margin : DD2_SMOOTH_MARGIN, ease);
}

int dd2_smooth_frame(Dd2Smooth *sm, bool visible, int dx)
{
    if (!visible || !sm->have || dx > SNAP_STEP || dx < -SNAP_STEP) {
        /* Screen off, first frame, or a cut: start again at the real camera. */
        dd2_smooth_reset(sm);
        sm->have = visible;
        return 0;
    }
    sm->cam += dx;

    /* Moving again after a stop (the longest gap in either rhythm is 3
     * frames): forget the standing-still history so the speed starts from
     * the new movement instead of ramping up through old zeros. */
    if (dx && sm->idle >= RESTART_IDLE) {
        /* Prime the history as if the camera had already been moving at
         * half its first step per frame (2 px walking -> 1 px/frame, 3 px
         * running -> 1.5): it glides from the first frame instead of copying
         * the game's raw steps while the windows fill. The lag history
         * starts at zero, so the first frames show the smoothed camera
         * itself; real frames replace the guess within PRIME frames. */
        double v0 = dx / 2.0, lag0 = 0;
        sm->n = PRIME; sm->head = PRIME % DD2_SMOOTH_WINDOW;
        sm->dx_sum = sm->lag_sum = 0;
        for (int k = 0; k < PRIME; ++k) {
            sm->dx[k] = v0;  sm->dx_sum += v0;
            sm->lag[k] = lag0; sm->lag_sum += lag0;
        }
        sm->s = sm->cam - dx;
        sm->vd = sm->ease ? 0 : dx / 2.0;   /* ease in from rest, or start at speed */
    }
    sm->idle = dx ? 0 : sm->idle + 1;

    /* Average speed over the window (or what there is of it). */
    int i = sm->head;
    if (sm->n == DD2_SMOOTH_WINDOW) { sm->dx_sum -= sm->dx[i]; sm->lag_sum -= sm->lag[i]; }
    else sm->n++;
    sm->dx[i] = dx;
    sm->dx_sum += dx;
    double v = sm->dx_sum / sm->n;

    /* Ease the smoothed camera's speed toward that average, then advance:
     * never past the real camera in the direction of travel (a stop never
     * overshoots), never further behind than the lag limit. */
    double lag_limit = 2.0 * sm->margin > 6 ? 2.0 * sm->margin : 6;
    double dv = v - sm->vd;
    if (dv > sm->accel) dv = sm->accel;
    if (dv < -sm->accel) dv = -sm->accel;
    sm->vd += dv;
    sm->s += sm->vd;
    if (sm->vd > 0.01 && sm->s > sm->cam) { sm->s = sm->cam; }
    else if (sm->vd < -0.01 && sm->s < sm->cam) { sm->s = sm->cam; }
    else if (fabs(sm->vd) <= 0.01) {
        double d = sm->cam - sm->s;
        sm->s += d > 1 ? 1 : d < -1 ? -1 : d;
    }
    if (sm->cam - sm->s > lag_limit) sm->s = sm->cam - lag_limit;
    if (sm->s - sm->cam > lag_limit) sm->s = sm->cam + lag_limit;

    /* Centre: the shift is the lag minus its recent average, so the picture
     * swings both ways around the real camera instead of trailing it. */
    double lag = sm->cam - sm->s;
    sm->lag[i] = lag;
    sm->lag_sum += lag;
    sm->head = (i + 1) % DD2_SMOOTH_WINDOW;
    double off = lag - sm->lag_sum / sm->n;

    int shift = (int)lround(off);
    sm->clamped = shift > sm->margin || shift < -sm->margin;
    if (shift > sm->margin) shift = sm->margin;
    if (shift < -sm->margin) shift = -sm->margin;
    sm->shift = shift;
    sm->off = off;
    /* Where the next frame will most likely put the smoothed camera: on at
     * this speed; once the camera has been still for longer than any gap in
     * the game's rhythm (2 frames), never past the real camera, which has
     * stopped. In-between pictures move toward it, so a stop never makes
     * them run ahead and pull back, while a gap mid-rhythm keeps them going. */
    double next = sm->s + sm->vd;
    if (sm->idle < 3) { /* still travelling */ }
    else if (sm->vd > 0.01 && next > sm->cam) next = sm->cam;
    else if (sm->vd < -0.01 && next < sm->cam) next = sm->cam;
    else if (fabs(sm->vd) <= 0.01) { double d = sm->cam - sm->s; next = sm->s + (d > 1 ? 1 : d < -1 ? -1 : d); }
    sm->vel = next - sm->s;
    return shift;
}
