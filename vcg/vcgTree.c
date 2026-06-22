#include "vcg.h"
#define VCG_FILE_VER_vcgTree 1

gboolean readField(const gchar *filePath, const gchar *key,
                           gchar *buf, gsize bufSz)
{
    FILE *f = fopen(filePath, "r");
    if (!f) return FALSE;
    char line[MAX_LINE];
    size_t klen = strlen(key);
    gboolean found = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            const char *p = line + klen;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                size_t vlen = strlen(p);
                while (vlen > 0 &&
                       (p[vlen-1] == '\n' || p[vlen-1] == '\r' || p[vlen-1] == ' '))
                    vlen--;
                snprintf(buf, bufSz, "%.*s", (int)vlen, p);
                found = TRUE;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

/* Read .vc/HEAD -> active branch name */
gchar *readActiveBranch(const gchar *workDir)
{
    gchar *path = g_build_filename(workDir, ".vc", "HEAD", NULL);
    FILE *f = fopen(path, "r");
    g_free(path);
    if (!f) return g_strdup("main");
    char line[128] = {0};
    if (!fgets(line, sizeof(line), f)) { fclose(f); return g_strdup("main"); }
    fclose(f);
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
    return len > 0 ? g_strdup(line) : g_strdup("main");
}

/* Read one-line text file (e.g. HEAD_COMMIT) */
gchar *readOneLine(const gchar *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[4096] = {0};
    if (!fgets(line, sizeof(line), f)) { fclose(f); return NULL; }
    fclose(f);
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
    return len > 0 ? g_strdup(line) : NULL;
}

/* Comparator: sort zip names newest-first (they are YYYYMMDD_HHMMSS.zip) */
int cmpZipNewest(const void *a, const void *b)
{
    return strcmp(*(const char **)b, *(const char **)a);
}

void refreshTree(VcG *app)
{
    syncWorkDir(app);
    gtk_tree_store_clear(app->treeStore);

    /* ------------------------------------------------------------------ */
    /* Section: Branches                                                   */
    /* ------------------------------------------------------------------ */
    gtk_tree_store_append(app->treeStore, &app->iterBranches, NULL);
    gtk_tree_store_set(app->treeStore, &app->iterBranches,
        TREE_COL_ICON,    "📁",
        TREE_COL_LABEL,   "Branches",
        TREE_COL_TOOLTIP, "",
        TREE_COL_TYPE,    "section",
        TREE_COL_NAME,    "",
        -1);

    gchar *activeBranch = readActiveBranch(app->workDir);

    gchar *branchesDir = g_build_filename(app->workDir, ".vc", "branches", NULL);

    DIR *bd = opendir(branchesDir);
    if (bd) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        struct dirent *de;
        while ((de = readdir(bd))) {
            if (de->d_name[0] == '.') continue;
            gchar *path = g_build_filename(branchesDir, de->d_name, NULL);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                g_ptr_array_add(names, g_strdup(de->d_name));
            g_free(path);
        }
        closedir(bd);
        g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);

        for (guint i = 0; i < names->len; i++) {
            const gchar *bname = names->pdata[i];
            gboolean active = (strcmp(bname, activeBranch) == 0);

            gchar *infoPath = g_build_filename(branchesDir, bname, "info", NULL);
            gchar created[64]=""; gchar author[64]=""; gchar from[64]="";
            readField(infoPath, "created", created, sizeof(created));
            readField(infoPath, "author",  author,  sizeof(author));
            readField(infoPath, "from",    from,    sizeof(from));
            g_free(infoPath);

            gchar *hcPath = g_build_filename(branchesDir, bname, "HEAD_COMMIT", NULL);
            gchar *headCommit = readOneLine(hcPath);
            g_free(hcPath);

            gchar *label   = active
                ? g_strdup_printf("★ %s  (active)", bname)
                : g_strdup_printf("   %s", bname);
            gchar *tooltip = g_strdup_printf(
                "Branch: %s\nActive: %s\nCreated: %s\nAuthor: %s\nFrom: %s\nHEAD commit: %s",
                bname, active ? "yes" : "no",
                created[0]   ? created    : "–",
                author[0]    ? author     : "–",
                from[0]      ? from       : "–",
                headCommit   ? headCommit : "none");

            GtkTreeIter iter;
            gtk_tree_store_append(app->treeStore, &iter, &app->iterBranches);
            gtk_tree_store_set(app->treeStore, &iter,
                TREE_COL_ICON,    active ? "✦" : "⎇",
                TREE_COL_LABEL,   label,
                TREE_COL_TOOLTIP, tooltip,
                TREE_COL_TYPE,    "branch",
                TREE_COL_NAME,    bname,
                -1);

            g_free(label);
            g_free(tooltip);
            g_free(headCommit);
        }
        g_ptr_array_free(names, TRUE);
    } else {
        GtkTreeIter iter;
        gtk_tree_store_append(app->treeStore, &iter, &app->iterBranches);
        gtk_tree_store_set(app->treeStore, &iter,
            TREE_COL_ICON,    "⚠",
            TREE_COL_LABEL,   "(no .vc found – run Init)",
            TREE_COL_TOOLTIP, "Run 'vc init' in the working directory first.",
            TREE_COL_TYPE,    "section",
            TREE_COL_NAME,    "",
            -1);
    }

    g_free(activeBranch);
    g_free(branchesDir);

    /* ------------------------------------------------------------------ */
    /* Section: Tags                                                        */
    /* ------------------------------------------------------------------ */
    gtk_tree_store_append(app->treeStore, &app->iterTags, NULL);
    gtk_tree_store_set(app->treeStore, &app->iterTags,
        TREE_COL_ICON,    "🏷",
        TREE_COL_LABEL,   "Tags",
        TREE_COL_TOOLTIP, "",
        TREE_COL_TYPE,    "section",
        TREE_COL_NAME,    "",
        -1);

    gchar *tagsDir = g_build_filename(app->workDir, ".vc", "tags", NULL);

    DIR *td = opendir(tagsDir);
    if (td) {
        GPtrArray *tagNames = g_ptr_array_new_with_free_func(g_free);
        struct dirent *de;
        while ((de = readdir(td))) {
            if (de->d_name[0] == '.') continue;
            gchar *path = g_build_filename(tagsDir, de->d_name, NULL);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                g_ptr_array_add(tagNames, g_strdup(de->d_name));
            g_free(path);
        }
        closedir(td);
        g_ptr_array_sort(tagNames, (GCompareFunc)g_strcmp0);

        for (guint i = 0; i < tagNames->len; i++) {
            const gchar *tname = tagNames->pdata[i];
            gchar *tpath = g_build_filename(tagsDir, tname, NULL);

            gchar commit[128]=""; gchar branch[64]="";
            gchar author[64]="";  gchar date[64]="";
            gchar message[256]="";
            readField(tpath, "commit",  commit,  sizeof(commit));
            readField(tpath, "branch",  branch,  sizeof(branch));
            readField(tpath, "author",  author,  sizeof(author));
            readField(tpath, "date",    date,    sizeof(date));
            readField(tpath, "message", message, sizeof(message));
            g_free(tpath);

            gchar *label = date[0]
                ? g_strdup_printf("🏷 %s  (%s)", tname, date)
                : g_strdup_printf("🏷 %s", tname);
            gchar *tooltip = g_strdup_printf(
                "Tag: %s\nCommit: %s\nBranch: %s\nAuthor: %s\nDate: %s\nMessage: %s",
                tname,
                commit[0]  ? commit  : "–",
                branch[0]  ? branch  : "–",
                author[0]  ? author  : "–",
                date[0]    ? date    : "–",
                message[0] ? message : "–");

            GtkTreeIter iter;
            gtk_tree_store_append(app->treeStore, &iter, &app->iterTags);
            gtk_tree_store_set(app->treeStore, &iter,
                TREE_COL_ICON,    "🏷",
                TREE_COL_LABEL,   label,
                TREE_COL_TOOLTIP, tooltip,
                TREE_COL_TYPE,    "tag",
                TREE_COL_NAME,    tname,
                -1);
            g_free(label);
            g_free(tooltip);
        }
        if (tagNames->len == 0) {
            GtkTreeIter iter;
            gtk_tree_store_append(app->treeStore, &iter, &app->iterTags);
            gtk_tree_store_set(app->treeStore, &iter,
                TREE_COL_ICON, "", TREE_COL_LABEL, "(no tags)",
                TREE_COL_TOOLTIP, "", TREE_COL_TYPE, "section", TREE_COL_NAME, "", -1);
        }
        g_ptr_array_free(tagNames, TRUE);
    }

    /* ------------------------------------------------------------------ */
    /* Section: Commits                                                    */
    /* ------------------------------------------------------------------ */
    gtk_tree_store_append(app->treeStore, &app->iterCommits, NULL);
    gtk_tree_store_set(app->treeStore, &app->iterCommits,
        TREE_COL_ICON,    "📦",
        TREE_COL_LABEL,   "Commits",
        TREE_COL_TOOLTIP, "",
        TREE_COL_TYPE,    "section",
        TREE_COL_NAME,    "",
        -1);

    gchar *dataDir = g_build_filename(app->workDir, ".vc", "data", NULL);

    DIR *dd = opendir(dataDir);
    if (dd) {
        /* Build tag->commit lookup */
        GHashTable *tagForCommit = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, g_free);

        DIR *td2 = opendir(tagsDir);
        if (td2) {
            struct dirent *de;
            while ((de = readdir(td2))) {
                if (de->d_name[0] == '.') continue;
                gchar *tp = g_build_filename(tagsDir, de->d_name, NULL);
                gchar commit[128] = "";
                if (readField(tp, "commit", commit, sizeof(commit)) && commit[0]) {
                    gchar *existing = g_hash_table_lookup(tagForCommit, commit);
                    if (existing)
                        g_hash_table_insert(tagForCommit, g_strdup(commit),
                            g_strdup_printf("%s, %s", existing, de->d_name));
                    else
                        g_hash_table_insert(tagForCommit, g_strdup(commit),
                            g_strdup(de->d_name));
                }
                g_free(tp);
            }
            closedir(td2);
        }

        GPtrArray *zips = g_ptr_array_new_with_free_func(g_free);
        struct dirent *de;
        while ((de = readdir(dd))) {
            size_t len = strlen(de->d_name);
            if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
                g_ptr_array_add(zips, g_strdup(de->d_name));
        }
        closedir(dd);
        g_ptr_array_sort(zips, (GCompareFunc)cmpZipNewest);

        for (guint i = 0; i < zips->len; i++) {
            const gchar *zname = zips->pdata[i];
            gchar *zipPath = g_build_filename(dataDir, zname, NULL);

            struct stat zst;
            gchar sizeStr[32] = "";
            if (stat(zipPath, &zst) == 0) {
                if (zst.st_size > 1024*1024)
                    snprintf(sizeStr, sizeof(sizeStr), "%.1f MB",
                             (double)zst.st_size / (1024.0*1024.0));
                else
                    snprintf(sizeStr, sizeof(sizeStr), "%ld KB",
                             (long)(zst.st_size / 1024));
            }
            g_free(zipPath);

            const gchar *tagLabel = g_hash_table_lookup(tagForCommit, zname);
            gchar *label = tagLabel
                ? g_strdup_printf("📦 %s  🏷 %s", zname, tagLabel)
                : g_strdup_printf("📦 %s", zname);
            gchar *tooltip = g_strdup_printf(
                "Commit archive: %s\nSize: %s\nTags: %s",
                zname,
                sizeStr[0] ? sizeStr : "–",
                tagLabel   ? tagLabel : "none");

            GtkTreeIter iter;
            gtk_tree_store_append(app->treeStore, &iter, &app->iterCommits);
            gtk_tree_store_set(app->treeStore, &iter,
                TREE_COL_ICON,    "📦",
                TREE_COL_LABEL,   label,
                TREE_COL_TOOLTIP, tooltip,
                TREE_COL_TYPE,    "commit",
                TREE_COL_NAME,    zname,
                -1);
            g_free(label);
            g_free(tooltip);
        }

        if (zips->len == 0) {
            GtkTreeIter iter;
            gtk_tree_store_append(app->treeStore, &iter, &app->iterCommits);
            gtk_tree_store_set(app->treeStore, &iter,
                TREE_COL_ICON, "", TREE_COL_LABEL, "(no commits yet)",
                TREE_COL_TOOLTIP, "", TREE_COL_TYPE, "section", TREE_COL_NAME, "", -1);
        }

        g_ptr_array_free(zips, TRUE);
        g_hash_table_destroy(tagForCommit);
    }

    g_free(tagsDir);
    g_free(dataDir);

    /* ------------------------------------------------------------------ */
    /* Section: Stash                                                       */
    /* ------------------------------------------------------------------ */
    gtk_tree_store_append(app->treeStore, &app->iterStash, NULL);
    gtk_tree_store_set(app->treeStore, &app->iterStash,
        TREE_COL_ICON,    "📥",
        TREE_COL_LABEL,   "Stash",
        TREE_COL_TOOLTIP, "Saved stash entries (.vc/stash/)",
        TREE_COL_TYPE,    "section",
        TREE_COL_NAME,    "",
        -1);
    {
        gchar *stashDir = g_build_filename(app->workDir, ".vc", "stash", NULL);
        DIR   *sd = opendir(stashDir);
        if (sd) {
            GPtrArray *snames = g_ptr_array_new_with_free_func(g_free);
            struct dirent *de;
            while ((de = readdir(sd))) {
                if (de->d_name[0] == '.') continue;
                gchar *sp = g_build_filename(stashDir, de->d_name, NULL);
                struct stat st;
                if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode))
                    g_ptr_array_add(snames, g_strdup(de->d_name));
                g_free(sp);
            }
            closedir(sd);
            g_ptr_array_sort(snames, (GCompareFunc)g_strcmp0);

            for (gint si = (gint)snames->len - 1; si >= 0; si--) {
                const gchar *sname = snames->pdata[si];
                gchar *msgPath = g_build_filename(stashDir, sname, "message", NULL);
                gchar *msg     = readOneLine(msgPath);
                g_free(msgPath);

                gchar *label   = msg && msg[0]
                    ? g_strdup_printf("📥 %s  — %s", sname, msg)
                    : g_strdup_printf("📥 %s", sname);
                gchar *tooltip = g_strdup_printf("Stash: %s\nMessage: %s",
                    sname, msg && msg[0] ? msg : "(none)");

                GtkTreeIter iter;
                gtk_tree_store_append(app->treeStore, &iter, &app->iterStash);
                gtk_tree_store_set(app->treeStore, &iter,
                    TREE_COL_ICON,    "📥",
                    TREE_COL_LABEL,   label,
                    TREE_COL_TOOLTIP, tooltip,
                    TREE_COL_TYPE,    "stash",
                    TREE_COL_NAME,    sname,
                    -1);
                g_free(label); g_free(tooltip); g_free(msg);
            }
            if (snames->len == 0) {
                GtkTreeIter iter;
                gtk_tree_store_append(app->treeStore, &iter, &app->iterStash);
                gtk_tree_store_set(app->treeStore, &iter,
                    TREE_COL_ICON,"",TREE_COL_LABEL,"(no stashes)",
                    TREE_COL_TOOLTIP,"",TREE_COL_TYPE,"section",TREE_COL_NAME,"",-1);
            }
            g_ptr_array_free(snames, TRUE);
        }
        g_free(stashDir);
    }

    /* ------------------------------------------------------------------ */
    /* Section: Archive                                                     */
    /* ------------------------------------------------------------------ */
    gtk_tree_store_append(app->treeStore, &app->iterArchive, NULL);
    gtk_tree_store_set(app->treeStore, &app->iterArchive,
        TREE_COL_ICON,    "🗄",
        TREE_COL_LABEL,   "Archive",
        TREE_COL_TOOLTIP, "Archived commit zips (.vc/archive/)",
        TREE_COL_TYPE,    "section",
        TREE_COL_NAME,    "",
        -1);
    {
        gchar *archDir = g_build_filename(app->workDir, ".vc", "archive", NULL);
        DIR   *ad = opendir(archDir);
        if (ad) {
            GPtrArray *azips = g_ptr_array_new_with_free_func(g_free);
            struct dirent *de;
            while ((de = readdir(ad))) {
                size_t len = strlen(de->d_name);
                if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
                    g_ptr_array_add(azips, g_strdup(de->d_name));
            }
            closedir(ad);
            g_ptr_array_sort(azips, (GCompareFunc)cmpZipNewest);

            for (guint ai = 0; ai < azips->len; ai++) {
                const gchar *zname = azips->pdata[ai];
                gchar *zipPath = g_build_filename(archDir, zname, NULL);
                struct stat zst;
                gchar sizeStr[32] = "";
                if (stat(zipPath, &zst) == 0) {
                    if (zst.st_size >= 1024*1024)
                        snprintf(sizeStr, sizeof(sizeStr), "%.1f MB",
                                 (double)zst.st_size/(1024.0*1024));
                    else
                        snprintf(sizeStr, sizeof(sizeStr), "%ld KB",
                                 (long)(zst.st_size/1024));
                }
                g_free(zipPath);
                gchar *label   = g_strdup_printf("🗄 %s  %s", zname, sizeStr);
                gchar *tooltip = g_strdup_printf("Archived commit: %s\nSize: %s",
                                                  zname, sizeStr);
                GtkTreeIter iter;
                gtk_tree_store_append(app->treeStore, &iter, &app->iterArchive);
                gtk_tree_store_set(app->treeStore, &iter,
                    TREE_COL_ICON,    "🗄",
                    TREE_COL_LABEL,   label,
                    TREE_COL_TOOLTIP, tooltip,
                    TREE_COL_TYPE,    "archive",
                    TREE_COL_NAME,    zname,
                    -1);
                g_free(label); g_free(tooltip);
            }
            if (azips->len == 0) {
                GtkTreeIter iter;
                gtk_tree_store_append(app->treeStore, &iter, &app->iterArchive);
                gtk_tree_store_set(app->treeStore, &iter,
                    TREE_COL_ICON,"",TREE_COL_LABEL,"(no archived commits)",
                    TREE_COL_TOOLTIP,"",TREE_COL_TYPE,"section",TREE_COL_NAME,"",-1);
            }
            g_ptr_array_free(azips, TRUE);
        }
        g_free(archDir);
    }

    /* Populate branch/tag/checkout combo dropdowns */
    populateCombos(app);

    /* Refresh the sidebar file list */
    refreshFileList(app);

    /* Expand all sections */
    gtk_tree_view_expand_all(GTK_TREE_VIEW(app->treeView));
}

