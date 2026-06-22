/*
 * vcg.h  –  Shared types, constants, and forward declarations for vcg.
 *
 * Every .c file in the project includes this header.
 */
#ifndef VCG_H
#define VCG_H

#pragma once   /* redundant on compilers that support it, but harmless */

#include <gtk/gtk.h>
#include <sys/wait.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include "vcVersion.h"

/* -----------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */
#define VCG_VERSION         APP_VERSION
#define VC_CMD           "vc"
#define MAX_LINE         512

/* Tree view column indices */
enum {
    TREE_COL_ICON = 0,
    TREE_COL_LABEL,
    TREE_COL_TOOLTIP,
    TREE_COL_TYPE,
    TREE_COL_NAME,
    TREE_N_COLS
};

/* Output colour tag names */
#define TAG_NORMAL   "normal"
#define TAG_SUCCESS  "success"
#define TAG_WARNING  "warning"
#define TAG_ERROR    "error"
#define TAG_INFO     "info"
#define TAG_HEADING  "heading"
#define TAG_CMD      "cmd"

/* -----------------------------------------------------------------------
 * VcConfig  –  persistent settings
 * --------------------------------------------------------------------- */
typedef struct {
    gint  winWidth;
    gint  winHeight;
    gint  pane1Pos;
    gint  pane2Pos;
    gint  winX;
    gint  winY;

    gint  uiFontSize;
    gint  treeFontSize;
    gint  outputFontSize;
    gchar outputFontFamily[64];

    gchar colBg[16];
    gchar colBgDark[16];
    gchar colFg[16];
    gchar colAccent[16];
    gchar colSuccess[16];
    gchar colWarning[16];
    gchar colError[16];

    gchar vcBinary[256];
    gchar lastWorkDir[4096];
    gint  autoRefreshTree;
} VcConfig;

/* -----------------------------------------------------------------------
 * VcG  –  complete application state
 * --------------------------------------------------------------------- */
typedef struct {
    /* ---- Window ---- */
    GtkWidget *window;

    /* ---- Sidebar ---- */
    GtkWidget *dirEntry;
    GtkWidget *browseBtn;
    GtkWidget *refreshTreeBtn;
    GtkWidget *statusBtn;
    GtkWidget *addBtn;
    GtkWidget *listBtn;
    GtkWidget *logBtn;
    GtkWidget *pullBtn;
    GtkWidget *pushBtn;
    GtkWidget *initBtn;
    GtkWidget *configBtn;
    GtkWidget *versionBtn;
    GtkWidget *infoBtn;
    GtkWidget *authBtn;
    GtkWidget *authClearBtn;

    /* ---- Sidebar file list ---- */
    GtkWidget *fileListBox;     /* GtkListBox showing tracked files    */
    GtkWidget *fileListLabel;   /* section label showing branch name   */

    /* ---- Repository tree ---- */
    GtkWidget        *treeView;
    GtkTreeStore     *treeStore;
    GtkTreeIter       iterBranches;
    GtkTreeIter       iterTags;
    GtkTreeIter       iterCommits;
    GtkTreeIter       iterStash;
    GtkTreeIter       iterArchive;

    /* ---- Controls ---- */
    GtkWidget *branchCombo;
    GtkWidget *branchBtn;
    GtkWidget *deleteBranchBtn;
    GtkWidget *mergeCombo;
    GtkWidget *mergeOursBtn;
    GtkWidget *mergeTheirsBtn;
    GtkWidget *commitMsgEntry;
    GtkWidget *commitBtn;
    GtkWidget *tagCombo;
    GtkWidget *tagMsgEntry;
    GtkWidget *tagBtn;
    GtkWidget *deleteTagBtn;

    GtkWidget *fileCombo;
    GtkWidget *fileEntry2;
    GtkWidget *addFileBtn;
    GtkWidget *deleteFileBtn;
    GtkWidget *revertFileBtn;
    GtkWidget *diffFileBtn;
    GtkWidget *diffMeldBtn;
    GtkWidget *historyFileBtn;
    GtkWidget *checkoutFileBtn;
    GtkWidget *renameBtn;
    GtkWidget *moveBtn;
    GtkWidget *untrackBtn;

    GtkWidget *checkoutAllBtn;
    GtkWidget *checkoutCombo;
    GtkWidget *checkoutTagBtn;
    GtkWidget *listCommitFilesBtn;
    GtkWidget *showCommitBtn;

    GtkWidget *cloneUrlEntry;
    GtkWidget *cloneBtn;

    /* ---- Ignore ---- */
    GtkWidget *ignoreEntry;
    GtkWidget *ignoreAddBtn;
    GtkWidget *ignoreDeleteBtn;
    GtkWidget *ignoreCheckBtn;
    GtkWidget *ignoreListBtn;

    /* ---- Stash ---- */
    GtkWidget *stashMsgEntry;
    GtkWidget *stashPushBtn;
    GtkWidget *stashPopBtn;
    GtkWidget *stashApplyBtn;
    GtkWidget *stashListBtn;
    GtkWidget *stashNameCombo;
    GtkWidget *stashDropBtn;

    /* ---- Archive ---- */
    GtkWidget *archiveKeepEntry;
    GtkWidget *archiveBtn;
    GtkWidget *archiveAllBtn;
    GtkWidget *archiveListBtn;
    GtkWidget *archiveRestoreCombo;
    GtkWidget *archiveRestoreBtn;

    /* ---- Output ---- */
    GtkWidget     *outputView;
    GtkTextBuffer *outputBuf;

    /* ---- Status bar ---- */
    GtkWidget *statusBar;
    guint      statusCtx;

    /* ---- Async process ---- */
    GPid       childPid;
    gint       childStdout;
    gint       childStderr;
    guint      ioWatchId;
    guint      ioErrWatchId;
    GIOChannel *ioChan;
    GIOChannel *ioChanErr;

    /* ---- Working directory ---- */
    gchar *workDir;

    /* ---- Persistent config ---- */
    VcConfig       cfg;
    GtkCssProvider *cssProvider;
    GtkCssProvider *outputFontProvider;

    /* ---- Paned widgets ---- */
    GtkWidget *hpaned1;
    GtkWidget *hpaned2;
} VcG;

