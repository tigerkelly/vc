#include "vcg.h"
#define VCG_FILE_VER_vcgOutput 1

void appendOutput(VcG *app, const gchar *text, const gchar *tagName)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->outputBuf, &end);
    if (tagName)
        gtk_text_buffer_insert_with_tags_by_name(app->outputBuf, &end, text, -1, tagName, NULL);
    else
        gtk_text_buffer_insert(app->outputBuf, &end, text, -1);

    gtk_text_buffer_get_end_iter(app->outputBuf, &end);
    GtkTextMark *mark = gtk_text_buffer_get_mark(app->outputBuf, "insert");
    gtk_text_buffer_place_cursor(app->outputBuf, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app->outputView), mark);
}

void setStatus(VcG *app, const gchar *msg)
{
    gtk_statusbar_pop(GTK_STATUSBAR(app->statusBar), app->statusCtx);
    gtk_statusbar_push(GTK_STATUSBAR(app->statusBar), app->statusCtx, msg);
}

void clearOutput(VcG *app)
{
    gtk_text_buffer_set_text(app->outputBuf, "", 0);
}

const gchar *pickTag(const gchar *line)
{
    if (strstr(line, "ERROR") || strstr(line, "error") || strstr(line, "FAIL"))
        return TAG_ERROR;
    if (strstr(line, "WARN")  || strstr(line, "warn")  || strstr(line, "missing"))
        return TAG_WARNING;
    if (strstr(line, "staged")|| strstr(line, "Committed") || strstr(line, "OK"))
        return TAG_SUCCESS;
    if (strstr(line, "Branch")|| strstr(line, "Project")   || strstr(line, "---"))
        return TAG_HEADING;
    if (strstr(line, "modified") || strstr(line, "untracked"))
        return TAG_WARNING;
    return TAG_NORMAL;
}

/* -----------------------------------------------------------------------
 * Async child-process I/O
 * --------------------------------------------------------------------- */
static void drainChannel(VcG *app, GIOChannel *chan, const gchar *fixedTag)
{
    gchar    *line = NULL;
    gsize     len  = 0;
    GIOStatus st;
    while ((st = g_io_channel_read_line(chan, &line, &len, NULL, NULL))
            == G_IO_STATUS_NORMAL) {
        if (line) {
            appendOutput(app, line, fixedTag ? fixedTag : pickTag(line));
            g_free(line);
            line = NULL;
        }
    }
}

static gboolean onChildData(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VcG *app = (VcG *)data;
    (void)chan;
    if ((cond & G_IO_IN) && app->ioChan)
        drainChannel(app, app->ioChan, NULL);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        app->ioWatchId = 0;
        return FALSE;
    }
    return TRUE;
}

static gboolean onChildErrData(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VcG *app = (VcG *)data;
    (void)chan;
    if ((cond & G_IO_IN) && app->ioChanErr)
        drainChannel(app, app->ioChanErr, TAG_ERROR);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        app->ioErrWatchId = 0;
        return FALSE;
    }
    return TRUE;
}

static void releaseChannel(VcG *app, GIOChannel **chanPtr,
                            guint *watchIdPtr, const gchar *fixedTag)
{
    if (!*chanPtr) return;
    if (*watchIdPtr) { g_source_remove(*watchIdPtr); *watchIdPtr = 0; }
    drainChannel(app, *chanPtr, fixedTag);
    g_io_channel_shutdown(*chanPtr, FALSE, NULL);
    g_io_channel_unref(*chanPtr);
    *chanPtr = NULL;
}

static void onChildExit(GPid pid, gint status, gpointer data)
{
    VcG *app = (VcG *)data;
    releaseChannel(app, &app->ioChan,    &app->ioWatchId,    NULL);
    releaseChannel(app, &app->ioChanErr, &app->ioErrWatchId, TAG_ERROR);
    appendOutput(app, "\n", TAG_NORMAL);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        setStatus(app, "Command completed successfully.");
        appendOutput(app, "✔ Done.\n", TAG_SUCCESS);
    } else {
        setStatus(app, "Command exited with errors.");
        appendOutput(app, "✘ Exited with errors.\n", TAG_ERROR);
    }
    g_spawn_close_pid(pid);
    app->childPid = 0;
    /* Refresh tree after every command so branches/tags/commits update */
    refreshTree(app);
}

/* -----------------------------------------------------------------------
 * Run a vc sub-command asynchronously
 * --------------------------------------------------------------------- */
void syncWorkDir(VcG *app)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app->dirEntry));
    g_free(app->workDir);
    app->workDir = g_strdup(text && text[0] ? text : g_get_home_dir());
}

void runVcCmd(VcG *app, gchar **argv)
{
    if (app->childPid != 0) {
        appendOutput(app, "A command is already running.\n", TAG_WARNING);
        return;
    }
    syncWorkDir(app);
    if (!app->workDir || app->workDir[0] == '\0') {
        appendOutput(app, "ERROR: No working directory set.\n", TAG_ERROR);
        return;
    }

    /* Override argv[0] with the configured binary path */
    argv[0] = app->cfg.vcBinary;

    GString *cmdLine = g_string_new("$ ");
    for (int i = 0; argv[i]; i++) {
        if (i) g_string_append_c(cmdLine, ' ');
        g_string_append(cmdLine, argv[i]);
    }
    g_string_append_c(cmdLine, '\n');
    appendOutput(app, cmdLine->str, TAG_CMD);
    g_string_free(cmdLine, TRUE);

    GError   *err = NULL;
    gboolean  ok  = g_spawn_async_with_pipes(
        app->workDir, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &app->childPid, NULL,
        &app->childStdout, &app->childStderr, &err);

    if (!ok) {
        gchar *msg = g_strdup_printf("Failed to launch vc: %s\n",
                                     err ? err->message : "unknown");
        appendOutput(app, msg, TAG_ERROR);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    app->ioChan = g_io_channel_unix_new(app->childStdout);
    g_io_channel_set_flags(app->ioChan, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding(app->ioChan, NULL, NULL);
    app->ioWatchId = g_io_add_watch(app->ioChan,
        G_IO_IN | G_IO_HUP | G_IO_ERR, onChildData, app);

    app->ioChanErr = g_io_channel_unix_new(app->childStderr);
    g_io_channel_set_flags(app->ioChanErr, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding(app->ioChanErr, NULL, NULL);
    app->ioErrWatchId = g_io_add_watch(app->ioChanErr,
        G_IO_IN | G_IO_HUP | G_IO_ERR, onChildErrData, app);

    g_child_watch_add(app->childPid, onChildExit, app);
    setStatus(app, "Running…");
}

#define RUN1(app, sub) \
    do { gchar *_av[] = {VC_CMD, (sub), NULL}; runVcCmd((app), _av); } while(0)
