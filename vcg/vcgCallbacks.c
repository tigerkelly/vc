#include "vcg.h"
#define VCG_FILE_VER_vcgCallbacks 1

/* -----------------------------------------------------------------------
 * Button callbacks
 * --------------------------------------------------------------------- */
void onRefreshTreeClicked(GtkButton *b, gpointer d)
    { (void)b; refreshTree((VcG *)d); setStatus((VcG *)d, "Tree refreshed."); }

void onStatusClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "status"); }

void onAddAllClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "add"); }

void onLogClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "log"); }

void onListClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "list"); }

void onPullClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "pull"); }

void onPushClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "push"); }

void onVersionClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "version"); }

/* Light CSS applied to any modal dialog so widgets are readable.
 * Added to the screen before gtk_widget_show_all() and removed on destroy. */

void onInitClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;

    GtkCssProvider *lightCss = dialogLightCssAdd();

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Init Repository", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Init", GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid    = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(content), grid);

    const gchar *labels[] = {"User:","Email:","Project:","Repo Path:","Repo Host:","Repo Login:", NULL};
    const gchar *flags[]  = {"--user","--email","--project","--repoPath","--repoHost","--repoLogin", NULL};
    GtkWidget *entries[8] = {NULL};
    for (int i = 0; labels[i]; i++) {
        GtkWidget *lbl = gtk_label_new(labels[i]);
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        entries[i] = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(entries[i]), 32);
        gtk_grid_attach(GTK_GRID(grid), lbl,        0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entries[i], 1, i, 1, 1);
        gtk_widget_set_hexpand(entries[i], TRUE);
    }

    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GPtrArray *av = g_ptr_array_new();
        g_ptr_array_add(av, VC_CMD);
        g_ptr_array_add(av, "init");
        for (int i = 0; flags[i]; i++) {
            const gchar *val = gtk_entry_get_text(GTK_ENTRY(entries[i]));
            if (val && val[0]) {
                g_ptr_array_add(av, (gpointer)flags[i]);
                g_ptr_array_add(av, (gpointer)val);
            }
        }
        g_ptr_array_add(av, NULL);
        clearOutput(app);
        runVcCmd(app, (gchar **)av->pdata);
        g_ptr_array_free(av, FALSE);
    }
    gtk_widget_destroy(dialog);
    dialogLightCssRemove(lightCss);
}

void onConfigClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); gchar *av[] = {VC_CMD,"config","--show",NULL}; runVcCmd((VcG *)d,av); }

void onBranchClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->branchCombo);
    clearOutput(app);
    if (name && name[0]) { gchar *av[] = {VC_CMD,"branch",(gchar*)name,NULL}; runVcCmd(app,av); }
    else RUN1(app,"branch");
}

void onDeleteBranchClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->branchCombo);
    if (!name || !name[0]) {
        appendOutput(app, "Enter a branch name to delete.\n", TAG_WARNING);
        return;
    }

    /* Protect the main branch — it cannot be deleted */
    if (strcmp(name, "main") == 0) {
        appendOutput(app, "ERROR: The 'main' branch cannot be deleted.\n", TAG_ERROR);
        return;
    }

    /* Protect the currently active branch */
    gchar *active = readActiveBranch(app->workDir);
    if (active && strcmp(name, active) == 0) {
        gchar *msg = g_strdup_printf(
            "ERROR: Cannot delete '%s' — it is the currently active branch.\n"
            "Switch to a different branch first.\n", name);
        appendOutput(app, msg, TAG_ERROR);
        g_free(msg);
        g_free(active);
        return;
    }
    g_free(active);

    clearOutput(app);
    gchar *av[] = {VC_CMD, "branch", "--delete", (gchar *)name, NULL};
    runVcCmd(app, av);
}

void onMergeOursClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *br = comboGetText(app->mergeCombo);
    if (!br||!br[0]){appendOutput(app,"Enter a branch name to merge.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"merge",(gchar*)br,"--ours",NULL}; runVcCmd(app,av);
}

void onMergeTheirsClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *br = comboGetText(app->mergeCombo);
    if (!br||!br[0]){appendOutput(app,"Enter a branch name to merge.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"merge",(gchar*)br,"--theirs",NULL}; runVcCmd(app,av);
}

void onInfoClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "info"); }

void onShowCommitClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *val = comboGetText(app->checkoutCombo);
    clearOutput(app);
    if (val && val[0]) {
        /* If it looks like a tag use --tag, else treat as archive name */
        size_t vlen = strlen(val);
        if (vlen > 4 && strcmp(val+vlen-4,".zip")==0) {
            gchar *av[]={VC_CMD,"show",(gchar*)val,NULL}; runVcCmd(app,av);
        } else {
            gchar *av[]={VC_CMD,"show","--tag",(gchar*)val,NULL}; runVcCmd(app,av);
        }
    } else {
        /* No argument — show most recent commit */
        RUN1(app,"show");
    }
}

void onDiffMeldClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"diff","--meld",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onHistoryFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"history",(gchar*)file,NULL}; runVcCmd(app,av);
}

/* ---- Ignore ---- */
void onIgnoreListClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "ignore"); }

void onIgnoreAddClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *pat = gtk_entry_get_text(GTK_ENTRY(app->ignoreEntry));
    if (!pat||!pat[0]){appendOutput(app,"Enter a pattern to add.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"ignore",(gchar*)pat,NULL}; runVcCmd(app,av);
}

void onIgnoreDeleteClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *pat = gtk_entry_get_text(GTK_ENTRY(app->ignoreEntry));
    if (!pat||!pat[0]){appendOutput(app,"Enter a pattern to remove.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"ignore","--delete",(gchar*)pat,NULL}; runVcCmd(app,av);
}

void onIgnoreCheckClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *pat = gtk_entry_get_text(GTK_ENTRY(app->ignoreEntry));
    if (!pat||!pat[0]){appendOutput(app,"Enter a filename to check.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"ignore","--check",(gchar*)pat,NULL}; runVcCmd(app,av);
}

/* ---- Stash ---- */
void onStashPushClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *msg = gtk_entry_get_text(GTK_ENTRY(app->stashMsgEntry));
    clearOutput(app);
    if (msg && msg[0]) { gchar *av[]={VC_CMD,"stash","--msg",(gchar*)msg,NULL}; runVcCmd(app,av); }
    else RUN1(app,"stash");
}

void onStashPopClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->stashNameCombo);
    clearOutput(app);
    if (name && name[0]) { gchar *av[]={VC_CMD,"stash","pop",(gchar*)name,NULL}; runVcCmd(app,av); }
    else { gchar *av[]={VC_CMD,"stash","pop",NULL}; runVcCmd(app,av); }
}

void onStashApplyClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->stashNameCombo);
    clearOutput(app);
    if (name && name[0]) { gchar *av[]={VC_CMD,"stash","apply",(gchar*)name,NULL}; runVcCmd(app,av); }
    else { gchar *av[]={VC_CMD,"stash","apply",NULL}; runVcCmd(app,av); }
}

void onStashListClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); gchar *av[]={VC_CMD,"stash","list",NULL}; runVcCmd((VcG *)d,av); }

void onStashDropClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->stashNameCombo);
    if (!name||!name[0]){appendOutput(app,"Enter a stash name to drop.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"stash","drop",(gchar*)name,NULL}; runVcCmd(app,av);
}

/* ---- Archive ---- */
void onArchiveClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *keep = gtk_entry_get_text(GTK_ENTRY(app->archiveKeepEntry));
    clearOutput(app);
    if (keep && keep[0]) {
        gchar *av[]={VC_CMD,"archive","--keep",(gchar*)keep,NULL}; runVcCmd(app,av);
    } else {
        RUN1(app,"archive");
    }
}

void onArchiveAllClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); gchar *av[]={VC_CMD,"archive","--all",NULL}; runVcCmd((VcG *)d,av); }

void onArchiveListClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); gchar *av[]={VC_CMD,"archive","--list",NULL}; runVcCmd((VcG *)d,av); }

void onArchiveRestoreClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *zip = comboGetText(app->archiveRestoreCombo);
    if (!zip||!zip[0]){appendOutput(app,"Enter a zip name to restore.\n",TAG_WARNING);return;}
    clearOutput(app);
    gchar *av[]={VC_CMD,"archive","--restore",(gchar*)zip,NULL}; runVcCmd(app,av);
}

void onCommitClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *msg = gtk_entry_get_text(GTK_ENTRY(app->commitMsgEntry));
    clearOutput(app);
    if (msg && msg[0]) { gchar *av[] = {VC_CMD,"commit","--msg",(gchar*)msg,NULL}; runVcCmd(app,av); }
    else RUN1(app,"commit");
}

void onTagClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->tagCombo);
    const gchar *tmsg = gtk_entry_get_text(GTK_ENTRY(app->tagMsgEntry));
    clearOutput(app);
    if (name && name[0]) {
        if (tmsg && tmsg[0]) { gchar *av[] = {VC_CMD,"tag",(gchar*)name,"--msg",(gchar*)tmsg,NULL}; runVcCmd(app,av); }
        else                 { gchar *av[] = {VC_CMD,"tag",(gchar*)name,NULL}; runVcCmd(app,av); }
    } else RUN1(app,"tag");
}

void onDeleteTagClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *name = comboGetText(app->tagCombo);
    if (!name || !name[0]) { appendOutput(app,"Enter a tag name to delete.\n",TAG_WARNING); return; }
    clearOutput(app);
    gchar *av[] = {VC_CMD,"tag","--delete",(gchar*)name,NULL}; runVcCmd(app,av);
}

void onAddFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"add",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onDeleteFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"delete",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onRevertFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"revert",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onDiffFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"diff",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onCheckoutFileClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]){appendOutput(app,"Enter a file name.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"checkout",(gchar*)file,NULL}; runVcCmd(app,av);
}

void onRenameClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *oN = comboGetText(app->fileCombo);
    const gchar *nN = gtk_entry_get_text(GTK_ENTRY(app->fileEntry2));
    if (!oN||!oN[0]||!nN||!nN[0]){appendOutput(app,"Enter old and new names.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"rename",(gchar*)oN,(gchar*)nN,NULL}; runVcCmd(app,av);
}

void onCheckoutAllClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); gchar *av[]={VC_CMD,"checkout","--all",NULL}; runVcCmd((VcG *)d,av); }

void onCheckoutTagClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *tag = comboGetText(app->checkoutCombo);
    if (!tag||!tag[0]){appendOutput(app,"Enter a tag name.\n",TAG_WARNING);return;}
    clearOutput(app);
    /* If it looks like a zip file use --zip, otherwise --tag */
    size_t len = strlen(tag);
    if (len > 4 && strcmp(tag + len - 4, ".zip") == 0) {
        gchar *av[]={VC_CMD,"checkout","--zip",(gchar*)tag,NULL}; runVcCmd(app,av);
    } else {
        gchar *av[]={VC_CMD,"checkout","--tag",(gchar*)tag,NULL}; runVcCmd(app,av);
    }
}

/* -----------------------------------------------------------------------
 * List files inside a commit zip with full metadata
 *
 * Two subprocesses:
 *   1. unzip -p <zip> MANIFEST.txt  — commit header (author, date, message)
 *   2. unzip -v <zip>               — per-file: size, compressed, %, date,
 *                                     time, CRC-32, name
 * --------------------------------------------------------------------- */

/* Run a command synchronously, return heap-allocated stdout. Caller frees. */
static gchar *runAndCapture(gchar **argv)
{
    GError *err = NULL;
    gchar  *out = NULL;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH,
                 NULL, NULL, &out, NULL, NULL, &err);
    if (err) { g_clear_error(&err); g_free(out); return NULL; }
    return out;   /* may be NULL if nothing captured */
}

