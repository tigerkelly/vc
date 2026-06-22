/*
 * vcg.c  –  Entry point only.
 *
 * IMPORTANT: this project is split across multiple files.
 * If you have an old single-file vcg.c in the same directory,
 * DELETE IT before building.  The required files are:
 *
 *   vcg.c            (this file — main only)
 *   vcg.h
 *   vcgConfig.c
 *   vcgOutput.c
 *   vcgTree.c
 *   vcgCallbacks.c
 *   vcgPrefs.c
 *   vcgUi.c
 *
 * Build:  make
 */

/* Compile-time marker — vcg.h checks for this so the build fails fast
 * with a readable message if an old monolithic vcg.c is compiled instead
 * of this slim entry-point file. */
#define VCG_SPLIT_BUILD 1

#include "vcg.h"

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    VcG app;
    memset(&app, 0, sizeof(app));

    /* Load persistent config (fills defaults, then overlays saved values) */
    configLoad(&app.cfg);

    /* Working directory: last saved or cwd */
    if (app.cfg.lastWorkDir[0])
        app.workDir = g_strdup(app.cfg.lastWorkDir);
    else
        app.workDir = g_strdup(g_get_current_dir());

    buildUi(&app);

    /* Restore working directory into the entry */
    gtk_entry_set_text(GTK_ENTRY(app.dirEntry), app.workDir);

    gtk_widget_show_all(app.window);

    setStatus(&app, "Ready.  Select a working directory, then use the tree and buttons.");
    appendOutput(&app,
        "vcg " VCG_VERSION " — vc Version Control GUI\n"
        "─────────────────────────────────────────────────\n"
        "• Select a working directory (sidebar) or Browse…\n"
        "• The Repository tree (centre) shows Branches, Tags, and Commits\n"
        "  read directly from .vc/ — no subprocess required.\n"
        "• Double-click a branch/tag/commit to load it into the fields.\n"
        "• Right-click for a context menu.\n"
        "• Edit → Preferences to change fonts, colours, window size, and more.\n"
        "• Settings are saved automatically on exit.\n\n",
        TAG_HEADING);

    refreshTree(&app);

    gtk_main();
    g_free(app.workDir);
    return 0;
}