#define RUN1(app, sub) \
    do { gchar *_av[] = {VC_CMD, (sub), NULL}; runVcCmd((app), _av); } while(0)

/* -----------------------------------------------------------------------
 * vcg_config.c
 * --------------------------------------------------------------------- */
gchar  *configFilePath(void);
void    configDefaults(VcConfig *cfg);
void    configLoad(VcConfig *cfg);
void    configSave(const VcConfig *cfg);
gchar  *configBuildCss(const VcConfig *cfg);

/* -----------------------------------------------------------------------
 * vcg_output.c
 * --------------------------------------------------------------------- */
void         appendOutput(VcG *app, const gchar *text, const gchar *tagName);
void         setStatus(VcG *app, const gchar *msg);
void         clearOutput(VcG *app);
const gchar *pickTag(const gchar *line);
void         syncWorkDir(VcG *app);
void         runVcCmd(VcG *app, gchar **argv);

/* -----------------------------------------------------------------------
 * vcg_tree.c
 * --------------------------------------------------------------------- */
gboolean  readField(const gchar *filePath, const gchar *key,
                    gchar *buf, gsize bufSz);
gchar    *readActiveBranch(const gchar *workDir);
gchar    *readOneLine(const gchar *path);
int       cmpZipNewest(const void *a, const void *b);
void      refreshTree(VcG *app);
void      refreshFileList(VcG *app);
void      populateCombos(VcG *app);

/* -----------------------------------------------------------------------
 * vcg_callbacks.c
 * --------------------------------------------------------------------- */
