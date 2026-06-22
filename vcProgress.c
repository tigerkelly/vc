// vcProgress.c  –  progress display for push/pull/clone.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "vcProgress.h"

#define BAR_WIDTH  40
#define NAME_WIDTH 30

static void print_bar(int done, int total, const char *name) {
    int filled = 0, pct = 0;
    if (total > 0) {
        filled = (done * BAR_WIDTH) / total;
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;
        pct = (done * 100) / total;
    }
    char bar[BAR_WIDTH + 1];
    memset(bar, '-', BAR_WIDTH);
    bar[BAR_WIDTH] = '\0';
    for (int i = 0; i < filled; i++) bar[i] = '#';

    char nm[NAME_WIDTH + 1];
    size_t fl = strlen(name);
    if (fl > NAME_WIDTH)
        snprintf(nm, sizeof(nm), "...%s", name + fl - (NAME_WIDTH - 3));
    else
        snprintf(nm, sizeof(nm), "%s", name);

    printf("  [%s] %3d%% (%d/%d) %-*s\r",
           bar, pct, done, total, NAME_WIDTH, nm);
    fflush(stdout);
}

void vcp_init(VcProgress *p, int total) {
    p->total     = total;
    p->done      = 0;
    p->failed    = 0;
    p->bar_width = BAR_WIDTH;
    if (total > 0) {
        print_bar(0, total, "");
    }
}

void vcp_update(VcProgress *p, const char *label, const char *filename) {
    (void)label;
    p->done++;
    print_bar(p->done, p->total, filename);
}

void vcp_fail(VcProgress *p, const char *filename) {
    p->done++;
    p->failed++;
    // Clear the bar line and print the failure permanently.
    printf("  %-80s\r", "");
    printf("  FAIL: %s\n", filename);
    fflush(stdout);
    // Reprint bar with current progress.
    print_bar(p->done, p->total, filename);
}

void vcp_done(VcProgress *p) {
    // Print a completed bar permanently then newline.
    char bar[BAR_WIDTH + 1];
    memset(bar, '#', BAR_WIDTH);
    bar[BAR_WIDTH] = '\0';
    int total = p->total > 0 ? p->total : p->done;
    if (p->failed > 0)
        printf("  [%s] 100%% (%d/%d) %-*s  %d failed\n",
               bar, p->done - p->failed, total, NAME_WIDTH, "", p->failed);
    else
        printf("  [%s] 100%% (%d/%d) %-*s\n",
               bar, p->done, total, NAME_WIDTH, "");
    fflush(stdout);
}
