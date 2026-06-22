// ---------------------------------------------------------------------------
// vcDiff3.c  –  Pure C 3-way line-level merge.
//
// Algorithm:
//   1. Read all three files into line arrays.
//   2. Compute LCS(base, ours)   → edit script A (what we changed).
//   3. Compute LCS(base, theirs) → edit script B (what they changed).
//   4. Walk the base line by line, applying both edit scripts.
//      - Only A changed a region  → take ours.
//      - Only B changed a region  → take theirs.
//      - Both changed identically → take either (same result).
//      - Both changed differently → emit conflict markers.
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "vcDiff3.h"

// ---------------------------------------------------------------------------
// Line array
// ---------------------------------------------------------------------------
#define MAX_LINES 65536
#define MAX_LINE  4096

typedef struct {
    char  **lines;
    int     count;
} LineArray;

static LineArray *la_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    LineArray *la = calloc(1, sizeof(LineArray));
    la->lines = malloc(MAX_LINES * sizeof(char *));
    la->count = 0;

    char buf[MAX_LINE];
    while (la->count < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        la->lines[la->count++] = strdup(buf);
    }
    fclose(f);
    return la;
}

static void la_free(LineArray *la) {
    if (!la) return;
    for (int i = 0; i < la->count; i++) free(la->lines[i]);
    free(la->lines);
    free(la);
}

// ---------------------------------------------------------------------------
// LCS (Myers diff) — edit opcodes
// ---------------------------------------------------------------------------
typedef enum { OP_EQ, OP_INS, OP_DEL } OpCode;

typedef struct {
    OpCode  op;
    int     baseIdx;   // index in 'a' (base)
    int     otherIdx;  // index in 'b' (ours or theirs)
} Edit;

typedef struct {
    Edit   *edits;
    int     count;
    int     cap;
} EditScript;

static void es_push(EditScript *es, OpCode op, int ai, int bi) {
    if (es->count == es->cap) {
        es->cap = es->cap ? es->cap * 2 : 256;
        es->edits = realloc(es->edits, es->cap * sizeof(Edit));
    }
    es->edits[es->count++] = (Edit){op, ai, bi};
}

// Classic O(ND) LCS via dynamic programming.
// For files up to ~8000 lines this is fine.
static EditScript *lcs_diff(const LineArray *a, const LineArray *b) {
    int n = a->count, m = b->count;

    // dp[i][j] = length of LCS of a[0..i-1] and b[0..j-1]
    // Use flat array for cache friendliness.
    int *dp = calloc((size_t)(n+1) * (m+1), sizeof(int));
    #define DP(i,j) dp[(i)*(m+1)+(j)]

    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            if (strcmp(a->lines[i-1], b->lines[j-1]) == 0)
                DP(i,j) = DP(i-1,j-1) + 1;
            else
                DP(i,j) = DP(i-1,j) > DP(i,j-1) ? DP(i-1,j) : DP(i,j-1);
        }

    // Backtrack to produce edit script.
    EditScript *es = calloc(1, sizeof(EditScript));
    int i = n, j = m;
    // Collect in reverse then reverse at end.
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 &&
            strcmp(a->lines[i-1], b->lines[j-1]) == 0) {
            es_push(es, OP_EQ, i-1, j-1);
            i--; j--;
        } else if (j > 0 && (i == 0 || DP(i,j-1) >= DP(i-1,j))) {
            es_push(es, OP_INS, i, j-1);   // inserted in b
            j--;
        } else {
            es_push(es, OP_DEL, i-1, j);   // deleted from a
            i--;
        }
    }
    #undef DP
    free(dp);

    // Reverse in place.
    for (int lo = 0, hi = es->count-1; lo < hi; lo++, hi--) {
        Edit tmp = es->edits[lo];
        es->edits[lo] = es->edits[hi];
        es->edits[hi] = tmp;
    }
    return es;
}

// ---------------------------------------------------------------------------
// Per-line change map: for each base line, what happened in ours/theirs?
// ---------------------------------------------------------------------------
typedef enum {
    CHG_SAME = 0,   // line unchanged
    CHG_MOD  = 1,   // line modified / lines inserted before here
    CHG_DEL  = 2,   // line deleted
} Change;

typedef struct {
    Change  change;
    int    *insertsBefore;  // indices into other[] of lines inserted before
    int     nInserts;
    int     insertedLine;   // if changed to a single different line
    bool    replaced;       // base line replaced (not just inserted before)
} LineState;

