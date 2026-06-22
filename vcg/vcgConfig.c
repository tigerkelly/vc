#include "vcg.h"
#define VCG_FILE_VER_vcgConfig 1

gchar *configFilePath(void)
{
    return g_build_filename(g_get_user_config_dir(), "vcg", "vcg.conf", NULL);
}

/* ---- Apply hard-coded defaults ---- */
void configDefaults(VcConfig *cfg)
{
    cfg->winWidth        = 1400;
    cfg->winHeight       = 800;
    cfg->pane1Pos        = 185;
    cfg->pane2Pos        = 310;
    cfg->winX            = -1;
    cfg->winY            = -1;

    cfg->uiFontSize      = 12;
    cfg->treeFontSize    = 12;
    cfg->outputFontSize  = 12;
    g_strlcpy(cfg->outputFontFamily, "monospace", sizeof(cfg->outputFontFamily));

    g_strlcpy(cfg->colBg,      "1e1e2e", sizeof(cfg->colBg));
    g_strlcpy(cfg->colBgDark,  "11111b", sizeof(cfg->colBgDark));
    g_strlcpy(cfg->colFg,      "cdd6f4", sizeof(cfg->colFg));
    g_strlcpy(cfg->colAccent,  "89b4fa", sizeof(cfg->colAccent));
    g_strlcpy(cfg->colSuccess, "a6e3a1", sizeof(cfg->colSuccess));
    g_strlcpy(cfg->colWarning, "f9e2af", sizeof(cfg->colWarning));
    g_strlcpy(cfg->colError,   "f38ba8", sizeof(cfg->colError));

    g_strlcpy(cfg->vcBinary,      "vc",           sizeof(cfg->vcBinary));
    cfg->lastWorkDir[0]  = '\0';
    cfg->autoRefreshTree = 1;
}

/* ---- Load from disk (missing keys keep defaults) ---- */
void configLoad(VcConfig *cfg)
{
    configDefaults(cfg);

    gchar *path = configFilePath();
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_free(path);
        return;
    }
    g_free(path);

#define KI(g,k,f) { GError *e=NULL; gint v=g_key_file_get_integer(kf,g,k,&e); if(!e) cfg->f=v; g_clear_error(&e); }
#define KS(g,k,f) { GError *e=NULL; gchar *v=g_key_file_get_string(kf,g,k,&e); \
                    if(!e&&v){ g_strlcpy(cfg->f,v,sizeof(cfg->f)); g_free(v); } g_clear_error(&e); }

    KI("window","width",      winWidth)
    KI("window","height",     winHeight)
    KI("window","pane1",      pane1Pos)
    KI("window","pane2",      pane2Pos)
    KI("window","x",          winX)
    KI("window","y",          winY)

    KI("fonts","ui_size",     uiFontSize)
    KI("fonts","tree_size",   treeFontSize)
    KI("fonts","output_size", outputFontSize)
    KS("fonts","output_family", outputFontFamily)

    KS("colours","bg",        colBg)
    KS("colours","bg_dark",   colBgDark)
    KS("colours","fg",        colFg)
    KS("colours","accent",    colAccent)
    KS("colours","success",   colSuccess)
    KS("colours","warning",   colWarning)
    KS("colours","error",     colError)

    KS("behaviour","vc_binary",       vcBinary)
    KS("behaviour","last_work_dir",   lastWorkDir)
    KI("behaviour","auto_refresh_tree", autoRefreshTree)

#undef KI
#undef KS
    g_key_file_free(kf);
}

