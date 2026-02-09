#pragma once

#include <glib.h>
#include <gio/gio.h>
#include "clipium-store.h"
#include "clipium-db.h"

G_BEGIN_DECLS

typedef struct _ClipiumIpc ClipiumIpc;

/* Callback when "show" command received via IPC */
typedef void (*ClipiumIpcShowCallback)(gpointer user_data);

ClipiumIpc *clipium_ipc_server_start  (const char             *socket_path,
                                        ClipiumStore           *store,
                                        ClipiumDb              *db,
                                        ClipiumIpcShowCallback  show_cb,
                                        gpointer                show_cb_data);
void        clipium_ipc_server_stop   (ClipiumIpc *ipc);

/* Client helpers (used by CLI modes) */
char       *clipium_ipc_send_command  (const char *socket_path, const char *json_cmd);

G_END_DECLS
