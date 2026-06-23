/*
 * vcgUi.c
 *
 * Copyright (c) 2026 Kelly Wiles.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "vcg.h"
#define VCG_FILE_VER_vcgUi 1

/* -----------------------------------------------------------------------
 * UI helpers
 * --------------------------------------------------------------------- */
GtkWidget *makeSection(const gchar *title)
{
    GtkWidget *lbl = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "section");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_top(lbl, 8);
    return lbl;
}

GtkWidget *makeBtn(const gchar *label, const gchar *cssClass)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    /* Left-align the label inside the button */
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(btn));
    if (GTK_IS_LABEL(child))
        gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
    if (cssClass)
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), cssClass);
    return btn;
}

GtkWidget *makeSep(void)
{
    return gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
}

/* -----------------------------------------------------------------------
 * ComboBox-with-entry helpers
 *
 * All four combo boxes (branch, merge, tag, checkout) are
 * GtkComboBoxText widgets created with makeCombo().
 * The child GtkEntry is accessed via gtk_bin_get_child().
 * --------------------------------------------------------------------- */

/* Read the current text from a combo-with-entry (typed or selected). */
const gchar *comboGetText(GtkWidget *combo)
{
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combo));
    return entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
}

/* Write text into a combo-with-entry (does not add it to the dropdown). */
void comboSetText(GtkWidget *combo, const gchar *text)
{
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combo));
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), text ? text : "");
}

/* Set placeholder text on the combo's entry child. */
void comboSetPlaceholder(GtkWidget *combo, const gchar *text)
{
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combo));
    if (entry) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), text);
}

/* Clear all dropdown items and re-fill from a NULL-terminated string array.
 * Preserves whatever the user has typed in the entry child. */
void comboRepopulate(GtkWidget *combo, gchar **items, gint nItems)
{
    /* Save current text */
    const gchar *cur = comboGetText(combo);
    gchar *saved = g_strdup(cur && cur[0] ? cur : "");

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (gint i = 0; i < nItems; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);

    /* Restore typed text */
    comboSetText(combo, saved);
    g_free(saved);
}

/* -----------------------------------------------------------------------
 * Populate branch/merge/tag/checkout combos from .vc/
 * Called from refreshTree() so the dropdowns stay in sync.
 * --------------------------------------------------------------------- */


/* -----------------------------------------------------------------------
 * makeCombo  –  create a GtkComboBoxText with editable entry.
 *
 * Row limit: GTK3 CSS does not support max-height on scrolledwindow
 * (that is a GTK4 feature).  Instead we connect to notify::popup-shown
 * and walk the popup's widget tree to find the internal GtkScrolledWindow,
 * then cap its height to ROW_HEIGHT * MAX_ROWS pixels so a scrollbar
 * appears automatically for longer lists.
 * --------------------------------------------------------------------- */
#define COMBO_MAX_ROWS   15
#define COMBO_ROW_HEIGHT 30   /* px — approximate GTK3 default row height */

/* Walk widget tree looking for the first GtkScrolledWindow child */
static GtkWidget *findScrolledWindow(GtkWidget *w)
{
    if (GTK_IS_SCROLLED_WINDOW(w)) return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = children; l; l = l->next) {
            GtkWidget *found = findScrolledWindow(GTK_WIDGET(l->data));
            if (found) { g_list_free(children); return found; }
        }
        g_list_free(children);
    }
    return NULL;
}

/* Called when the combo popup appears or disappears */
static void onComboPopupShown(GObject *obj, GParamSpec *ps, gpointer data)
{
    (void)ps; (void)data;
    GtkComboBox *combo = GTK_COMBO_BOX(obj);
    gboolean shown = FALSE;
    g_object_get(combo, "popup-shown", &shown, NULL);
    if (!shown) return;

    /* The popup is a sibling toplevel window — find it by iterating
     * all toplevel windows and looking for a GtkWindow of type POPUP. */
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next) {
        GtkWidget *win = GTK_WIDGET(l->data);
        if (!GTK_IS_WINDOW(win)) continue;
        if (gtk_window_get_window_type(GTK_WINDOW(win)) != GTK_WINDOW_POPUP)
            continue;
        GtkWidget *sw = findScrolledWindow(win);
        if (sw) {
            gint maxH = COMBO_MAX_ROWS * COMBO_ROW_HEIGHT;
            gint curW, curH;
            gtk_widget_get_size_request(sw, &curW, &curH);
            /* Only shrink, never force-expand */
            if (curH < 0 || curH > maxH)
                gtk_widget_set_size_request(sw, curW, maxH);
        }
    }
    g_list_free(tops);
}

