#pragma once

#include <glib.h>
#include <gio/gio.h>
#include "clipium-store.h"
#include "clipium-db.h"

G_BEGIN_DECLS

typedef struct _ClipiumWatcher ClipiumWatcher;

ClipiumWatcher *clipium_watcher_start (ClipiumStore *store, ClipiumDb *db);
void            clipium_watcher_stop  (ClipiumWatcher *watcher);

G_END_DECLS
