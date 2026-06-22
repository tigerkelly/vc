#include "vcg.h"
#define VCG_FILE_VER_vcgPrefs 1

static const gchar *dialogLightCss =
    "dialog, dialog * { color: #1a1a2e; }"
    "dialog entry, dialog spinbutton entry {"
    "  background-color: #f0f0f8; color: #1a1a2e;"
    "  border: 1px solid #8888aa; border-radius: 4px; padding: 3px 6px; }"
    "dialog button {"
    "  background: #ddddf0; color: #1a1a2e;"
    "  border: 1px solid #8888aa; border-radius: 4px; padding: 4px 10px; }"
    "dialog button:hover { background: #c8c8e8; }"
    "dialog notebook tab {"
    "  background: #d0d0e8; padding: 4px 10px; color: #1a1a2e; }"
    "dialog notebook tab:checked { background: #b0b0d8; font-weight: bold; }"
    "dialog label { color: #1a1a2e; }"
    "dialog checkbutton label { color: #1a1a2e; }"
    "dialog .section { color: #1a1a2e; }";

/* Create a light CSS provider, register it screen-wide, return it.
 * Caller must call dialogLightCssRemove() and g_object_unref() when done. */
GtkCssProvider *dialogLightCssAdd(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, dialogLightCss, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    return p;
}

void dialogLightCssRemove(GtkCssProvider *p)
{
    gtk_style_context_remove_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p));
    g_object_unref(p);
}


/* -----------------------------------------------------------------------
 * Apply config: rebuild screen-wide CSS and update the output-view font
 * via a dedicated per-widget CSS provider (no deprecated API needed).
 * --------------------------------------------------------------------- */
void applyConfig(VcG *app)
{
    /* Push the main theme to the whole screen */
    gchar *css = configBuildCss(&app->cfg);
    gtk_css_provider_load_from_data(app->cssProvider, css, -1, NULL);
    g_free(css);

    /* Update the output-view font.
     * gtk_widget_override_font() is deprecated in GTK 3.16+.
     * The correct replacement is a CSS provider attached to the widget's
     * own GtkStyleContext.  The `font` shorthand accepts any Pango
     * description string, e.g. "monospace 12" or "Consolas 14". */
    if (app->outputView && app->outputFontProvider) {
        const gchar *family = app->cfg.outputFontFamily[0]
                              ? app->cfg.outputFontFamily : "monospace";
        gint size = app->cfg.outputFontSize > 0
                    ? app->cfg.outputFontSize : 12;

        /* Use `pt` units so the size matches what Preferences shows */
        gchar *fontCss = g_strdup_printf(
            "textview#outputView text { font-family: %s; font-size: %dpt; }",
            family, size);
        gtk_css_provider_load_from_data(app->outputFontProvider,
                                        fontCss, -1, NULL);
        g_free(fontCss);
    }
}

/* -----------------------------------------------------------------------
 * Save window geometry into cfg (called on window-delete-event)
 * --------------------------------------------------------------------- */
gboolean onWindowDelete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)ev;
    VcG *app = (VcG *)data;
    gtk_window_get_size(GTK_WINDOW(w), &app->cfg.winWidth, &app->cfg.winHeight);
    gtk_window_get_position(GTK_WINDOW(w), &app->cfg.winX, &app->cfg.winY);
    app->cfg.pane1Pos = gtk_paned_get_position(GTK_PANED(app->hpaned1));
    app->cfg.pane2Pos = gtk_paned_get_position(GTK_PANED(app->hpaned2));
    /* Save last working directory */
    if (app->workDir && app->workDir[0])
        g_strlcpy(app->cfg.lastWorkDir, app->workDir,
                  sizeof(app->cfg.lastWorkDir));
    configSave(&app->cfg);
    return FALSE;   /* let the default destroy handler run */
}

/* Preferences dialog helpers */

void hexToRgba(const gchar *hex, GdkRGBA *rgba)
{
    gchar buf[8];
    snprintf(buf, sizeof(buf), "#%s", hex);
    if (!gdk_rgba_parse(rgba, buf)) {
        rgba->red = rgba->green = rgba->blue = 0.5;
        rgba->alpha = 1.0;
    }
}

