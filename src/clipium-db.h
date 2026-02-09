#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include "clipium-store.h"

G_BEGIN_DECLS

typedef struct {
    sqlite3 *db;
    char    *path;
    GMutex   lock;
} ClipiumDb;

ClipiumDb *clipium_db_open     (const char *path);
void       clipium_db_close    (ClipiumDb *db);
gboolean   clipium_db_init     (ClipiumDb *db);
gboolean   clipium_db_load_all (ClipiumDb *db, ClipiumStore *store);
void       clipium_db_save     (ClipiumDb *db, const ClipiumEntry *entry);
void       clipium_db_save_async(ClipiumDb *db, const ClipiumEntry *entry);
gboolean   clipium_db_delete   (ClipiumDb *db, guint64 id);
gboolean   clipium_db_clear    (ClipiumDb *db);
gboolean   clipium_db_update_pin(ClipiumDb *db, guint64 id, gboolean pinned);

G_END_DECLS