// Build a mapping from base-line index to what happened in 'other'.
// Returns array of (base->count + 1) LineState entries.
// The +1 slot handles insertions after the last base line.
static LineState *build_map(const LineArray *base,
                             const LineArray *other,
                             const EditScript *es) {
    int n = base->count;
    LineState *map = calloc((size_t)(n + 1), sizeof(LineState));

    for (int e = 0; e < es->count; e++) {
        Edit *ed = &es->edits[e];
        if (ed->op == OP_EQ) {
            map[ed->baseIdx].change = CHG_SAME;
        } else if (ed->op == OP_DEL) {
            map[ed->baseIdx].change = CHG_DEL;
        } else { // OP_INS — inserted before next base line
            int slot = ed->baseIdx; // insert before base[slot]
            map[slot].nInserts++;
            map[slot].insertsBefore = realloc(
                map[slot].insertsBefore,
                map[slot].nInserts * sizeof(int));
            map[slot].insertsBefore[map[slot].nInserts-1] = ed->otherIdx;
        }
    }
    return map;
}

static void map_free(LineState *map, int n) {
    for (int i = 0; i <= n; i++) free(map[i].insertsBefore);
    free(map);
}

// ---------------------------------------------------------------------------
// 3-way merge
// ---------------------------------------------------------------------------
int vc_merge3(const char *oursPath,
              const char *basePath,
              const char *theirsPath,
              const char *outPath,
              const char *oursLabel,
              const char *theirsLabel) {

    LineArray *base   = la_read(basePath);
    LineArray *ours   = la_read(oursPath);
    LineArray *theirs = la_read(theirsPath);

    if (!base || !ours || !theirs) {
        la_free(base); la_free(ours); la_free(theirs);
        return -1;
    }

    EditScript *esA = lcs_diff(base, ours);    // base → ours
    EditScript *esB = lcs_diff(base, theirs);  // base → theirs

    LineState *mapA = build_map(base, ours,   esA);
    LineState *mapB = build_map(base, theirs, esB);

    FILE *out = fopen(outPath, "w");
    if (!out) {
        la_free(base); la_free(ours); la_free(theirs);
        free(esA->edits); free(esA);
        free(esB->edits); free(esB);
        map_free(mapA, base->count);
        map_free(mapB, base->count);
        return -1;
    }

    int conflicts = 0;

    // Walk each base line (plus the virtual slot after the last one).
    for (int i = 0; i <= base->count; i++) {

        LineState *a = &mapA[i];
        LineState *b = &mapB[i];

        // ---- Handle insertions before this base line ----

        bool aHasIns = a->nInserts > 0;
        bool bHasIns = b->nInserts > 0;

        if (aHasIns && bHasIns) {
            // Both inserted lines here — check if identical.
            bool same = (a->nInserts == b->nInserts);
            if (same) {
                for (int k = 0; k < a->nInserts && same; k++) {
                    if (strcmp(ours->lines[a->insertsBefore[k]],
                               theirs->lines[b->insertsBefore[k]]) != 0)
                        same = false;
                }
            }
            if (same) {
                // Identical insertions — take ours.
                for (int k = 0; k < a->nInserts; k++)
                    fputs(ours->lines[a->insertsBefore[k]], out);
            } else {
                // Conflicting insertions.
                conflicts++;
                fprintf(out, "<<<<<<< %s\n", oursLabel);
                for (int k = 0; k < a->nInserts; k++)
                    fputs(ours->lines[a->insertsBefore[k]], out);
                fprintf(out, "||||||| base\n");
                fprintf(out, "=======\n");
                for (int k = 0; k < b->nInserts; k++)
                    fputs(theirs->lines[b->insertsBefore[k]], out);
                fprintf(out, ">>>>>>> %s\n", theirsLabel);
            }
        } else if (aHasIns) {
            for (int k = 0; k < a->nInserts; k++)
                fputs(ours->lines[a->insertsBefore[k]], out);
        } else if (bHasIns) {
            for (int k = 0; k < b->nInserts; k++)
                fputs(theirs->lines[b->insertsBefore[k]], out);
        }

        if (i == base->count) break;  // past-end slot, only insertions matter

        // ---- Handle the base line itself ----

        bool aDel = (a->change == CHG_DEL);
        bool bDel = (b->change == CHG_DEL);

        if (!aDel && !bDel) {
            // Both kept base line unchanged.
            fputs(base->lines[i], out);
        } else if (aDel && bDel) {
            // Both deleted — no output needed.
        } else if (aDel && !bDel) {
            // Only ours deleted it — take ours (delete the line).
        } else if (!aDel && bDel) {
            // Only theirs deleted it — take theirs (delete the line).
        }
        // Note: replacement (DEL + INS at same slot) is handled by the
        // insertions logic above since the edit script always pairs a
        // deletion with insertions at the adjacent slot.
    }

    fclose(out);

    // Cleanup.
    la_free(base); la_free(ours); la_free(theirs);
    free(esA->edits); free(esA);
    free(esB->edits); free(esB);
    map_free(mapA, base->count - 1 >= 0 ? base->count : 0);
    map_free(mapB, base->count - 1 >= 0 ? base->count : 0);

    return conflicts > 0 ? 1 : 0;
}