/* -----------------------------------------------------------------------
 * refreshFileList  –  populate the sidebar GtkListBox with the tracked
 * files for the current branch, read from the branch index file.
 *
 * Index line format:  relative/path|state|mtime|size
 * States: staged, committed, modified
 * --------------------------------------------------------------------- */
void refreshFileList(VcG *app)
{
    if (!app->fileListBox || !app->workDir || !app->workDir[0]) return;

    /* Remove all existing rows */
    GList *rows = gtk_container_get_children(GTK_CONTAINER(app->fileListBox));
    for (GList *l = rows; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(rows);

    /* Update section label with current branch name */
    gchar *activeBr = readActiveBranch(app->workDir);
    if (app->fileListLabel) {
        gchar *ltext = g_strdup_printf("📁 Files  [%s]",
                                        activeBr && activeBr[0] ? activeBr : "?");
        gtk_label_set_text(GTK_LABEL(app->fileListLabel), ltext);
        g_free(ltext);
    }

    /* Read index file */
    gchar *indexPath = g_build_filename(app->workDir, ".vc", "branches",
                                        activeBr ? activeBr : "", "index", NULL);
    g_free(activeBr);

    FILE *f = fopen(indexPath, "r");
    g_free(indexPath);

    if (!f) {
        /* No index yet — show a placeholder */
        GtkWidget *lbl = gtk_label_new("(no tracked files)");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(lbl, 4);
        gtk_container_add(GTK_CONTAINER(app->fileListBox), lbl);
        gtk_widget_show_all(app->fileListBox);
        return;
    }

    char line[4096];
    gint count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';
        if (!ll) continue;

        /* Split on first pipe to get path and state */
        char *pipe1 = strchr(line, '|');
        if (!pipe1) continue;
        *pipe1 = '\0';
        const char *filePath  = line;
        const char *stateStr  = pipe1 + 1;

        /* Truncate state at next pipe */
        char *pipe2 = strchr(stateStr, '|');
        if (pipe2) *pipe2 = '\0';

        /* Choose icon based on state; colour applied via CSS class below */
        const gchar *icon;
        const gchar *cssClass;
        if (strcmp(stateStr, "staged") == 0) {
            icon = "●"; cssClass = "file-staged";
        } else if (strcmp(stateStr, "modified") == 0) {
            icon = "◐"; cssClass = "file-modified";
        } else {
            icon = "○"; cssClass = NULL;   /* committed / clean — default colour */
        }

        /* Build a one-line row: icon + filename */
        const char *slash = strrchr(filePath, '/');
        const char *base  = slash ? slash + 1 : filePath;
        gchar *rowText = (slash)
            ? g_strdup_printf("%s %s  %.*s", icon, base,
                              (int)(slash - filePath + 1), filePath)
            : g_strdup_printf("%s %s", icon, filePath);

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(box), 2);

        GtkWidget *lbl = gtk_label_new(rowText);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
        /* No tooltip — the text itself is sufficient */

        if (cssClass)
            gtk_style_context_add_class(gtk_widget_get_style_context(lbl), cssClass);

        gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), box);

        /* Store the full path so double-click can load it */
        g_object_set_data_full(G_OBJECT(row), "filepath",
                               g_strdup(filePath), g_free);

        gtk_container_add(GTK_CONTAINER(app->fileListBox), row);
        g_free(rowText);
        count++;
    }
    fclose(f);

    if (count == 0) {
        GtkWidget *lbl = gtk_label_new("(no tracked files)");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(lbl, 4);
        gtk_container_add(GTK_CONTAINER(app->fileListBox), lbl);
    }

    gtk_widget_show_all(app->fileListBox);
}

