#ifndef VC_PROGRESS_H
#define VC_PROGRESS_H

// ---------------------------------------------------------------------------
// vcProgress.h  –  simple terminal progress bar for push/pull/clone.
//
// Usage:
//   VcProgress p;
//   vcp_init(&p, total_files);
//   vcp_update(&p, filename);   // call for each file processed
//   vcp_done(&p);               // print newline when finished
// ---------------------------------------------------------------------------

#include <stddef.h>

typedef struct {
    int   total;      // total number of files expected
    int   done;       // files processed so far
    int   failed;     // files that failed
    int   bar_width;  // width of the progress bar in chars (default 40)
} VcProgress;

// Initialise progress bar. total = expected number of files (0 = unknown).
void vcp_init(VcProgress *p, int total);

// Update bar after processing one file.
// label: short label shown after bar ("push", "pull", "clone", "FAIL")
// filename: the file just processed (truncated if long)
void vcp_update(VcProgress *p, const char *label, const char *filename);

// Mark a file as failed (counts in red if terminal supports it).
void vcp_fail(VcProgress *p, const char *filename);

// Finish — move to next line.
void vcp_done(VcProgress *p);

#endif // VC_PROGRESS_H
