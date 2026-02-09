#pragma once

#include <adwaita.h>
#include "clipium-store.h"
#include "clipium-db.h"
#include "clipium-ipc.h"
#include "clipium-watcher.h"
#include "clipium-paster.h"

G_BEGIN_DECLS

#define CLIPIUM_TYPE_APP (clipium_app_get_type())
G_DECLARE_FINAL_TYPE(ClipiumApp, clipium_app, CLIPIUM, APP, AdwApplication)

ClipiumApp    *clipium_app_new     (void);
ClipiumStore  *clipium_app_get_store(ClipiumApp *app);
ClipiumDb     *clipium_app_get_db  (ClipiumApp *app);
ClipiumPaster *clipium_app_get_paster(ClipiumApp *app);

G_END_DECLS