/* -----------------------------------------------------------------------
 * Tree row activated: double-click populates entry fields
 * --------------------------------------------------------------------- */
void onTreeRowActivated(GtkTreeView *tv, GtkTreePath *path,
                                GtkTreeViewColumn *col, gpointer data)
{
    VcG *app = (VcG *)data;
    (void)col;

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->treeStore), &iter, path))
        return;

    gchar *type = NULL, *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->treeStore), &iter,
                       TREE_COL_TYPE, &type, TREE_COL_NAME, &name, -1);

    if (type && name && name[0]) {
        if (strcmp(type, "branch") == 0) {
            comboSetText(app->branchCombo, name);
            setStatus(app, "Double-click: branch name loaded. Press Switch/Create to switch.");
        } else if (strcmp(type, "tag") == 0) {
            comboSetText(app->tagCombo, name);
            comboSetText(app->checkoutCombo, name);
            setStatus(app, "Double-click: tag name loaded.");
        } else if (strcmp(type, "commit") == 0) {
            comboSetText(app->checkoutCombo, name);
            setStatus(app, "Double-click: commit zip loaded into Checkout / Show field.");
        } else if (strcmp(type, "stash") == 0) {
            comboSetText(app->stashNameCombo, name);
            setStatus(app, "Double-click: stash name loaded.");
        } else if (strcmp(type, "archive") == 0) {
            comboSetText(app->archiveRestoreCombo, name);
            setStatus(app, "Double-click: archive zip loaded into Restore field.");
        }
    }
    g_free(type);
    g_free(name);
}