/* Format a byte count as human-readable, always exactly 9 chars wide */
static void fmtBytes(long bytes, gchar *buf, gsize sz)
{
    if (bytes < 0)
        snprintf(buf, sz, "        -");          /*  9 chars */
    else if (bytes >= 1024L*1024*1024)
        snprintf(buf, sz, "%6.1fG  ", (double)bytes / (1024.0*1024*1024)); /* 6+1+2=9 */
    else if (bytes >= 1024L*1024)
        snprintf(buf, sz, "%6.1fM  ", (double)bytes / (1024.0*1024));
    else if (bytes >= 1024)
        snprintf(buf, sz, "%6.1fK  ", (double)bytes / 1024.0);
    else
        snprintf(buf, sz, "%7ldB  ", bytes);     /* 7+1+1=9 — e.g. "     35B " */
    buf[9] = '\0';   /* guarantee termination at exactly 9 */
}

static void listCommitFiles(VcG *app, const gchar *zipName)
{
    syncWorkDir(app);
    gchar *zipPath = g_build_filename(app->workDir, ".vc", "data", zipName, NULL);

    clearOutput(app);

    /* ------------------------------------------------------------------
     * Step 1 – read MANIFEST.txt for commit header info
     * ---------------------------------------------------------------- */
    gchar *manifestArgv[] = { "unzip", "-p", zipPath, "MANIFEST.txt", NULL };
    gchar *manifest = runAndCapture(manifestArgv);

    /* Helper: extract "field : value" from manifest text */
    gchar mAuthor[128]="", mDate[64]="", mMessage[256]="", mProject[128]="";
    if (manifest) {
        /* parse "key : value" lines */
        gchar **mlines = g_strsplit(manifest, "\n", 0);
        for (int i = 0; mlines[i]; i++) {
            gchar *l = g_strstrip(mlines[i]);
#define MFIELD(key, dst) \
    if (g_str_has_prefix(l, key)) { \
        const gchar *v = l + strlen(key); \
        while (*v==':' || *v==' ' || *v=='\t') v++; \
        g_strlcpy(dst, v, sizeof(dst)); }
            MFIELD("author",  mAuthor)
            MFIELD("date",    mDate)
            MFIELD("message", mMessage)
            MFIELD("project", mProject)
#undef MFIELD
        }
        g_strfreev(mlines);
        g_free(manifest);
    }

    /* Commit header block */
    gchar *hdr = g_strdup_printf(
        "Commit:  %s\n"
        "Project: %s\n"
        "Author:  %s\n"
        "Date:    %s\n"
        "Message: %s\n"
        "─────────────────────────────────────────────────────────"
        "──────────────────────\n",
        zipName,
        mProject[0] ? mProject : "–",
        mAuthor[0]  ? mAuthor  : "–",
        mDate[0]    ? mDate    : "–",
        mMessage[0] ? mMessage : "–");
    appendOutput(app, hdr, TAG_HEADING);
    g_free(hdr);

    /* Column header — positions match data row format exactly:
       2 + 9 + 2 + 9 + 2 + 4 + 2 + 10 + 2 + 5 + 2 + 8 + 2 + name      */
    appendOutput(app,
        "  Size          Stored   Cmpr  Date        Time   CRC-32    Name\n"
        "  ---------  ---------  ----  ----------  -----  --------  ----\n",
        TAG_INFO);

    /* ------------------------------------------------------------------
     * Step 2 – run  unzip -v  for per-file metadata
     *
     * unzip -v output format (one line per entry):
     *   Length   Method    Size  Cmpr    Date    Time   CRC-32   Name
     *  --------  ------  ------- ---- ---------- ----- --------  ----
     *        12  Stored       12   0% 2025-05-13 14:23 af083b2d  src/main.c
     * ---------------------------------------------------------------- */
    gchar *verbArgv[] = { "unzip", "-v", zipPath, NULL };
    gchar *verbOut = runAndCapture(verbArgv);

    gint fileCount = 0;
    gint dirCount  = 0;
    long totalOrig = 0, totalStored = 0;

    if (verbOut) {
        gchar **vlines = g_strsplit(verbOut, "\n", 0);
        for (int i = 0; vlines[i]; i++) {
            const gchar *l = vlines[i];

            /* Skip header/footer/blank/separator lines.
               Data lines start with spaces then a digit (the size). */
            const gchar *p = l;
            while (*p == ' ') p++;
            if (!g_ascii_isdigit(*p)) continue;

            /* Parse fields by position — unzip -v uses fixed columns:
               cols 0-8   : Length   (right-justified)
               cols 10-15 : Method
               cols 18-24 : Size
               cols 26-28 : Cmpr%
               cols 30-39 : Date (YYYY-MM-DD)
               cols 41-45 : Time (HH:MM)
               cols 47-54 : CRC-32
               cols 57+   : Name
               We use sscanf which handles variable spacing robustly. */
            long   origSz = 0, storSz = 0;
            int    cmprPct = 0;
            char   method[16]="", date[16]="", time2[16]="", crc[16]="", name[1024]="";

            int parsed = sscanf(l,
                " %ld %15s %ld %d%% %15s %15s %15s %1023[^\n]",
                &origSz, method, &storSz, &cmprPct,
                date, time2, crc, name);

            if (parsed < 8) continue;

            /* Skip MANIFEST.txt */
            if (strcmp(name, "MANIFEST.txt") == 0) continue;

            gboolean isDir = (name[strlen(name)-1] == '/');

            gchar origBuf[12], storBuf[12];
            fmtBytes(origSz, origBuf, sizeof(origBuf));
            fmtBytes(storSz, storBuf, sizeof(storBuf));

            /* Row: fmtBytes already produces exactly 9 chars each */
            gchar *row = g_strdup_printf(
                "  %s  %s  %3d%%  %10s  %5s  %-8s  %s\n",
                origBuf, storBuf, cmprPct,
                date, time2, crc, name);

            appendOutput(app, row, isDir ? TAG_INFO : TAG_NORMAL);
            g_free(row);

            if (isDir) dirCount++;
            else { fileCount++; totalOrig += origSz; totalStored += storSz; }
        }
        g_strfreev(vlines);
        g_free(verbOut);
    }

    if (fileCount == 0 && dirCount == 0)
        appendOutput(app, "  (no entries — is unzip installed?)\n", TAG_WARNING);

    /* Summary footer */
    gchar origBuf[12], storBuf[12];
    fmtBytes(totalOrig,   origBuf, sizeof(origBuf));
    fmtBytes(totalStored, storBuf, sizeof(storBuf));

    gint savings = (totalOrig > 0)
        ? (gint)((1.0 - (double)totalStored / (double)totalOrig) * 100.0 + 0.5)
        : 0;

    gchar *summary = g_strdup_printf(
        "  ---------  ---------  -------------------------------------------\n"
        "  %s  %s  %d file(s), %d dir(s) — %d%% space saved\n",
        origBuf, storBuf, fileCount, dirCount, savings);
    appendOutput(app, summary, TAG_HEADING);
    g_free(summary);

    g_free(zipPath);
    setStatus(app, "Commit file listing complete.");
}

