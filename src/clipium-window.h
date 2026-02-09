#pragma once

#include <adwaita.h>
#include "clipium-store.h"
#include "clipium-db.h"
#include "clipium-paster.h"

G_BEGIN_DECLS

#define CLIPIUM_TYPE_WINDOW (clipium_window_get_type())
G_DECLARE_FINAL_TYPE(ClipiumWindow, clipium_window, CLIPIUM, WINDOW, GtkWindow)

ClipiumWindow *clipium_window_new  (GtkApplication *app,
                                     ClipiumStore   *store,
                                     ClipiumDb      *db,
                                     ClipiumPaster  *paster);
void           clipium_window_show_popup (ClipiumWindow *win);
void           clipium_window_hide_popup (ClipiumWindow *win);

G_END_DECLS