/* GdkRGBA -> hex string (6 chars + NUL) into caller's buffer */
void rgbaToHex(const GdkRGBA *rgba, gchar *buf, gsize sz)
{
    guint r = (guint)(CLAMP(rgba->red,   0.0, 1.0) * 255.0 + 0.5);
    guint g = (guint)(CLAMP(rgba->green, 0.0, 1.0) * 255.0 + 0.5);
    guint b = (guint)(CLAMP(rgba->blue,  0.0, 1.0) * 255.0 + 0.5);
    snprintf(buf, sz, "%02x%02x%02x", r, g, b);
}

/* -----------------------------------------------------------------------
 * Preferences dialog
 * --------------------------------------------------------------------- */
void showPrefsDialog(VcG *app)
{
    GtkCssProvider *lightCss = dialogLightCssAdd();

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Preferences", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply",  GTK_RESPONSE_APPLY,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 500, 480);

    GtkWidget *nb      = gtk_notebook_new();
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_pack_start(GTK_BOX(content), nb, TRUE, TRUE, 0);

#define MAKE_PAGE(NAME, TITLE)                                              \
    GtkWidget *_grid_##NAME = gtk_grid_new();                               \
    gtk_grid_set_row_spacing(GTK_GRID(_grid_##NAME), 9);                    \
    gtk_grid_set_column_spacing(GTK_GRID(_grid_##NAME), 14);                \
    gtk_container_set_border_width(GTK_CONTAINER(_grid_##NAME), 14);        \
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), _grid_##NAME,                \
                             gtk_label_new(TITLE));                         \
    gint _row_##NAME = 0;

#define ADD_ROW(NAME, LABEL_STR, WIDGET)                                        \
    do {                                                                        \
        GtkWidget *_lbl = gtk_label_new(LABEL_STR);                             \
        gtk_widget_set_halign(_lbl, GTK_ALIGN_END);                             \
        gtk_widget_set_valign(_lbl, GTK_ALIGN_CENTER);                          \
        gtk_grid_attach(GTK_GRID(_grid_##NAME), _lbl,     0, _row_##NAME, 1, 1);\
        gtk_grid_attach(GTK_GRID(_grid_##NAME), (WIDGET), 1, _row_##NAME, 1, 1);\
        gtk_widget_set_hexpand((WIDGET), TRUE);                                 \
        _row_##NAME++;                                                           \
    } while(0)

    /* ---- Tab: Window ---- */
    MAKE_PAGE(win, "Window")

    GtkWidget *spnWinW  = gtk_spin_button_new_with_range(400, 7680, 1);
    GtkWidget *spnWinH  = gtk_spin_button_new_with_range(300, 4320, 1);
    GtkWidget *spnPane1 = gtk_spin_button_new_with_range(100, 1000, 1);
    GtkWidget *spnPane2 = gtk_spin_button_new_with_range(100, 1000, 1);

    /* Read live values from the running window, not stale cfg */
    gint curW = 0, curH = 0;
    gtk_window_get_size(GTK_WINDOW(app->window), &curW, &curH);
    gint curP1 = gtk_paned_get_position(GTK_PANED(app->hpaned1));
    gint curP2 = gtk_paned_get_position(GTK_PANED(app->hpaned2));

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnWinW),  (gdouble)curW);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnWinH),  (gdouble)curH);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnPane1), (gdouble)curP1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnPane2), (gdouble)curP2);

    ADD_ROW(win, "Window width:",     spnWinW);
    ADD_ROW(win, "Window height:",    spnWinH);
    ADD_ROW(win, "Sidebar width:",    spnPane1);
    ADD_ROW(win, "Tree panel width:", spnPane2);

    /* ---- Tab: Fonts ---- */
    MAKE_PAGE(fnt, "Fonts")

    GtkWidget *spnUiPt   = gtk_spin_button_new_with_range(7, 32, 1);
    GtkWidget *spnTreePt = gtk_spin_button_new_with_range(7, 32, 1);
    GtkWidget *spnOutPt  = gtk_spin_button_new_with_range(7, 32, 1);
    GtkWidget *entFamily = gtk_entry_new();

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnUiPt),   (gdouble)app->cfg.uiFontSize);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnTreePt),  (gdouble)app->cfg.treeFontSize);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spnOutPt),   (gdouble)app->cfg.outputFontSize);
    gtk_entry_set_text(GTK_ENTRY(entFamily), app->cfg.outputFontFamily);

    ADD_ROW(fnt, "UI font size (pt):",     spnUiPt);
    ADD_ROW(fnt, "Tree font size (pt):",   spnTreePt);
    ADD_ROW(fnt, "Output font size (pt):", spnOutPt);
    ADD_ROW(fnt, "Output font family:",    entFamily);

    /* ---- Tab: Colours ---- */
    MAKE_PAGE(col, "Colours")

    typedef struct { const gchar *lbl; gchar *dst; gsize dstsz; GtkWidget *btn; } ColRow;
    ColRow colRows[] = {
        {"Background:",      app->cfg.colBg,      sizeof(app->cfg.colBg),      NULL},
        {"Dark background:", app->cfg.colBgDark,  sizeof(app->cfg.colBgDark),  NULL},
        {"Foreground:",      app->cfg.colFg,      sizeof(app->cfg.colFg),      NULL},
        {"Accent:",          app->cfg.colAccent,  sizeof(app->cfg.colAccent),  NULL},
        {"Success text:",    app->cfg.colSuccess, sizeof(app->cfg.colSuccess), NULL},
        {"Warning text:",    app->cfg.colWarning, sizeof(app->cfg.colWarning), NULL},
        {"Error text:",      app->cfg.colError,   sizeof(app->cfg.colError),   NULL},
    };
    const int nColRows = (int)(sizeof(colRows) / sizeof(colRows[0]));

    for (int i = 0; i < nColRows; i++) {
        GdkRGBA rgba;
        hexToRgba(colRows[i].dst, &rgba);
        colRows[i].btn = gtk_color_button_new_with_rgba(&rgba);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(colRows[i].btn), FALSE);
        gtk_color_button_set_title(GTK_COLOR_BUTTON(colRows[i].btn), colRows[i].lbl);
        ADD_ROW(col, colRows[i].lbl, colRows[i].btn);
    }

    /* ---- Tab: Behaviour ---- */
    MAKE_PAGE(beh, "Behaviour")

    GtkWidget *entVc      = gtk_entry_new();
    GtkWidget *chkRefresh = gtk_check_button_new_with_label("Auto-refresh tree after commands");
    gtk_entry_set_text(GTK_ENTRY(entVc), app->cfg.vcBinary);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chkRefresh),
                                 app->cfg.autoRefreshTree != 0);
    ADD_ROW(beh, "vc binary path:", entVc);
    gtk_grid_attach(GTK_GRID(_grid_beh), chkRefresh, 0, _row_beh, 2, 1);

