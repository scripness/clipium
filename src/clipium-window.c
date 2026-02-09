#include "clipium-window.h"
#include "clipium-entry-row.h"
#include "clipium-config.h"
#include <gtk4-layer-shell.h>
#include <string.h>

struct _ClipiumWindow {
    GtkWindow       parent;
    ClipiumStore   *store;
    ClipiumDb      *db;
    ClipiumPaster  *paster;

    /* Widgets */
    GtkBox         *overlay_box;    /* Full-screen transparent background */
    GtkBox         *card_box;       /* Centered card */
    GtkSearchEntry *search_entry;
    GtkListBox     *listbox;
    GtkScrolledWindow *scrolled;
    GtkLabel       *hint_label;
    GtkLabel       *empty_label;

    GtkCssProvider *css_provider;
};

G_DEFINE_TYPE(ClipiumWindow, clipium_window, GTK_TYPE_WINDOW)

/* --- CSS --- */

static const char *CLIPIUM_CSS =
    "window.clipium-overlay {"
    "  background-color: rgba(0, 0, 0, 0.5);"
    "}"
    ".clipium-card {"
    "  background-color: @window_bg_color;"
    "  border-radius: 12px;"
    "  border: 1px solid alpha(@window_fg_color, 0.1);"
    "  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);"
    "}"
    ".clipium-search {"
    "  margin: 12px;"
    "  font-size: 14px;"
    "}"
    ".clipium-listbox {"
    "  background: transparent;"
    "}"
    ".clipium-listbox row {"
    "  border-radius: 8px;"
    "  margin: 2px 8px;"
    "}"
    ".clipium-listbox row:selected {"
    "  background-color: @accent_bg_color;"
    "}"
    ".clipium-hint {"
    "  padding: 8px 12px;"
    "  font-size: 11px;"
    "}"
    ".clipium-empty {"
    "  padding: 24px;"
    "  font-size: 13px;"
    "}";

/* --- Forward declarations --- */

static void populate_listbox(ClipiumWindow *self, const char *search_query);
static void on_row_activated(GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data);
static void select_current_row(ClipiumWindow *self);

/* --- Paste flow --- */

typedef struct {
    ClipiumPaster *paster;
} PasteData;

static gboolean
do_paste_after_delay(gpointer user_data)
{
    PasteData *pd = user_data;
    clipium_paster_ctrl_v(pd->paster);
    g_free(pd);
    return G_SOURCE_REMOVE;
}

static void
do_select_entry(ClipiumWindow *self, guint64 entry_id)
{
    ClipiumEntry *entry = clipium_store_get(self->store, entry_id);
    if (!entry) return;

    /* Copy to clipboard via wl-copy */
    gsize content_len;
    const char *content_data = g_bytes_get_data(entry->content, &content_len);

    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE,
        &err,
        "wl-copy", "--type", entry->mime_type, NULL);

    if (proc) {
        GOutputStream *stdin_pipe = g_subprocess_get_stdin_pipe(proc);
        g_output_stream_write_all(stdin_pipe, content_data, content_len, NULL, NULL, NULL);
        g_output_stream_close(stdin_pipe, NULL, NULL);
        g_object_unref(proc);
    } else {
        g_warning("Failed to run wl-copy: %s", err->message);
        g_error_free(err);
    }

    /* Hide window */
    clipium_window_hide_popup(self);

    /* After delay, simulate Ctrl+V */
    if (self->paster) {
        PasteData *pd = g_new(PasteData, 1);
        pd->paster = self->paster;
        g_timeout_add(CLIPIUM_PASTE_DELAY_MS, do_paste_after_delay, pd);
    }
}

/* --- Search changed --- */

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    ClipiumWindow *self = CLIPIUM_WINDOW(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    populate_listbox(self, (text && *text) ? text : NULL);
}

