#include "clipium-entry-row.h"
#include "clipium-config.h"

struct _ClipiumEntryRow {
    GtkListBoxRow parent;
    guint64       entry_id;
    char         *preview;
    char         *mime_type;
    gboolean      pinned;
    GtkLabel     *preview_label;
    GtkLabel     *time_label;
    GtkImage     *pin_icon;
};

G_DEFINE_TYPE(ClipiumEntryRow, clipium_entry_row, GTK_TYPE_LIST_BOX_ROW)

static char *
format_time_ago(gint64 timestamp)
{
    gint64 now = g_get_real_time();
    gint64 diff = (now - timestamp) / G_USEC_PER_SEC;

    if (diff < 5) return g_strdup("now");
    if (diff < 60) return g_strdup_printf("%" G_GINT64_FORMAT "s", diff);
    if (diff < 3600) return g_strdup_printf("%" G_GINT64_FORMAT "m", diff / 60);
    if (diff < 86400) return g_strdup_printf("%" G_GINT64_FORMAT "h", diff / 3600);
    return g_strdup_printf("%" G_GINT64_FORMAT "d", diff / 86400);
}

static void
clipium_entry_row_finalize(GObject *obj)
{
    ClipiumEntryRow *self = CLIPIUM_ENTRY_ROW(obj);
    g_free(self->preview);
    g_free(self->mime_type);
    G_OBJECT_CLASS(clipium_entry_row_parent_class)->finalize(obj);
}

static void
clipium_entry_row_class_init(ClipiumEntryRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clipium_entry_row_finalize;
}

static void
clipium_entry_row_init(ClipiumEntryRow *self)
{
    (void)self;
}

ClipiumEntryRow *
clipium_entry_row_new(const ClipiumEntry *entry)
{
    ClipiumEntryRow *self = g_object_new(CLIPIUM_TYPE_ENTRY_ROW, NULL);
    self->entry_id = entry->id;
    self->preview = g_strdup(entry->preview);
    self->mime_type = g_strdup(entry->mime_type);
    self->pinned = entry->pinned;

    /* Build widget tree: HBox [ icon | preview_label | time_label | pin ] */
    GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    gtk_widget_set_margin_start(GTK_WIDGET(hbox), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(hbox), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(hbox), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(hbox), 6);

    /* Type icon */
    const char *icon_name;
    if (g_str_has_prefix(entry->mime_type, "image/"))
        icon_name = "image-x-generic-symbolic";
    else
        icon_name = "edit-paste-symbolic";

    GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name(icon_name));
    gtk_widget_add_css_class(GTK_WIDGET(icon), "dim-label");
    gtk_box_append(hbox, GTK_WIDGET(icon));

    /* Preview label */
    self->preview_label = GTK_LABEL(gtk_label_new(entry->preview));
    gtk_label_set_ellipsize(self->preview_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(self->preview_label, 0.0f);
    gtk_widget_set_hexpand(GTK_WIDGET(self->preview_label), TRUE);
    gtk_box_append(hbox, GTK_WIDGET(self->preview_label));

    /* Pin icon */
    if (entry->pinned) {
        self->pin_icon = GTK_IMAGE(gtk_image_new_from_icon_name("view-pin-symbolic"));
        gtk_widget_add_css_class(GTK_WIDGET(self->pin_icon), "dim-label");
        gtk_box_append(hbox, GTK_WIDGET(self->pin_icon));
    }

    /* Time label */
    g_autofree char *time_str = format_time_ago(entry->timestamp);
    self->time_label = GTK_LABEL(gtk_label_new(time_str));
    gtk_widget_add_css_class(GTK_WIDGET(self->time_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(self->time_label), "caption");
    gtk_box_append(hbox, GTK_WIDGET(self->time_label));

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(self), GTK_WIDGET(hbox));

    return self;
}

guint64
clipium_entry_row_get_id(ClipiumEntryRow *row)
{
    return row->entry_id;
}

const char *
clipium_entry_row_get_preview(ClipiumEntryRow *row)
{
    return row->preview;
}