GtkWidget *makeCombo(void)
{
    GtkWidget *combo = gtk_combo_box_text_new_with_entry();
    gtk_combo_box_set_popup_fixed_width(GTK_COMBO_BOX(combo), FALSE);
    g_signal_connect(combo, "notify::popup-shown",
                     G_CALLBACK(onComboPopupShown), NULL);
    return combo;
}

/* -----------------------------------------------------------------------
 * Build UI
 * --------------------------------------------------------------------- */
GtkWidget *buildUi(VcG *app)
{
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "vcg — vc Version Control");

    /* Clamp window size to screen dimensions so it's never larger than
     * the display — prevents the window appearing but being off-screen. */
    GdkScreen *screen = gdk_screen_get_default();
    gint screenW = gdk_screen_get_width(screen);
    gint screenH = gdk_screen_get_height(screen);
    gint w = app->cfg.winWidth  > 0 ? app->cfg.winWidth  : 1400;
    gint h = app->cfg.winHeight > 0 ? app->cfg.winHeight : 800;
    if (w > screenW) w = screenW;
    if (h > screenH) h = screenH;

    gtk_window_set_default_size(GTK_WINDOW(app->window), w, h);

    /* Centre the window unless a saved position is available. */
    if (app->cfg.winX >= 0 && app->cfg.winY >= 0)
        gtk_window_move(GTK_WINDOW(app->window), app->cfg.winX, app->cfg.winY);
    else
        gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);

    gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(onWindowDelete), app);
    g_signal_connect(app->window, "destroy",      G_CALLBACK(gtk_main_quit),  NULL);

    app->cssProvider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(app->cssProvider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    applyConfig(app);  /* push initial CSS */

    /* Outer vbox */
    GtkWidget *outerVbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), outerVbox);

    /* Menu bar */
    GtkWidget *menuBar  = gtk_menu_bar_new();

    /* File menu */
    GtkWidget *fileMenu = gtk_menu_new();
    GtkWidget *fileMI   = gtk_menu_item_new_with_label("File");
    GtkWidget *quitMI   = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quitMI);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(fileMI), fileMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), fileMI);
    g_signal_connect(quitMI, "activate", G_CALLBACK(gtk_main_quit), NULL);

    /* Edit menu */
    GtkWidget *editMenu  = gtk_menu_new();
    GtkWidget *editMI    = gtk_menu_item_new_with_label("Edit");
    GtkWidget *prefsMI   = gtk_menu_item_new_with_label("Preferences…");
    gtk_menu_shell_append(GTK_MENU_SHELL(editMenu), prefsMI);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(editMI), editMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), editMI);
    g_signal_connect_swapped(prefsMI, "activate", G_CALLBACK(showPrefsDialog), app);

    /* Help menu */
    GtkWidget *helpMenu = gtk_menu_new();
    GtkWidget *helpMI   = gtk_menu_item_new_with_label("Help");
    GtkWidget *aboutMI  = gtk_menu_item_new_with_label("About");
    gtk_menu_shell_append(GTK_MENU_SHELL(helpMenu), aboutMI);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(helpMI), helpMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), helpMI);
    g_signal_connect(aboutMI, "activate", G_CALLBACK(onAboutClicked), app);
    gtk_box_pack_start(GTK_BOX(outerVbox), menuBar, FALSE, FALSE, 0);

    /* 3-pane HPaned */
    app->hpaned1 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    app->hpaned2 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(app->hpaned1), app->cfg.pane1Pos);
    gtk_paned_set_position(GTK_PANED(app->hpaned2), app->cfg.pane2Pos);
    gtk_box_pack_start(GTK_BOX(outerVbox), app->hpaned1, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(app->hpaned1), app->hpaned2, TRUE, TRUE);

    /* ================================================================
     * PANE 1 – Sidebar
     * ============================================================== */
    GtkWidget *sideScroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sideScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sideScroll, 210, -1);
    GtkWidget *sideBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(sideBox), 7);
    gtk_container_add(GTK_CONTAINER(sideScroll), sideBox);
    gtk_paned_pack1(GTK_PANED(app->hpaned1), sideScroll, FALSE, FALSE);

    /* Directory */
    gtk_box_pack_start(GTK_BOX(sideBox), makeSection("Working Directory"), FALSE, FALSE, 0);
    app->dirEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->dirEntry), g_get_current_dir());
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->dirEntry), "/path/to/project");
    gtk_box_pack_start(GTK_BOX(sideBox), app->dirEntry, FALSE, FALSE, 0);
    g_signal_connect(app->dirEntry, "activate", G_CALLBACK(onDirEntryActivate), app);

    app->browseBtn = makeBtn("📂  Browse…", NULL);
    gtk_box_pack_start(GTK_BOX(sideBox), app->browseBtn, FALSE, FALSE, 0);
    g_signal_connect(app->browseBtn, "clicked", G_CALLBACK(onBrowseClicked), app);

    app->refreshTreeBtn = makeBtn("🔄  Refresh Tree", NULL);
    gtk_box_pack_start(GTK_BOX(sideBox), app->refreshTreeBtn, FALSE, FALSE, 0);
    g_signal_connect(app->refreshTreeBtn, "clicked", G_CALLBACK(onRefreshTreeClicked), app);

    /* Quick actions */
    gtk_box_pack_start(GTK_BOX(sideBox), makeSection("Quick Actions"), FALSE, FALSE, 0);
    struct { GtkWidget **btn; const gchar *lbl; const gchar *cls; GCallback cb; } qa[] = {
        {&app->statusBtn,  "📋  Status",  "action",  G_CALLBACK(onStatusClicked)},
        {&app->addBtn,     "➕  Add All", "success", G_CALLBACK(onAddAllClicked)},
        {&app->listBtn,    "📄  List",    NULL,      G_CALLBACK(onListClicked)},
        {&app->logBtn,     "📜  Log",     NULL,      G_CALLBACK(onLogClicked)},
        {&app->pullBtn,    "⬇  Pull",    "action",  G_CALLBACK(onPullClicked)},
        {&app->pushBtn,    "⬆  Push",    "action",  G_CALLBACK(onPushClicked)},
        {&app->initBtn,    "🗂  Init…",  NULL,      G_CALLBACK(onInitClicked)},
        {&app->configBtn,  "⚙  Config",  NULL,      G_CALLBACK(onConfigClicked)},
        {&app->versionBtn, "ℹ  Version", NULL,      G_CALLBACK(onVersionClicked)},
        {&app->infoBtn,    "🔍  Info",    NULL,      G_CALLBACK(onInfoClicked)},
        {&app->authBtn,    "🔑  Auth",    NULL,      G_CALLBACK(onAuthClicked)},
        {&app->authClearBtn,"🔓  Clear Auth","danger", G_CALLBACK(onAuthClearClicked)},
        {NULL,NULL,NULL,NULL}
    };
    for (int i = 0; qa[i].btn; i++) {
        *qa[i].btn = makeBtn(qa[i].lbl, qa[i].cls);
        gtk_box_pack_start(GTK_BOX(sideBox), *qa[i].btn, FALSE, FALSE, 0);
        g_signal_connect(*qa[i].btn, "clicked", qa[i].cb, app);
    }

    /* ---- Sidebar file list ---- */
    gtk_box_pack_start(GTK_BOX(sideBox), makeSep(), FALSE, FALSE, 4);

    app->fileListLabel = makeSection("📁 Files");
    gtk_box_pack_start(GTK_BOX(sideBox), app->fileListLabel, FALSE, FALSE, 0);

    GtkWidget *fileListScroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fileListScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(fileListScroll, TRUE);
    gtk_box_pack_start(GTK_BOX(sideBox), fileListScroll, TRUE, TRUE, 0);

    app->fileListBox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->fileListBox),
                                    GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(fileListScroll), app->fileListBox);

    /* Double-click a file row to load its path into the file combo */
    g_signal_connect(app->fileListBox, "row-activated",
                     G_CALLBACK(onFileListRowActivated), app);

    /* ================================================================
     * PANE 2 – Repository Tree
     * ============================================================== */
    GtkWidget *treeFrame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(treeFrame), GTK_SHADOW_IN);
    gtk_widget_set_size_request(treeFrame, 340, -1);

    GtkWidget *treeVbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(treeFrame), treeVbox);
    gtk_paned_pack1(GTK_PANED(app->hpaned2), treeFrame, FALSE, FALSE);

    /* Tree header label */
    GtkWidget *treeHdr = gtk_label_new("Repository");
    gtk_style_context_add_class(gtk_widget_get_style_context(treeHdr), "section");
    gtk_widget_set_margin_start(treeHdr, 8);
    gtk_widget_set_margin_top(treeHdr, 6);
    gtk_widget_set_margin_bottom(treeHdr, 4);
    gtk_widget_set_halign(treeHdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(treeVbox), treeHdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(treeVbox), makeSep(), FALSE, FALSE, 0);

    /* GtkTreeStore: icon, label, tooltip, type, name */
    app->treeStore = gtk_tree_store_new(TREE_N_COLS,
        G_TYPE_STRING,  /* icon  */
        G_TYPE_STRING,  /* label */
        G_TYPE_STRING,  /* tooltip */
        G_TYPE_STRING,  /* type */
        G_TYPE_STRING   /* name */
    );
    app->treeView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->treeStore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->treeView), FALSE);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(app->treeView), TREE_COL_TOOLTIP);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app->treeView), FALSE);

    /* Column: icon + label */
    GtkCellRenderer *iconRend  = gtk_cell_renderer_text_new();
    GtkCellRenderer *labelRend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, iconRend,  FALSE);
    gtk_tree_view_column_pack_start(col, labelRend, TRUE);
    gtk_tree_view_column_add_attribute(col, iconRend,  "text", TREE_COL_ICON);
    gtk_tree_view_column_add_attribute(col, labelRend, "text", TREE_COL_LABEL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->treeView), col);

    g_signal_connect(app->treeView, "row-activated",
                     G_CALLBACK(onTreeRowActivated), app);
    g_signal_connect(app->treeView, "button-press-event",
                     G_CALLBACK(onTreeButtonPress), app);

    GtkWidget *treeScroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(treeScroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(treeScroll), app->treeView);
    gtk_box_pack_start(GTK_BOX(treeVbox), treeScroll, TRUE, TRUE, 0);

    /* ================================================================
     * PANE 3 – Controls (tabbed) + Output
     * ============================================================== */
    GtkWidget *rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(rightBox), 6);
    gtk_paned_pack2(GTK_PANED(app->hpaned2), rightBox, TRUE, TRUE);

    /* Notebook — four tabs keep the controls compact regardless of count */
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(rightBox), nb, FALSE, FALSE, 0);