/* --- Key handler --- */

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               gpointer               user_data)
{
    (void)controller;
    (void)keycode;
    ClipiumWindow *self = CLIPIUM_WINDOW(user_data);

    if (keyval == GDK_KEY_Escape) {
        clipium_window_hide_popup(self);
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        select_current_row(self);
        return TRUE;
    }

    if (keyval == GDK_KEY_Delete && (state & GDK_SHIFT_MASK)) {
        /* Shift+Delete: remove selected entry */
        GtkListBoxRow *row = gtk_list_box_get_selected_row(self->listbox);
        if (row && CLIPIUM_IS_ENTRY_ROW(row)) {
            guint64 id = clipium_entry_row_get_id(CLIPIUM_ENTRY_ROW(row));
            clipium_store_delete(self->store, id);
            if (self->db)
                clipium_db_delete(self->db, id);
            const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
            populate_listbox(self, (text && *text) ? text : NULL);
        }
        return TRUE;
    }

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        /* Let the listbox handle navigation, but ensure focus stays on search */
        GtkListBoxRow *selected = gtk_list_box_get_selected_row(self->listbox);
        if (!selected) {
            GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->listbox, 0);
            if (first)
                gtk_list_box_select_row(self->listbox, first);
            return TRUE;
        }

        int idx = gtk_list_box_row_get_index(selected);
        GtkListBoxRow *next = NULL;

        if (keyval == GDK_KEY_Down)
            next = gtk_list_box_get_row_at_index(self->listbox, idx + 1);
        else if (keyval == GDK_KEY_Up && idx > 0)
            next = gtk_list_box_get_row_at_index(self->listbox, idx - 1);

        if (next)
            gtk_list_box_select_row(self->listbox, next);

        return TRUE;
    }

    return FALSE;
}

/* --- Click on overlay background to dismiss --- */

static void
on_overlay_click(GtkGestureClick *gesture,
                 int              n_press,
                 double           x,
                 double           y,
                 gpointer         user_data)
{
    (void)gesture;
    (void)n_press;
    ClipiumWindow *self = CLIPIUM_WINDOW(user_data);

    /* Check if click is outside the card */
    graphene_point_t point = GRAPHENE_POINT_INIT((float)x, (float)y);
    graphene_point_t card_point;
    if (!gtk_widget_compute_point(GTK_WIDGET(self->overlay_box),
                                   GTK_WIDGET(self->card_box),
                                   &point, &card_point)) {
        clipium_window_hide_popup(self);
        return;
    }

    int card_w = gtk_widget_get_width(GTK_WIDGET(self->card_box));
    int card_h = gtk_widget_get_height(GTK_WIDGET(self->card_box));

    if (card_point.x < 0 || card_point.y < 0 ||
        card_point.x > card_w || card_point.y > card_h) {
        clipium_window_hide_popup(self);
    }
}

/* --- Populate listbox --- */

static void
populate_listbox(ClipiumWindow *self, const char *search_query)
{
    /* Remove all existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->listbox))))
        gtk_list_box_remove(self->listbox, child);

    GArray *entries;
    if (search_query)
        entries = clipium_store_search(self->store, search_query, 50);
    else
        entries = clipium_store_list(self->store, 50, 0);

    if (entries->len == 0) {
        gtk_widget_set_visible(GTK_WIDGET(self->scrolled), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->empty_label), TRUE);
        if (search_query)
            gtk_label_set_text(self->empty_label, "No matches found");
        else
            gtk_label_set_text(self->empty_label, "Clipboard is empty");
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->scrolled), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(self->empty_label), FALSE);

        for (guint i = 0; i < entries->len; i++) {
            ClipiumEntry *e = g_array_index(entries, ClipiumEntry *, i);
            ClipiumEntryRow *row = clipium_entry_row_new(e);
            gtk_list_box_append(self->listbox, GTK_WIDGET(row));
        }

        /* Select first row */
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->listbox, 0);
        if (first)
            gtk_list_box_select_row(self->listbox, first);
    }

    g_array_free(entries, TRUE);
}

static void
select_current_row(ClipiumWindow *self)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(self->listbox);
    if (row && CLIPIUM_IS_ENTRY_ROW(row)) {
        guint64 id = clipium_entry_row_get_id(CLIPIUM_ENTRY_ROW(row));
        do_select_entry(self, id);
    }
}

/* --- GObject --- */

static void
clipium_window_finalize(GObject *obj)
{
    ClipiumWindow *self = CLIPIUM_WINDOW(obj);
    if (self->css_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(self->css_provider));
        g_clear_object(&self->css_provider);
    }
    G_OBJECT_CLASS(clipium_window_parent_class)->finalize(obj);
}

static void
clipium_window_class_init(ClipiumWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clipium_window_finalize;
}

static void
clipium_window_init(ClipiumWindow *self)
{
    (void)self;
}

/* --- Constructor --- */

