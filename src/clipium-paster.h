#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _ClipiumPaster ClipiumPaster;

ClipiumPaster *clipium_paster_new     (void);
void           clipium_paster_free    (ClipiumPaster *paster);
void           clipium_paster_ctrl_v  (ClipiumPaster *paster);

G_END_DECLS