/* Helper: create a padded VBox to use as a notebook page */
#define MAKE_TAB(varname, title) \
    GtkWidget *varname = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3); \
    gtk_container_set_border_width(GTK_CONTAINER(varname), 6); \
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), varname, gtk_label_new(title));

#define ROW() gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5)
#define PACK(box, w) gtk_box_pack_start(GTK_BOX(box), (w), FALSE, FALSE, 0)
#define PACKX(box, w) gtk_box_pack_start(GTK_BOX(box), (w), TRUE, TRUE, 0)

    /* ---- Tab 1: Source (Branch / Merge / Commit / Tag) ---- */
    MAKE_TAB(srcTab, "Source")

    /* Branch */
    {
        GtkWidget *row = ROW();
        app->branchCombo     = makeCombo();
        comboSetPlaceholder(app->branchCombo, "branch  (blank = list)");
        app->branchBtn       = makeBtn("Switch / Create", "action");
        app->deleteBranchBtn = makeBtn("Delete", "danger");
        PACKX(row, app->branchCombo);
        PACK(row, app->branchBtn);
        PACK(row, app->deleteBranchBtn);
        gtk_box_pack_start(GTK_BOX(srcTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->branchBtn,       "clicked", G_CALLBACK(onBranchClicked),       app);
        g_signal_connect(app->deleteBranchBtn, "clicked", G_CALLBACK(onDeleteBranchClicked), app);
    }
    /* Merge */
    {
        GtkWidget *row = ROW();
        app->mergeCombo      = makeCombo();
        comboSetPlaceholder(app->mergeCombo, "source branch to merge");
        app->mergeOursBtn    = makeBtn("Merge (ours)",   "action");
        app->mergeTheirsBtn  = makeBtn("Merge (theirs)", NULL);
        PACKX(row, app->mergeCombo);
        PACK(row, app->mergeOursBtn);
        PACK(row, app->mergeTheirsBtn);
        gtk_box_pack_start(GTK_BOX(srcTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->mergeOursBtn,   "clicked", G_CALLBACK(onMergeOursClicked),   app);
        g_signal_connect(app->mergeTheirsBtn, "clicked", G_CALLBACK(onMergeTheirsClicked), app);
    }
    /* Commit */
    {
        GtkWidget *row = ROW();
        app->commitMsgEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->commitMsgEntry), "commit message");
        app->commitBtn = makeBtn("Commit", "success");
        PACKX(row, app->commitMsgEntry);
        PACK(row, app->commitBtn);
        gtk_box_pack_start(GTK_BOX(srcTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->commitBtn, "clicked", G_CALLBACK(onCommitClicked), app);
    }
    /* Tag */
    {
        GtkWidget *row = ROW();
        app->tagCombo    = makeCombo();
        comboSetPlaceholder(app->tagCombo, "tag name  (blank = list)");
        gtk_widget_set_size_request(app->tagCombo, 150, -1);
        app->tagMsgEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->tagMsgEntry), "message (opt)");
        app->tagBtn       = makeBtn("Create / List", "action");
        app->deleteTagBtn = makeBtn("Delete", "danger");
        PACK(row, app->tagCombo);
        PACKX(row, app->tagMsgEntry);
        PACK(row, app->tagBtn);
        PACK(row, app->deleteTagBtn);
        gtk_box_pack_start(GTK_BOX(srcTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->tagBtn,       "clicked", G_CALLBACK(onTagClicked),       app);
        g_signal_connect(app->deleteTagBtn, "clicked", G_CALLBACK(onDeleteTagClicked), app);
    }

    /* ---- Tab 2: Files (File Ops / Checkout / Clone) ---- */
    MAKE_TAB(filesTab, "Files")

    /* File ops */
    {
        GtkWidget *row1 = ROW();
        app->fileCombo  = makeCombo();
        comboSetPlaceholder(app->fileCombo, "file / old name");
        app->fileEntry2 = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->fileEntry2), "new name (rename)");
        PACKX(row1, app->fileCombo);
        PACKX(row1, app->fileEntry2);
        gtk_box_pack_start(GTK_BOX(filesTab), row1, FALSE, FALSE, 0);

        GtkWidget *btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        app->addFileBtn      = makeBtn("Add",      "success");
        app->deleteFileBtn   = makeBtn("Delete",   "danger");
        app->revertFileBtn   = makeBtn("Revert",   NULL);
        app->diffFileBtn     = makeBtn("Diff",     NULL);
        app->diffMeldBtn     = makeBtn("Meld",     NULL);
        app->historyFileBtn  = makeBtn("History",  NULL);
        app->checkoutFileBtn = makeBtn("Checkout", "action");
        app->renameBtn       = makeBtn("Rename",   NULL);
        app->moveBtn         = makeBtn("Move",     NULL);
        app->untrackBtn      = makeBtn("Untrack",  "danger");
        GtkWidget *fb[] = {app->addFileBtn, app->deleteFileBtn, app->revertFileBtn,
                           app->diffFileBtn, app->diffMeldBtn, app->historyFileBtn,
                           app->checkoutFileBtn, app->renameBtn,
                           app->moveBtn, app->untrackBtn, NULL};
        for (int i = 0; fb[i]; i++)
            gtk_box_pack_start(GTK_BOX(btnRow), fb[i], TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(filesTab), btnRow, FALSE, FALSE, 0);
        g_signal_connect(app->addFileBtn,      "clicked", G_CALLBACK(onAddFileClicked),      app);
        g_signal_connect(app->deleteFileBtn,   "clicked", G_CALLBACK(onDeleteFileClicked),   app);
        g_signal_connect(app->revertFileBtn,   "clicked", G_CALLBACK(onRevertFileClicked),   app);
        g_signal_connect(app->diffFileBtn,     "clicked", G_CALLBACK(onDiffFileClicked),     app);
        g_signal_connect(app->diffMeldBtn,     "clicked", G_CALLBACK(onDiffMeldClicked),     app);
        g_signal_connect(app->historyFileBtn,  "clicked", G_CALLBACK(onHistoryFileClicked),  app);
        g_signal_connect(app->checkoutFileBtn, "clicked", G_CALLBACK(onCheckoutFileClicked), app);
        g_signal_connect(app->renameBtn,       "clicked", G_CALLBACK(onRenameClicked),       app);
        g_signal_connect(app->moveBtn,         "clicked", G_CALLBACK(onMoveClicked),         app);
        g_signal_connect(app->untrackBtn,      "clicked", G_CALLBACK(onUntrackClicked),      app);
    }
    gtk_box_pack_start(GTK_BOX(filesTab), makeSep(), FALSE, FALSE, 2);
    /* Checkout */
    {
        GtkWidget *row1 = ROW();
        app->checkoutAllBtn = makeBtn("⟳  Restore All Files (--all)", "action");
        gtk_widget_set_tooltip_text(app->checkoutAllBtn,
            "Restore every file from the most recent commit on this branch");
        PACKX(row1, app->checkoutAllBtn);
        gtk_box_pack_start(GTK_BOX(filesTab), row1, FALSE, FALSE, 0);
        g_signal_connect(app->checkoutAllBtn, "clicked",
                         G_CALLBACK(onCheckoutAllClicked), app);

        GtkWidget *row2 = ROW();
        app->checkoutCombo = makeCombo();
        comboSetPlaceholder(app->checkoutCombo, "tag name  or  20250513_142300.zip");
        app->checkoutTagBtn     = makeBtn("Restore",    "action");
        app->showCommitBtn      = makeBtn("Show",       NULL);
        app->listCommitFilesBtn = makeBtn("List Files", NULL);
        gtk_widget_set_tooltip_text(app->checkoutTagBtn,
            "Restore files from the selected tag or commit archive");
        gtk_widget_set_tooltip_text(app->showCommitBtn,
            "Show full details of the selected commit");
        gtk_widget_set_tooltip_text(app->listCommitFilesBtn,
            "List all files stored in the selected commit archive");
        PACKX(row2, app->checkoutCombo);
        PACK(row2, app->checkoutTagBtn);
        PACK(row2, app->showCommitBtn);
        PACK(row2, app->listCommitFilesBtn);
        gtk_box_pack_start(GTK_BOX(filesTab), row2, FALSE, FALSE, 0);
        g_signal_connect(app->checkoutTagBtn,     "clicked",
                         G_CALLBACK(onCheckoutTagClicked),     app);
        g_signal_connect(app->showCommitBtn,      "clicked",
                         G_CALLBACK(onShowCommitClicked),      app);
        g_signal_connect(app->listCommitFilesBtn, "clicked",
                         G_CALLBACK(onListCommitFilesClicked), app);
    }
    gtk_box_pack_start(GTK_BOX(filesTab), makeSep(), FALSE, FALSE, 2);
    /* Clone */
    {
        GtkWidget *row = ROW();
        app->cloneUrlEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->cloneUrlEntry),
                                       "user@host:/path/to/remote/repo");
        gtk_widget_set_tooltip_text(app->cloneUrlEntry,
            "Remote repository location in the form user@host:/path/to/repo");
        app->cloneBtn = makeBtn("Clone Remote Repo", "action");
        gtk_widget_set_tooltip_text(app->cloneBtn,
            "Copy the remote repository into the current working directory");
        PACKX(row, app->cloneUrlEntry);
        PACK(row, app->cloneBtn);
        gtk_box_pack_start(GTK_BOX(filesTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->cloneBtn, "clicked", G_CALLBACK(onCloneClicked), app);
    }

    /* ---- Tab 3: Manage (Ignore / Stash / Archive) ---- */
    MAKE_TAB(mgmtTab, "Manage")

    /* Ignore */
    {
        GtkWidget *row = ROW();
        app->ignoreEntry     = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->ignoreEntry), "pattern or filename");
        app->ignoreListBtn   = makeBtn("List",   NULL);
        app->ignoreAddBtn    = makeBtn("Add",    "success");
        app->ignoreDeleteBtn = makeBtn("Remove", "danger");
        app->ignoreCheckBtn  = makeBtn("Check",  NULL);
        PACKX(row, app->ignoreEntry);
        PACK(row, app->ignoreListBtn);
        PACK(row, app->ignoreAddBtn);
        PACK(row, app->ignoreDeleteBtn);
        PACK(row, app->ignoreCheckBtn);
        gtk_box_pack_start(GTK_BOX(mgmtTab), row, FALSE, FALSE, 0);
        g_signal_connect(app->ignoreListBtn,   "clicked", G_CALLBACK(onIgnoreListClicked),   app);
        g_signal_connect(app->ignoreAddBtn,    "clicked", G_CALLBACK(onIgnoreAddClicked),    app);
        g_signal_connect(app->ignoreDeleteBtn, "clicked", G_CALLBACK(onIgnoreDeleteClicked), app);
        g_signal_connect(app->ignoreCheckBtn,  "clicked", G_CALLBACK(onIgnoreCheckClicked),  app);
    }
    gtk_box_pack_start(GTK_BOX(mgmtTab), makeSep(), FALSE, FALSE, 2);
    /* Stash */
    {
        GtkWidget *row1 = ROW();
        app->stashMsgEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->stashMsgEntry), "stash message (opt)");
        app->stashPushBtn  = makeBtn("Stash", "success");
        app->stashListBtn  = makeBtn("List",  NULL);
        PACKX(row1, app->stashMsgEntry);
        PACK(row1, app->stashPushBtn);
        PACK(row1, app->stashListBtn);
        gtk_box_pack_start(GTK_BOX(mgmtTab), row1, FALSE, FALSE, 0);

        GtkWidget *row2 = ROW();
        app->stashNameCombo = makeCombo();
        comboSetPlaceholder(app->stashNameCombo, "stash name (blank=latest)");
        app->stashPopBtn    = makeBtn("Pop",   "action");
        app->stashApplyBtn  = makeBtn("Apply", NULL);
        app->stashDropBtn   = makeBtn("Drop",  "danger");
        PACKX(row2, app->stashNameCombo);
        PACK(row2, app->stashPopBtn);
        PACK(row2, app->stashApplyBtn);
        PACK(row2, app->stashDropBtn);
        gtk_box_pack_start(GTK_BOX(mgmtTab), row2, FALSE, FALSE, 0);
        g_signal_connect(app->stashPushBtn,  "clicked", G_CALLBACK(onStashPushClicked),  app);
        g_signal_connect(app->stashListBtn,  "clicked", G_CALLBACK(onStashListClicked),  app);
        g_signal_connect(app->stashPopBtn,   "clicked", G_CALLBACK(onStashPopClicked),   app);
        g_signal_connect(app->stashApplyBtn, "clicked", G_CALLBACK(onStashApplyClicked), app);
        g_signal_connect(app->stashDropBtn,  "clicked", G_CALLBACK(onStashDropClicked),  app);
    }
    gtk_box_pack_start(GTK_BOX(mgmtTab), makeSep(), FALSE, FALSE, 2);
    /* Archive */
    {
        GtkWidget *row1 = ROW();
        app->archiveKeepEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->archiveKeepEntry), "keep N commits (blank=1)");
        gtk_entry_set_width_chars(GTK_ENTRY(app->archiveKeepEntry), 18);
        app->archiveBtn     = makeBtn("Archive",     "action");
        app->archiveAllBtn  = makeBtn("Archive All", "danger");
        app->archiveListBtn = makeBtn("List",        NULL);
        PACKX(row1, app->archiveKeepEntry);
        PACK(row1, app->archiveBtn);
        PACK(row1, app->archiveAllBtn);
        PACK(row1, app->archiveListBtn);
        gtk_box_pack_start(GTK_BOX(mgmtTab), row1, FALSE, FALSE, 0);

        GtkWidget *row2 = ROW();
        app->archiveRestoreCombo = makeCombo();
        comboSetPlaceholder(app->archiveRestoreCombo, "zip name to restore");
        app->archiveRestoreBtn = makeBtn("Restore", "success");
        PACKX(row2, app->archiveRestoreCombo);
        PACK(row2, app->archiveRestoreBtn);
        gtk_box_pack_start(GTK_BOX(mgmtTab), row2, FALSE, FALSE, 0);
        g_signal_connect(app->archiveBtn,         "clicked", G_CALLBACK(onArchiveClicked),        app);
        g_signal_connect(app->archiveAllBtn,       "clicked", G_CALLBACK(onArchiveAllClicked),     app);
        g_signal_connect(app->archiveListBtn,      "clicked", G_CALLBACK(onArchiveListClicked),    app);
        g_signal_connect(app->archiveRestoreBtn,   "clicked", G_CALLBACK(onArchiveRestoreClicked), app);
    }

    /* ---- Tab 4: Info (Show / History) ---- */
    MAKE_TAB(infoTab, "Info")
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *lbl = gtk_label_new("Commit/Tag:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        GtkWidget *showEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(showEntry),
                                       "tag or commit.zip  (blank = latest)");
        /* Store reference so onShowCommitClicked and onListCommitFilesClicked
           can read from this entry when called from the Info tab buttons.
           The primary checkoutCombo on the Files tab remains the default source. */
        g_object_set_data(G_OBJECT(app->window), "info-show-entry", showEntry);

        GtkWidget *showBtn2 = makeBtn("Show",       NULL);
        GtkWidget *listBtn2 = makeBtn("List Files", NULL);
        g_signal_connect(showBtn2, "clicked", G_CALLBACK(onShowCommitClicked),      app);
        g_signal_connect(listBtn2, "clicked", G_CALLBACK(onListCommitFilesClicked), app);
        gtk_box_pack_start(GTK_BOX(row), lbl,       FALSE, FALSE, 0);
        PACKX(row, showEntry);
        gtk_box_pack_start(GTK_BOX(row), showBtn2,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), listBtn2,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(infoTab), row, FALSE, FALSE, 0);
    }

