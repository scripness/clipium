#include "clipium-watcher.h"
#include "clipium-config.h"
#include <string.h>

struct _ClipiumWatcher {
    ClipiumStore  *store;
    ClipiumDb     *db;
    GSubprocess   *proc;
    GCancellable  *cancellable;
    gboolean       running;
};

static void watcher_spawn(ClipiumWatcher *w);

static gboolean
watcher_spawn_timeout(gpointer user_data)
{
    ClipiumWatcher *w = user_data;
    watcher_spawn(w);
    return G_SOURCE_REMOVE;
}

static void
on_proc_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClipiumWatcher *w = user_data;
    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &err);
    g_clear_error(&err);

    if (w->running) {
        g_message("Watcher process exited, restarting in %dms", CLIPIUM_WATCHER_RESTART_MS);
        g_timeout_add(CLIPIUM_WATCHER_RESTART_MS, watcher_spawn_timeout, w);
    }
}

static void
watcher_spawn(ClipiumWatcher *w)
{
    if (!w->running) return;

    g_clear_object(&w->proc);

    GError *err = NULL;

    /* Find our own binary path for the _ingest callback */
    g_autofree char *self_path = g_file_read_link("/proc/self/exe", NULL);
    if (!self_path)
        self_path = g_find_program_in_path("clipium");
    if (!self_path) {
        g_warning("Cannot find clipium binary for watcher");
        return;
    }

    w->proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE,
        &err,
        "wl-paste", "--type", "text", "--watch",
        self_path, "_ingest",
        NULL);

    if (!w->proc) {
        g_warning("Failed to spawn watcher: %s", err->message);
        g_error_free(err);
        if (w->running)
            g_timeout_add(CLIPIUM_WATCHER_RESTART_MS, watcher_spawn_timeout, w);
        return;
    }

    g_message("Watcher started (wl-paste --watch)");
    g_subprocess_wait_async(w->proc, w->cancellable, on_proc_wait, w);
}

ClipiumWatcher *
clipium_watcher_start(ClipiumStore *store, ClipiumDb *db)
{
    ClipiumWatcher *w = g_new0(ClipiumWatcher, 1);
    w->store = store;
    w->db = db;
    w->cancellable = g_cancellable_new();
    w->running = TRUE;

    watcher_spawn(w);
    return w;
}

void
clipium_watcher_stop(ClipiumWatcher *w)
{
    if (!w) return;
    w->running = FALSE;
    g_cancellable_cancel(w->cancellable);
    if (w->proc) {
        g_subprocess_force_exit(w->proc);
        /* Wait synchronously so the async callback fires before we free */
        g_subprocess_wait(w->proc, NULL, NULL);
    }
    g_clear_object(&w->proc);
    g_clear_object(&w->cancellable);
    g_free(w);
}
