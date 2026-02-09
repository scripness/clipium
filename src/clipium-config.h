#pragma once

#include <glib.h>

#define CLIPIUM_APP_ID       "io.github.clipium"
#define CLIPIUM_VERSION      "0.1.0"
#define CLIPIUM_MAX_ENTRIES  1000
#define CLIPIUM_PREVIEW_LEN  100
#define CLIPIUM_CARD_WIDTH   450
#define CLIPIUM_CARD_HEIGHT  500
#define CLIPIUM_DB_FILENAME  "clipium.db"
#define CLIPIUM_SOCK_NAME    "clipium.sock"

/* IPC buffer sizes */
#define CLIPIUM_IPC_MAX_MSG  (16 * 1024 * 1024)  /* 16 MB max message */
#define CLIPIUM_IPC_HDR_SIZE 4                     /* 4-byte big-endian length */

/* Paste timing */
#define CLIPIUM_PASTE_DELAY_MS  50
#define CLIPIUM_KEY_DELAY_MS     5

/* Watcher restart delay */
#define CLIPIUM_WATCHER_RESTART_MS 1000

/* Evdev keycodes for Ctrl+V */
#define CLIPIUM_KEY_LEFTCTRL  29
#define CLIPIUM_KEY_V         47

/* Helper to get XDG paths */
static inline const char *
clipium_runtime_dir(void)
{
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    return dir ? dir : "/tmp";
}

static inline char *
clipium_socket_path(void)
{
    return g_build_filename(clipium_runtime_dir(), CLIPIUM_SOCK_NAME, NULL);
}

static inline char *
clipium_db_path(void)
{
    g_autofree char *data_dir = g_build_filename(
        g_get_user_data_dir(), "clipium", NULL);
    g_mkdir_with_parents(data_dir, 0700);
    return g_build_filename(data_dir, CLIPIUM_DB_FILENAME, NULL);
}