ClipiumWindow *
clipium_window_new(GtkApplication *app, ClipiumStore *store, ClipiumDb *db, ClipiumPaster *paster)
{
    ClipiumWindow *self = g_object_new(CLIPIUM_TYPE_WINDOW,
                                        "application", app,
                                        NULL);
    self->store = store;
    self->db = db;
    self->paster = paster;

    /* Layer shell setup — must be done before realize */
    gtk_layer_init_for_window(GTK_WINDOW(self));
    gtk_layer_set_layer(GTK_WINDOW(self), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(self), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_anchor(GTK_WINDOW(self), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(self), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(self), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(self), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_namespace(GTK_WINDOW(self), "clipium");

    gtk_widget_add_css_class(GTK_WIDGET(self), "clipium-overlay");

    /* CSS */
    self->css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(self->css_provider, CLIPIUM_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(self->css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Overlay box (fullscreen, transparent, centers the card) */
    self->overlay_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_set_halign(GTK_WIDGET(self->overlay_box), GTK_ALIGN_FILL);
    gtk_widget_set_valign(GTK_WIDGET(self->overlay_box), GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(GTK_WIDGET(self->overlay_box), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self->overlay_box), TRUE);

    /* Click gesture on overlay to dismiss */
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_overlay_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->overlay_box), GTK_EVENT_CONTROLLER(click));

    /* Card box (centered, fixed width) */
    self->card_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_add_css_class(GTK_WIDGET(self->card_box), "clipium-card");
    gtk_widget_set_halign(GTK_WIDGET(self->card_box), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(self->card_box), GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(GTK_WIDGET(self->card_box), CLIPIUM_CARD_WIDTH, -1);

    /* Search entry */
    self->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(self->search_entry), "clipium-search");
    g_object_set(self->search_entry, "placeholder-text", "Search clipboard...", NULL);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

    /* Scrolled window + listbox */
    self->scrolled = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(self->scrolled, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(self->scrolled), TRUE);
    gtk_scrolled_window_set_max_content_height(self->scrolled, 400);
    gtk_scrolled_window_set_propagate_natural_height(self->scrolled, TRUE);

    self->listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(self->listbox), "clipium-listbox");
    gtk_list_box_set_selection_mode(self->listbox, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(self->listbox, TRUE);
    g_signal_connect(self->listbox, "row-activated", G_CALLBACK(on_row_activated), self);

    gtk_scrolled_window_set_child(self->scrolled, GTK_WIDGET(self->listbox));

    /* Empty label */
    self->empty_label = GTK_LABEL(gtk_label_new("Clipboard is empty"));
    gtk_widget_add_css_class(GTK_WIDGET(self->empty_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(self->empty_label), "clipium-empty");
    gtk_widget_set_visible(GTK_WIDGET(self->empty_label), FALSE);

    /* Hint bar */
    self->hint_label = GTK_LABEL(gtk_label_new("↑↓ Navigate  ⏎ Paste  ⇧Del Remove  Esc Close"));
    gtk_widget_add_css_class(GTK_WIDGET(self->hint_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(self->hint_label), "clipium-hint");

    /* Separator above hints */
    GtkSeparator *sep = GTK_SEPARATOR(gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* Assemble card */
    gtk_box_append(self->card_box, GTK_WIDGET(self->search_entry));
    gtk_box_append(self->card_box, GTK_WIDGET(self->scrolled));
    gtk_box_append(self->card_box, GTK_WIDGET(self->empty_label));
    gtk_box_append(self->card_box, GTK_WIDGET(sep));
    gtk_box_append(self->card_box, GTK_WIDGET(self->hint_label));

    /* Put card in overlay */
    gtk_box_append(self->overlay_box, GTK_WIDGET(self->card_box));

    gtk_window_set_child(GTK_WINDOW(self), GTK_WIDGET(self->overlay_box));

    /* Key controller — on the window so it captures all keys */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);

    /* Start hidden */
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);

    return self;
}

/* --- Show/Hide --- */

void
clipium_window_show_popup(ClipiumWindow *self)
{
    /* Populate with all entries */
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    populate_listbox(self, NULL);

    gtk_widget_set_visible(GTK_WIDGET(self), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
}

void
clipium_window_hide_popup(ClipiumWindow *self)
{
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

/* --- Row activated (click) --- */

static void
on_row_activated(GtkListBox    *listbox,
                 GtkListBoxRow *row,
                 gpointer       user_data)
{
    (void)listbox;
    ClipiumWindow *self = CLIPIUM_WINDOW(user_data);

    if (row && CLIPIUM_IS_ENTRY_ROW(row)) {
        guint64 id = clipium_entry_row_get_id(CLIPIUM_ENTRY_ROW(row));
        do_select_entry(self, id);
    }
}