/* ---- Save to disk ---- */
void configSave(const VcConfig *cfg)
{
    gchar *path = configFilePath();

    /* Ensure directory exists */
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();

    g_key_file_set_integer(kf,"window","width",      cfg->winWidth);
    g_key_file_set_integer(kf,"window","height",     cfg->winHeight);
    g_key_file_set_integer(kf,"window","pane1",      cfg->pane1Pos);
    g_key_file_set_integer(kf,"window","pane2",      cfg->pane2Pos);
    g_key_file_set_integer(kf,"window","x",          cfg->winX);
    g_key_file_set_integer(kf,"window","y",          cfg->winY);

    g_key_file_set_integer(kf,"fonts","ui_size",     cfg->uiFontSize);
    g_key_file_set_integer(kf,"fonts","tree_size",   cfg->treeFontSize);
    g_key_file_set_integer(kf,"fonts","output_size", cfg->outputFontSize);
    g_key_file_set_string (kf,"fonts","output_family", cfg->outputFontFamily);

    g_key_file_set_string(kf,"colours","bg",        cfg->colBg);
    g_key_file_set_string(kf,"colours","bg_dark",   cfg->colBgDark);
    g_key_file_set_string(kf,"colours","fg",        cfg->colFg);
    g_key_file_set_string(kf,"colours","accent",    cfg->colAccent);
    g_key_file_set_string(kf,"colours","success",   cfg->colSuccess);
    g_key_file_set_string(kf,"colours","warning",   cfg->colWarning);
    g_key_file_set_string(kf,"colours","error",     cfg->colError);

    g_key_file_set_string (kf,"behaviour","vc_binary",          cfg->vcBinary);
    g_key_file_set_string (kf,"behaviour","last_work_dir",      cfg->lastWorkDir);
    g_key_file_set_integer(kf,"behaviour","auto_refresh_tree",  cfg->autoRefreshTree);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("vcg: could not save config: %s\n",
                   err ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* ---- Build the CSS string from current config ---- */
gchar *configBuildCss(const VcConfig *cfg)
{
    return g_strdup_printf(
        "window { background-color:#%s; color:#%s; }"
        "button { background:linear-gradient(to bottom,#313244,#2a2a3d);"
        "  color:#%s; border:1px solid #45475a; border-radius:5px;"
        "  padding:4px 10px; font-size:%dpt; }"
        "button:hover  { background:#45475a; }"
        "button:active { background:#585b70; }"
        "button.action  { background:linear-gradient(to bottom,#1e6bc4,#1558a8); }"
        "button.action:hover  { background:#1e6bc4; }"
        "button.danger  { background:linear-gradient(to bottom,#a6222a,#891c23); }"
        "button.danger:hover  { background:#a6222a; }"
        "button.success { background:linear-gradient(to bottom,#1e8a3c,#166b2e); }"
        "button.success:hover { background:#1e8a3c; }"
        "entry { background-color:#%s; color:#%s;"
        "  border:1px solid #45475a; border-radius:4px; padding:3px 6px;"
        "  font-size:%dpt; }"
        "entry:focus { border-color:#%s; }"
        "combobox entry { background-color:#%s; color:#%s; font-size:%dpt; }"
        "combobox button { background:#313244; border-radius:0 4px 4px 0; padding:2px 4px; }"
        "combobox button:hover { background:#45475a; }"
        "treeview { background-color:#%s; color:#%s; font-size:%dpt; }"
        "treeview:selected { background-color:#313244; }"
        "treeview header button { background:#1e1e2e; color:#%s;"
        "  border-bottom:1px solid #313244; padding:3px; font-size:%dpt; }"
        "textview text { background-color:#%s; color:#%s;"
        "  font-family:%s; font-size:%dpt; padding:4px; }"
        "textview { background-color:#%s; border:1px solid #313244; border-radius:4px; }"
        "scrolledwindow { border:none; }"
        "frame > border { border:1px solid #313244; border-radius:6px; }"
        "label.section { color:#%s; font-weight:bold; font-size:%dpt; }"
        "notebook tab { background:#313244; color:#cdd6f4; padding:3px 10px; }"
        "notebook tab:checked { background:#45475a; color:#cdd6f4; font-weight:bold; }"
        "notebook header { background:#1e1e2e; border-bottom:1px solid #45475a; }"
        "statusbar { background-color:#%s; color:#a6adc8; font-size:%dpt; padding:2px 8px; }"
        "separator { background-color:#313244; min-height:1px; }"
        "menu { background-color:#%s; color:#%s; }"
        "menuitem:hover { background-color:#313244; }"
        /* Theme the combo popup list rows to match the dark theme.
         * max-height is not valid in GTK3 CSS — row count is limited
         * programmatically in makeCombo() via notify::popup-shown. */
        "window.popup .view { background-color:#1e1e2e; color:#cdd6f4; }"
        "window.popup .view:selected { background-color:#313244; }"
        /* Sidebar file list — rows inherit the dark sidebar background */
        "list { background-color:#181825; }"
        "list row { background-color:#181825; color:#cdd6f4; padding:1px 4px; }"
        "list row:hover { background-color:#313244; }"
        "list row:selected { background-color:#45475a; }"
        "list row label { color:#cdd6f4; font-size:%dpt; }"
        ".file-staged   { color:#%s; }"
        ".file-modified { color:#%s; }",
        /* window */        cfg->colBg, cfg->colFg,
        /* button */        cfg->colFg, cfg->uiFontSize,
        /* entry */         cfg->colBgDark, cfg->colFg, cfg->uiFontSize, cfg->colAccent,
        /* combobox entry */cfg->colBgDark, cfg->colFg, cfg->uiFontSize,
        /* treeview */      cfg->colBgDark, cfg->colFg, cfg->treeFontSize,
        /* tree header */   cfg->colAccent, cfg->treeFontSize,
        /* textview */      cfg->colBgDark, cfg->colFg,
                            cfg->outputFontFamily, cfg->outputFontSize,
                            cfg->colBgDark,
        /* section label */ cfg->colAccent, cfg->uiFontSize,
        /* statusbar */     cfg->colBgDark, cfg->uiFontSize,
        /* menu */          cfg->colBg, cfg->colFg,
        /* file states */   cfg->treeFontSize, cfg->colSuccess, cfg->colWarning
    );
}