#undef MAKE_TAB
#undef ROW
#undef PACK
#undef PACKX

    /* Output view */
    GtkWidget *outScroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outScroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(rightBox), outScroll, TRUE, TRUE, 3);

    app->outputView = gtk_text_view_new();
    gtk_widget_set_name(app->outputView, "outputView");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->outputView), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->outputView), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->outputView), GTK_WRAP_CHAR);
    /* Force monospace regardless of CSS theme or platform font fallback */
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->outputView), TRUE);

    /* Attach a dedicated font provider to this widget's style context.
     * applyConfig() reloads it whenever the user changes font preferences.
     * Using APPLICATION+1 priority ensures it wins over the screen-wide theme. */
    app->outputFontProvider = gtk_css_provider_new();
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(app->outputView),
        GTK_STYLE_PROVIDER(app->outputFontProvider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    gtk_container_add(GTK_CONTAINER(outScroll), app->outputView);
    app->outputBuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->outputView));

    gtk_text_buffer_create_tag(app->outputBuf, TAG_NORMAL,   "foreground","#cdd6f4", NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_SUCCESS,  "foreground","#a6e3a1", NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_WARNING,  "foreground","#f9e2af", NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_ERROR,    "foreground","#f38ba8","weight",PANGO_WEIGHT_BOLD,NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_INFO,     "foreground","#89dceb", NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_HEADING,  "foreground","#89b4fa","weight",PANGO_WEIGHT_BOLD,NULL);
    gtk_text_buffer_create_tag(app->outputBuf, TAG_CMD,      "foreground","#b4befe","weight",PANGO_WEIGHT_BOLD,NULL);

    /* Status bar */
    app->statusBar = gtk_statusbar_new();
    app->statusCtx = gtk_statusbar_get_context_id(GTK_STATUSBAR(app->statusBar), "main");
    gtk_box_pack_start(GTK_BOX(outerVbox), app->statusBar, FALSE, FALSE, 0);

    return app->window;
}

/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