#undef MAKE_PAGE
#undef ADD_ROW

    gtk_widget_show_all(dlg);

    /* ---- Event loop ---- */
    gint resp;
    while ((resp = gtk_dialog_run(GTK_DIALOG(dlg))) == GTK_RESPONSE_APPLY
           || resp == GTK_RESPONSE_OK) {

        app->cfg.winWidth  = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnWinW));
        app->cfg.winHeight = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnWinH));
        app->cfg.pane1Pos  = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnPane1));
        app->cfg.pane2Pos  = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnPane2));

        app->cfg.uiFontSize     = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnUiPt));
        app->cfg.treeFontSize   = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnTreePt));
        app->cfg.outputFontSize = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spnOutPt));
        g_strlcpy(app->cfg.outputFontFamily,
                  gtk_entry_get_text(GTK_ENTRY(entFamily)),
                  sizeof(app->cfg.outputFontFamily));

        for (int i = 0; i < nColRows; i++) {
            GdkRGBA rgba;
            gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(colRows[i].btn), &rgba);
            rgbaToHex(&rgba, colRows[i].dst, colRows[i].dstsz);
        }

        g_strlcpy(app->cfg.vcBinary,
                  gtk_entry_get_text(GTK_ENTRY(entVc)),
                  sizeof(app->cfg.vcBinary));
        app->cfg.autoRefreshTree =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chkRefresh)) ? 1 : 0;

        gtk_paned_set_position(GTK_PANED(app->hpaned1), app->cfg.pane1Pos);
        gtk_paned_set_position(GTK_PANED(app->hpaned2), app->cfg.pane2Pos);
        gtk_window_resize(GTK_WINDOW(app->window),
                          app->cfg.winWidth, app->cfg.winHeight);
        applyConfig(app);
        configSave(&app->cfg);

        if (resp == GTK_RESPONSE_OK) break;
    }

    gtk_widget_destroy(dlg);
    dialogLightCssRemove(lightCss);
}
