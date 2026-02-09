#include "clipium-app.h"
#include "clipium-window.h"
#include "clipium-config.h"
#include <unistd.h>

struct _ClipiumApp {
    AdwApplication   parent;
    ClipiumStore    *store;
    ClipiumDb       *db;
    ClipiumIpc      *ipc;
    ClipiumWatcher  *watcher;
    ClipiumPaster   *paster;
    ClipiumWindow   *window;
};

G_DEFINE_TYPE(ClipiumApp, clipium_app, ADW_TYPE_APPLICATION)

/* --- IPC "show" callback (called from IPC thread context) --- */

static gboolean
show_window_idle(gpointer user_data)
{
    ClipiumApp *self = CLIPIUM_APP(user_data);
    if (self->window)
        clipium_window_show_popup(self->window);
    return G_SOURCE_REMOVE;
}

static void
on_ipc_show(gpointer user_data)
{
    /* Schedule on main thread */
    g_idle_add(show_window_idle, user_data);
}

/* --- Actions --- */

static void
action_show(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    ClipiumApp *self = CLIPIUM_APP(user_data);
    if (self->window)
        clipium_window_show_popup(self->window);
}

static void
action_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    GApplication *app = G_APPLICATION(user_data);
    g_application_quit(app);
}

static const GActionEntry app_actions[] = {
    { "show", action_show, NULL, NULL, NULL, {0} },
    { "quit", action_quit, NULL, NULL, NULL, {0} },
};

/* --- Application signals --- */

static void
clipium_app_startup(GApplication *app)
{
    G_APPLICATION_CLASS(clipium_app_parent_class)->startup(app);

    ClipiumApp *self = CLIPIUM_APP(app);

    /* Force dark theme */
    adw_style_manager_set_color_scheme(
        adw_style_manager_get_default(),
        ADW_COLOR_SCHEME_FORCE_DARK);

    /* Initialize store */
    self->store = clipium_store_new(CLIPIUM_MAX_ENTRIES);

    /* Open database and load entries */
    g_autofree char *db_path = clipium_db_path();
    self->db = clipium_db_open(db_path);
    if (self->db) {
        clipium_db_init(self->db);
        clipium_db_load_all(self->db, self->store);
    }

    /* Start IPC server */
    g_autofree char *sock_path = clipium_socket_path();
    self->ipc = clipium_ipc_server_start(sock_path, self->store, self->db,
                                          on_ipc_show, self);

    /* Start clipboard watcher */
    self->watcher = clipium_watcher_start(self->store, self->db);

    /* Initialize paster */
    self->paster = clipium_paster_new();

    /* Register actions */
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
                                    G_N_ELEMENTS(app_actions), self);

    g_message("Clipium daemon started (pid %d)", getpid());
}

static void
on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    ClipiumApp *self = CLIPIUM_APP(user_data);
    self->window = NULL;
}

static void
clipium_app_activate(GApplication *app)
{
    ClipiumApp *self = CLIPIUM_APP(app);

    if (!self->window) {
        self->window = clipium_window_new(GTK_APPLICATION(app),
                                           self->store, self->db, self->paster);
        g_signal_connect(self->window, "destroy", G_CALLBACK(on_window_destroy), self);
    }

    clipium_window_show_popup(self->window);
}

static void
clipium_app_shutdown(GApplication *app)
{
    ClipiumApp *self = CLIPIUM_APP(app);

    clipium_watcher_stop(self->watcher);
    clipium_ipc_server_stop(self->ipc);
    clipium_paster_free(self->paster);
    clipium_db_close(self->db);
    clipium_store_free(self->store);

    self->watcher = NULL;
    self->ipc = NULL;
    self->paster = NULL;
    self->db = NULL;
    self->store = NULL;

    G_APPLICATION_CLASS(clipium_app_parent_class)->shutdown(app);
}

static int
clipium_app_command_line(GApplication            *app,
                         GApplicationCommandLine *cmdline)
{
    /* If the remote instance is activated via command line, show the popup */
    clipium_app_activate(app);
    (void)cmdline;
    return 0;
}

/* --- GObject --- */

static void
clipium_app_class_init(ClipiumAppClass *klass)
{
    GApplicationClass *app_class = G_APPLICATION_CLASS(klass);
    app_class->startup = clipium_app_startup;
    app_class->activate = clipium_app_activate;
    app_class->shutdown = clipium_app_shutdown;
    app_class->command_line = clipium_app_command_line;
}

static void
clipium_app_init(ClipiumApp *self)
{
    (void)self;
}

ClipiumApp *
clipium_app_new(void)
{
    return g_object_new(CLIPIUM_TYPE_APP,
                         "application-id", CLIPIUM_APP_ID,
                         "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                         NULL);
}

ClipiumStore *
clipium_app_get_store(ClipiumApp *app)
{
    return app->store;
}

ClipiumDb *
clipium_app_get_db(ClipiumApp *app)
{
    return app->db;
}

ClipiumPaster *
clipium_app_get_paster(ClipiumApp *app)
{
    return app->paster;
}