/* Tree row right-click context menu */
gboolean onTreeButtonPress(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    VcG *app = (VcG *)data;
    if (event->button != 3) return FALSE;   /* only right-click */

    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                        (gint)event->x, (gint)event->y,
                                        &path, NULL, NULL, NULL))
        return FALSE;

    GtkTreeIter iter;
    gtk_tree_model_get_iter(GTK_TREE_MODEL(app->treeStore), &iter, path);
    gtk_tree_path_free(path);

    gchar *type = NULL, *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->treeStore), &iter,
                       TREE_COL_TYPE, &type, TREE_COL_NAME, &name, -1);

    if (!type || !name || !name[0]) { g_free(type); g_free(name); return FALSE; }

    GtkWidget *menu = gtk_menu_new();

    if (strcmp(type, "branch") == 0) {
        GtkWidget *switchMI = gtk_menu_item_new_with_label("Switch to this branch");
        GtkWidget *deleteMI = gtk_menu_item_new_with_label("Delete this branch");

        /* Pass name via label since we can't capture in C99 without a struct */
        comboSetText(app->branchCombo, name);

        g_signal_connect_swapped(switchMI, "activate",
            G_CALLBACK(gtk_button_clicked), app->branchBtn);
        g_signal_connect_swapped(deleteMI, "activate",
            G_CALLBACK(gtk_button_clicked), app->deleteBranchBtn);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), switchMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), deleteMI);

    } else if (strcmp(type, "tag") == 0) {
        GtkWidget *checkoutMI = gtk_menu_item_new_with_label("Checkout this tag");
        GtkWidget *deleteMI   = gtk_menu_item_new_with_label("Delete this tag");
        comboSetText(app->tagCombo, name);
        comboSetText(app->checkoutCombo, name);
        g_signal_connect_swapped(checkoutMI, "activate",
            G_CALLBACK(gtk_button_clicked), app->checkoutTagBtn);
        g_signal_connect_swapped(deleteMI, "activate",
            G_CALLBACK(gtk_button_clicked), app->deleteTagBtn);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), checkoutMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), deleteMI);

    } else if (strcmp(type, "commit") == 0) {
        GtkWidget *showMI      = gtk_menu_item_new_with_label("Show commit details");
        GtkWidget *checkoutMI  = gtk_menu_item_new_with_label("Checkout this commit (--zip)");
        GtkWidget *listFilesMI = gtk_menu_item_new_with_label("List files in this commit");
        comboSetText(app->checkoutCombo, name);
        g_signal_connect_swapped(showMI,      "activate", G_CALLBACK(gtk_button_clicked), app->showCommitBtn);
        g_signal_connect_swapped(checkoutMI,  "activate", G_CALLBACK(gtk_button_clicked), app->checkoutTagBtn);
        g_signal_connect_swapped(listFilesMI, "activate", G_CALLBACK(gtk_button_clicked), app->listCommitFilesBtn);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), showMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), checkoutMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), listFilesMI);

    } else if (strcmp(type, "stash") == 0) {
        GtkWidget *popMI   = gtk_menu_item_new_with_label("Pop this stash");
        GtkWidget *applyMI = gtk_menu_item_new_with_label("Apply this stash (keep)");
        GtkWidget *dropMI  = gtk_menu_item_new_with_label("Drop (delete) this stash");
        comboSetText(app->stashNameCombo, name);
        g_signal_connect_swapped(popMI,   "activate", G_CALLBACK(gtk_button_clicked), app->stashPopBtn);
        g_signal_connect_swapped(applyMI, "activate", G_CALLBACK(gtk_button_clicked), app->stashApplyBtn);
        g_signal_connect_swapped(dropMI,  "activate", G_CALLBACK(gtk_button_clicked), app->stashDropBtn);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), popMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), applyMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), dropMI);

    } else if (strcmp(type, "archive") == 0) {
        GtkWidget *restoreMI = gtk_menu_item_new_with_label("Restore this commit to active");
        GtkWidget *showMI    = gtk_menu_item_new_with_label("Show commit details");
        comboSetText(app->archiveRestoreCombo, name);
        comboSetText(app->checkoutCombo, name);
        g_signal_connect_swapped(restoreMI, "activate", G_CALLBACK(gtk_button_clicked), app->archiveRestoreBtn);
        g_signal_connect_swapped(showMI,    "activate", G_CALLBACK(gtk_button_clicked), app->showCommitBtn);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), restoreMI);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), showMI);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    g_free(type);
    g_free(name);
    return TRUE;
}


