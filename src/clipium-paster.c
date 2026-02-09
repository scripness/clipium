#include "clipium-paster.h"
#include "clipium-config.h"

struct _ClipiumPaster {
    GDBusConnection *bus;
    char            *session_path;
    gboolean         session_active;
};

static gboolean
paster_create_session(ClipiumPaster *self)
{
    if (!self->bus) {
        GError *err = NULL;
        self->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!self->bus) {
            g_warning("Failed to connect to session bus: %s", err->message);
            g_error_free(err);
            return FALSE;
        }
    }

    GError *err = NULL;

    /* CreateSession */
    GVariant *result = g_dbus_connection_call_sync(
        self->bus,
        "org.gnome.Mutter.RemoteDesktop",
        "/org/gnome/Mutter/RemoteDesktop",
        "org.gnome.Mutter.RemoteDesktop",
        "CreateSession",
        NULL,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &err);

    if (!result) {
        g_warning("CreateSession failed: %s", err->message);
        g_error_free(err);
        return FALSE;
    }

    g_free(self->session_path);
    g_variant_get(result, "(o)", &self->session_path);
    g_variant_unref(result);

    /* Start the session */
    result = g_dbus_connection_call_sync(
        self->bus,
        "org.gnome.Mutter.RemoteDesktop",
        self->session_path,
        "org.gnome.Mutter.RemoteDesktop.Session",
        "Start",
        NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &err);

    if (!result) {
        g_warning("Session.Start failed: %s", err->message);
        g_error_free(err);
        g_clear_pointer(&self->session_path, g_free);
        return FALSE;
    }

    g_variant_unref(result);
    self->session_active = TRUE;
    g_message("RemoteDesktop session started: %s", self->session_path);
    return TRUE;
}

static gboolean
paster_send_key(ClipiumPaster *self, guint32 keycode, gboolean pressed)
{
    if (!self->session_active)
        return FALSE;

    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        self->bus,
        "org.gnome.Mutter.RemoteDesktop",
        self->session_path,
        "org.gnome.Mutter.RemoteDesktop.Session",
        "NotifyKeyboardKeycode",
        g_variant_new("(ub)", keycode, pressed),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &err);

    if (!result) {
        g_warning("NotifyKeyboardKeycode failed: %s", err->message);
        g_error_free(err);
        self->session_active = FALSE;
        return FALSE;
    }

    g_variant_unref(result);
    return TRUE;
}

ClipiumPaster *
clipium_paster_new(void)
{
    ClipiumPaster *self = g_new0(ClipiumPaster, 1);
    /* Try to create session, but don't fail if it doesn't work.
     * Will retry on first paste. */
    paster_create_session(self);
    return self;
}

void
clipium_paster_free(ClipiumPaster *self)
{
    if (!self) return;

    if (self->session_active && self->bus && self->session_path) {
        /* Try to stop session */
        g_dbus_connection_call_sync(
            self->bus,
            "org.gnome.Mutter.RemoteDesktop",
            self->session_path,
            "org.gnome.Mutter.RemoteDesktop.Session",
            "Stop",
            NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1, NULL, NULL);
    }

    g_clear_object(&self->bus);
    g_free(self->session_path);
    g_free(self);
}

void
clipium_paster_ctrl_v(ClipiumPaster *self)
{
    if (!self) return;

    /* Ensure session is alive */
    if (!self->session_active) {
        if (!paster_create_session(self)) {
            g_warning("Cannot paste: RemoteDesktop session unavailable");
            return;
        }
    }

    /* Ctrl down */
    if (!paster_send_key(self, CLIPIUM_KEY_LEFTCTRL, TRUE)) {
        /* Session died, retry once */
        paster_create_session(self);
        if (!paster_send_key(self, CLIPIUM_KEY_LEFTCTRL, TRUE))
            return;
    }

    /* Small delay */
    g_usleep(CLIPIUM_KEY_DELAY_MS * 1000);

    /* V down */
    paster_send_key(self, CLIPIUM_KEY_V, TRUE);
    g_usleep(CLIPIUM_KEY_DELAY_MS * 1000);

    /* V up */
    paster_send_key(self, CLIPIUM_KEY_V, FALSE);
    g_usleep(CLIPIUM_KEY_DELAY_MS * 1000);

    /* Ctrl up */
    paster_send_key(self, CLIPIUM_KEY_LEFTCTRL, FALSE);
}
