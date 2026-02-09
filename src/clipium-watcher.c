#include "clipium-watcher.h"
#include "clipium-config.h"
#include <string.h>

/* We spawn two wl-paste --watch processes:
 *   1. clipboard (regular Ctrl+C / wl-copy)
 *   2. primary   (mouse selection)
 * Both invoke `clipium _ingest <selection>` which reads stdin and sends to daemon.
 * No --type filter so we capture all MIME types. */

typedef struct {
    GSubprocess     *proc;
    ClipiumWatcher  *watcher;
    const char      *selection;  /* "clipboard" or "primary" */
} WatcherProc;

struct _ClipiumWatcher {
    ClipiumStore  *store;
    ClipiumDb     *db;
    WatcherProc    clipboard;
    WatcherProc    primary;
    GCancellable  *cancellable;
    gboolean       running;
};

static void watcher_spawn_one(WatcherProc *wp);

static gboolean
watcher_respawn_timeout(gpointer user_data)
{
    WatcherProc *wp = user_data;
    watcher_spawn_one(wp);
    return G_SOURCE_REMOVE;
}

static void
on_proc_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
    WatcherProc *wp = user_data;
    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &err);
    g_clear_error(&err);

    if (wp->watcher->running) {
        g_message("Watcher (%s) exited, restarting in %dms",
                  wp->selection, CLIPIUM_WATCHER_RESTART_MS);
        g_timeout_add(CLIPIUM_WATCHER_RESTART_MS, watcher_respawn_timeout, wp);
    }
}

static void
watcher_spawn_one(WatcherProc *wp)
{
    ClipiumWatcher *w = wp->watcher;
    if (!w->running) return;

    g_clear_object(&wp->proc);

    GError *err = NULL;

    /* Find our own binary path for the _ingest callback */
    g_autofree char *self_path = g_file_read_link("/proc/self/exe", NULL);
    if (!self_path)
        self_path = g_find_program_in_path("clipium");
    if (!self_path) {
        g_warning("Cannot find clipium binary for watcher");
        return;
    }

    gboolean is_primary = g_str_equal(wp->selection, "primary");

    if (is_primary) {
        wp->proc = g_subprocess_new(
            G_SUBPROCESS_FLAGS_NONE,
            &err,
            "wl-paste", "--primary", "--watch",
            self_path, "_ingest", "primary",
            NULL);
    } else {
        wp->proc = g_subprocess_new(
            G_SUBPROCESS_FLAGS_NONE,
            &err,
            "wl-paste", "--watch",
            self_path, "_ingest", "clipboard",
            NULL);
    }

    if (!wp->proc) {
        g_warning("Failed to spawn watcher (%s): %s", wp->selection, err->message);
        g_error_free(err);
        if (w->running)
            g_timeout_add(CLIPIUM_WATCHER_RESTART_MS, watcher_respawn_timeout, wp);
        return;
    }

    g_message("Watcher started (%s)", wp->selection);
    g_subprocess_wait_async(wp->proc, w->cancellable, on_proc_wait, wp);
}

ClipiumWatcher *
clipium_watcher_start(ClipiumStore *store, ClipiumDb *db)
{
    ClipiumWatcher *w = g_new0(ClipiumWatcher, 1);
    w->store = store;
    w->db = db;
    w->cancellable = g_cancellable_new();
    w->running = TRUE;

    w->clipboard.watcher = w;
    w->clipboard.selection = "clipboard";
    w->primary.watcher = w;
    w->primary.selection = "primary";

    watcher_spawn_one(&w->clipboard);
    watcher_spawn_one(&w->primary);
    return w;
}

static void
watcher_stop_one(WatcherProc *wp)
{
    if (wp->proc) {
        g_subprocess_force_exit(wp->proc);
        g_subprocess_wait(wp->proc, NULL, NULL);
    }
    g_clear_object(&wp->proc);
}

void
clipium_watcher_stop(ClipiumWatcher *w)
{
    if (!w) return;
    w->running = FALSE;
    g_cancellable_cancel(w->cancellable);
    watcher_stop_one(&w->clipboard);
    watcher_stop_one(&w->primary);
    g_clear_object(&w->cancellable);
    g_free(w);
}