void populateCombos(VcG *app)
{
    if (!app->workDir || !app->workDir[0]) return;

    /* --- Branch names --- */
    gchar *branchesDir = g_build_filename(app->workDir, ".vc", "branches", NULL);
    GPtrArray *bnames  = g_ptr_array_new_with_free_func(g_free);
    DIR *bd = opendir(branchesDir);
    if (bd) {
        struct dirent *de;
        while ((de = readdir(bd))) {
            if (de->d_name[0] == '.') continue;
            gchar *p = g_build_filename(branchesDir, de->d_name, NULL);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
                g_ptr_array_add(bnames, g_strdup(de->d_name));
            g_free(p);
        }
        closedir(bd);
        g_ptr_array_sort(bnames, (GCompareFunc)g_strcmp0);
    }
    g_free(branchesDir);

    comboRepopulate(app->branchCombo, (gchar **)bnames->pdata, (gint)bnames->len);
    comboRepopulate(app->mergeCombo,  (gchar **)bnames->pdata, (gint)bnames->len);

    /* Default branchCombo to the current active branch if the field is empty */
    if (comboGetText(app->branchCombo)[0] == '\0') {
        gchar *active = readActiveBranch(app->workDir);
        if (active && active[0])
            comboSetText(app->branchCombo, active);
        g_free(active);
    }

    g_ptr_array_free(bnames, TRUE);

    /* --- Tag names --- */
    gchar *tagsDir = g_build_filename(app->workDir, ".vc", "tags", NULL);
    GPtrArray *tnames = g_ptr_array_new_with_free_func(g_free);
    DIR *td = opendir(tagsDir);
    if (td) {
        struct dirent *de;
        while ((de = readdir(td))) {
            if (de->d_name[0] == '.') continue;
            gchar *p = g_build_filename(tagsDir, de->d_name, NULL);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
                g_ptr_array_add(tnames, g_strdup(de->d_name));
            g_free(p);
        }
        closedir(td);
        g_ptr_array_sort(tnames, (GCompareFunc)g_strcmp0);
    }
    g_free(tagsDir);

    comboRepopulate(app->tagCombo, (gchar **)tnames->pdata, (gint)tnames->len);

    /* --- Checkout combo: tags first, then commit zips newest-first --- */
    GPtrArray *coItems = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < tnames->len; i++)
        g_ptr_array_add(coItems, g_strdup(tnames->pdata[i]));

    gchar *dataDir = g_build_filename(app->workDir, ".vc", "data", NULL);
    GPtrArray *zips = g_ptr_array_new_with_free_func(g_free);
    DIR *dd = opendir(dataDir);
    if (dd) {
        struct dirent *de;
        while ((de = readdir(dd))) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".zip") == 0)
                g_ptr_array_add(zips, g_strdup(de->d_name));
        }
        closedir(dd);
        g_ptr_array_sort(zips, (GCompareFunc)cmpZipNewest);
        for (guint i = 0; i < zips->len; i++)
            g_ptr_array_add(coItems, g_strdup(zips->pdata[i]));
    }
    g_free(dataDir);
    g_ptr_array_free(zips, TRUE);

    comboRepopulate(app->checkoutCombo,
                    (gchar **)coItems->pdata, (gint)coItems->len);
    g_ptr_array_free(coItems, TRUE);
    g_ptr_array_free(tnames, TRUE);

    /* --- Stash names, newest-first --- */
    gchar *stashDir = g_build_filename(app->workDir, ".vc", "stash", NULL);
    GPtrArray *snames = g_ptr_array_new_with_free_func(g_free);
    DIR *sd = opendir(stashDir);
    if (sd) {
        struct dirent *de;
        while ((de = readdir(sd))) {
            if (de->d_name[0] == '.') continue;
            gchar *p = g_build_filename(stashDir, de->d_name, NULL);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
                g_ptr_array_add(snames, g_strdup(de->d_name));
            g_free(p);
        }
        closedir(sd);
        /* Sort newest-first: stash names are timestamps, so reverse alpha */
        g_ptr_array_sort(snames, (GCompareFunc)cmpZipNewest);
    }
    g_free(stashDir);
    comboRepopulate(app->stashNameCombo,
                    (gchar **)snames->pdata, (gint)snames->len);
    g_ptr_array_free(snames, TRUE);

    /* --- Archive zips newest-first --- */
    gchar *archDir = g_build_filename(app->workDir, ".vc", "archive", NULL);
    GPtrArray *azips = g_ptr_array_new_with_free_func(g_free);
    DIR *ad = opendir(archDir);
    if (ad) {
        struct dirent *de;
        while ((de = readdir(ad))) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".zip") == 0)
                g_ptr_array_add(azips, g_strdup(de->d_name));
        }
        closedir(ad);
        g_ptr_array_sort(azips, (GCompareFunc)cmpZipNewest);
    }
    g_free(archDir);
    comboRepopulate(app->archiveRestoreCombo,
                    (gchar **)azips->pdata, (gint)azips->len);
    g_ptr_array_free(azips, TRUE);

    /* --- Tracked files from the active branch index ---
     * Index file: .vc/branches/<branch>/index
     * Format per line: relative/path|state|mtime|size           */
    gchar *activeBr  = readActiveBranch(app->workDir);
    gchar *indexPath = g_build_filename(app->workDir, ".vc", "branches",
                                        activeBr, "index", NULL);
    g_free(activeBr);

    GPtrArray *fnames = g_ptr_array_new_with_free_func(g_free);
    FILE *idxf = fopen(indexPath, "r");
    if (idxf) {
        char line[4096];
        while (fgets(line, sizeof(line), idxf)) {
            /* Strip newline */
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
                line[--ll] = '\0';
            if (!ll) continue;
            /* First field before the first '|' is the file path */
            char *pipe = strchr(line, '|');
            if (pipe) *pipe = '\0';
            if (line[0])
                g_ptr_array_add(fnames, g_strdup(line));
        }
        fclose(idxf);
        g_ptr_array_sort(fnames, (GCompareFunc)g_strcmp0);
    }
    g_free(indexPath);
    comboRepopulate(app->fileCombo,
                    (gchar **)fnames->pdata, (gint)fnames->len);
    g_ptr_array_free(fnames, TRUE);
}