/* Resolve a tag name -> zip filename by reading .vc/tags/<tag> commit field */
static gchar *resolveTagToZip(VcG *app, const gchar *tagName)
{
    syncWorkDir(app);
    gchar *tagPath = g_build_filename(app->workDir, ".vc", "tags", tagName, NULL);
    gchar commit[128] = "";
    readField(tagPath, "commit", commit, sizeof(commit));
    g_free(tagPath);
    return commit[0] ? g_strdup(commit) : NULL;
}

void onListCommitFilesClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *val = comboGetText(app->checkoutCombo);
    if (!val || !val[0]) {
        appendOutput(app,
            "Enter a commit filename (e.g. 20250513_142300.zip) or tag name,\n"
            "then click List Files.  Double-click a commit in the tree to load it.\n",
            TAG_WARNING);
        return;
    }

    size_t vlen = strlen(val);
    if (vlen > 4 && strcmp(val + vlen - 4, ".zip") == 0) {
        /* Looks like a zip filename — use directly */
        listCommitFiles(app, val);
    } else {
        /* Treat as a tag name — look up its commit zip */
        gchar *zip = resolveTagToZip(app, val);
        if (zip) {
            listCommitFiles(app, zip);
            g_free(zip);
        } else {
            gchar *msg = g_strdup_printf(
                "Cannot find commit for tag '%s'.\n"
                "Enter a .zip filename or a valid tag name.\n", val);
            appendOutput(app, msg, TAG_WARNING);
            g_free(msg);
        }
    }
}

void onCloneClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *url = gtk_entry_get_text(GTK_ENTRY(app->cloneUrlEntry));
    if (!url||!url[0]){appendOutput(app,"Enter a URL.\n",TAG_WARNING);return;}
    clearOutput(app); gchar *av[]={VC_CMD,"clone",(gchar*)url,NULL}; runVcCmd(app,av);
}

/* File list row double-clicked — load the file path into fileCombo */
void onFileListRowActivated(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    VcG *app = (VcG *)data; (void)lb;
    const gchar *path = g_object_get_data(G_OBJECT(row), "filepath");
    if (path && path[0])
        comboSetText(app->fileCombo, path);
}

void onBrowseClicked(GtkButton *btn, gpointer data)
{
    VcG *app = (VcG *)data; (void)btn;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Working Directory", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    const gchar *cur = gtk_entry_get_text(GTK_ENTRY(app->dirEntry));
    if (cur && cur[0]) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), cur);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gtk_entry_set_text(GTK_ENTRY(app->dirEntry), folder);
        g_free(app->workDir); app->workDir = folder;
        refreshTree(app);
        gchar *msg = g_strdup_printf("Working directory: %s", folder);
        setStatus(app, msg); g_free(msg);
    }
    gtk_widget_destroy(chooser);
}

void onDirEntryActivate(GtkEntry *entry, gpointer data)
{
    VcG *app = (VcG *)data; (void)entry;
    syncWorkDir(app);
    refreshTree(app);
    setStatus(app, "Working directory updated.");
}


/* -----------------------------------------------------------------------
 * Auth callbacks  (vc auth / vc auth --clear)
 * --------------------------------------------------------------------- */
void onAuthClicked(GtkButton *b, gpointer d)
    { (void)b; clearOutput((VcG *)d); RUN1((VcG *)d, "auth"); }

void onAuthClearClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    clearOutput(app);
    gchar *av[] = {VC_CMD, "auth", "--clear", NULL};
    runVcCmd(app, av);
}

/* -----------------------------------------------------------------------
 * Move callback  (vc move <src> <dest>)
 * --------------------------------------------------------------------- */
void onMoveClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *src  = comboGetText(app->fileCombo);
    const gchar *dest = gtk_entry_get_text(GTK_ENTRY(app->fileEntry2));
    if (!src||!src[0]||!dest||!dest[0]) {
        appendOutput(app, "Enter source file and destination path.\n", TAG_WARNING);
        return;
    }
    clearOutput(app);
    gchar *av[] = {VC_CMD, "move", (gchar*)src, (gchar*)dest, NULL};
    runVcCmd(app, av);
}

/* -----------------------------------------------------------------------
 * Untrack callback  (vc untrack <file>)
 * --------------------------------------------------------------------- */
void onUntrackClicked(GtkButton *b, gpointer d)
{
    VcG *app = (VcG *)d; (void)b;
    const gchar *file = comboGetText(app->fileCombo);
    if (!file||!file[0]) {
        appendOutput(app, "Enter a file name to untrack.\n", TAG_WARNING);
        return;
    }
    clearOutput(app);
    gchar *av[] = {VC_CMD, "untrack", (gchar*)file, NULL};
    runVcCmd(app, av);
}
void onAboutClicked(GtkMenuItem *item, gpointer data)
{
    VcG *app = (VcG *)data; (void)item;
    GtkWidget *about = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "vcg");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), VCG_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about),
        "Graphical front-end for the vc version-control system.\n"
        "Repository tree shows branches, tags and commits directly\n"
        "from .vc/ — no subprocess required.");
    gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(app->window));
    gtk_dialog_run(GTK_DIALOG(about));
    gtk_widget_destroy(about);
}
