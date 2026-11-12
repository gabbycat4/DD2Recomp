/* smooth_sim.c - run the camera smoother over a --scroll-log file.
 *   cc -O2 -Isrc tools/smooth_sim.c src/dd2_smooth.c -lm -o /tmp/smooth_sim
 *   /tmp/smooth_sim scroll.log [-v] [-mMARGIN] [-e]
 * Prints how far the displayed camera moved each frame (native vs smoothed)
 * while the camera was travelling. */
#include "dd2_smooth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: smooth_sim LOG [-v]\n"); return 2; }
    bool verbose = false;
    int margin = DD2_SMOOTH_MARGIN; bool ease = false;
    for (int a = 2; a < argc; ++a) {
        if (!strcmp(argv[a], "-v")) verbose = true;
        else if (!strncmp(argv[a], "-m", 2)) margin = atoi(argv[a] + 2);
        else if (!strcmp(argv[a], "-e")) ease = true;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    Dd2Smooth sm = {0}; dd2_smooth_set_margin(&sm, margin, ease); dd2_smooth_reset(&sm);
    char line[256];
    long nat[16] = {0}, smo[16] = {0}, clamps = 0, moving = 0;
    int prev_disp = 0, prev_cam = 0, still = 99; bool have = false;
    long cam = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') { if (strstr(line, "power") || strstr(line, "state")) have = false; continue; }
        long fr; char xs[16]; int dx = 0;
        if (sscanf(line, "%ld %15s %d", &fr, xs, &dx) < 2) continue;
        bool vis = strcmp(xs, "off") != 0;
        if (dx <= -128) dx += 256;
        int sh = dd2_smooth_frame(&sm, vis, vis && have ? dx : 0);
        if (!vis) { have = false; continue; }
        if (!have) { have = true; cam = 0; prev_cam = 0; prev_disp = 0; continue; }
        cam += dx;
        int disp = (int)cam - sh;                    /* displayed camera */
        still = dx ? 0 : still + 1;
        if (still < 4) {                             /* travelling */
            moving++;
            int a = abs((int)cam - prev_cam), b = abs(disp - prev_disp);
            nat[a < 15 ? a : 15]++; smo[b < 15 ? b : 15]++;
            if (sm.clamped) clamps++;
            if (verbose) printf("%6ld dx=%+d shift=%+d disp_step=%+d%s\n", fr, dx, sh, disp - prev_disp, sm.clamped ? " CLAMP" : "");
        }
        prev_cam = (int)cam; prev_disp = disp;
    }
    printf("travelling frames: %ld, clamped: %ld\n", moving, clamps);
    printf("px per frame   native   smoothed\n");
    for (int i = 0; i < 16; i++) if (nat[i] || smo[i]) printf("  %2d%s        %6ld   %6ld\n", i, i == 15 ? "+" : " ", nat[i], smo[i]);
    return 0;
}