void onRefreshTreeClicked(GtkButton *b, gpointer d);
void onStatusClicked(GtkButton *b, gpointer d);
void onAddAllClicked(GtkButton *b, gpointer d);
void onLogClicked(GtkButton *b, gpointer d);
void onListClicked(GtkButton *b, gpointer d);
void onPullClicked(GtkButton *b, gpointer d);
void onPushClicked(GtkButton *b, gpointer d);
void onVersionClicked(GtkButton *b, gpointer d);
void onInfoClicked(GtkButton *b, gpointer d);
void onConfigClicked(GtkButton *b, gpointer d);
void onBranchClicked(GtkButton *b, gpointer d);
void onDeleteBranchClicked(GtkButton *b, gpointer d);
void onMergeOursClicked(GtkButton *b, gpointer d);
void onMergeTheirsClicked(GtkButton *b, gpointer d);
void onShowCommitClicked(GtkButton *b, gpointer d);
void onDiffMeldClicked(GtkButton *b, gpointer d);
void onHistoryFileClicked(GtkButton *b, gpointer d);
void onIgnoreListClicked(GtkButton *b, gpointer d);
void onIgnoreAddClicked(GtkButton *b, gpointer d);
void onIgnoreDeleteClicked(GtkButton *b, gpointer d);
void onIgnoreCheckClicked(GtkButton *b, gpointer d);
void onStashPushClicked(GtkButton *b, gpointer d);
void onStashPopClicked(GtkButton *b, gpointer d);
void onStashApplyClicked(GtkButton *b, gpointer d);
void onStashListClicked(GtkButton *b, gpointer d);
void onStashDropClicked(GtkButton *b, gpointer d);
void onArchiveClicked(GtkButton *b, gpointer d);
void onArchiveAllClicked(GtkButton *b, gpointer d);
void onArchiveListClicked(GtkButton *b, gpointer d);
void onArchiveRestoreClicked(GtkButton *b, gpointer d);
void onCommitClicked(GtkButton *b, gpointer d);
void onTagClicked(GtkButton *b, gpointer d);
void onDeleteTagClicked(GtkButton *b, gpointer d);
void onAddFileClicked(GtkButton *b, gpointer d);
void onDeleteFileClicked(GtkButton *b, gpointer d);
void onRevertFileClicked(GtkButton *b, gpointer d);
void onDiffFileClicked(GtkButton *b, gpointer d);
void onCheckoutFileClicked(GtkButton *b, gpointer d);
void onRenameClicked(GtkButton *b, gpointer d);
void onCheckoutAllClicked(GtkButton *b, gpointer d);
void onCheckoutTagClicked(GtkButton *b, gpointer d);
void onListCommitFilesClicked(GtkButton *b, gpointer d);
void onCloneClicked(GtkButton *b, gpointer d);
void onBrowseClicked(GtkButton *btn, gpointer data);
void onDirEntryActivate(GtkEntry *entry, gpointer data);
void onAboutClicked(GtkMenuItem *item, gpointer data);
void onAuthClicked(GtkButton *b, gpointer d);
void onAuthClearClicked(GtkButton *b, gpointer d);
void onMoveClicked(GtkButton *b, gpointer d);
void onUntrackClicked(GtkButton *b, gpointer d);
void onTreeRowActivated(GtkTreeView *tv, GtkTreePath *path,
                        GtkTreeViewColumn *col, gpointer data);
gboolean onTreeButtonPress(GtkWidget *widget, GdkEventButton *event, gpointer data);
void onInitClicked(GtkButton *b, gpointer d);
void onFileListRowActivated(GtkListBox *lb, GtkListBoxRow *row, gpointer data);

/* -----------------------------------------------------------------------
 * vcg_prefs.c
 * --------------------------------------------------------------------- */
void             hexToRgba(const gchar *hex, GdkRGBA *rgba);
void             rgbaToHex(const GdkRGBA *rgba, gchar *buf, gsize sz);
void             applyConfig(VcG *app);
GtkCssProvider  *dialogLightCssAdd(void);
void             dialogLightCssRemove(GtkCssProvider *p);
gboolean         onWindowDelete(GtkWidget *w, GdkEvent *ev, gpointer data);
void             showPrefsDialog(VcG *app);

/* -----------------------------------------------------------------------
 * vcg_ui.c
 * --------------------------------------------------------------------- */
GtkWidget  *makeSection(const gchar *title);
GtkWidget  *makeBtn(const gchar *label, const gchar *cssClass);
GtkWidget  *makeSep(void);
const gchar *comboGetText(GtkWidget *combo);
void         comboSetText(GtkWidget *combo, const gchar *text);
void         comboSetPlaceholder(GtkWidget *combo, const gchar *text);
void         comboRepopulate(GtkWidget *combo, gchar **items, gint nItems);
GtkWidget   *makeCombo(void);
GtkWidget   *buildUi(VcG *app);

#endif /* VCG_H */
