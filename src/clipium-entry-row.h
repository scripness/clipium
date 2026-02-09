#pragma once

#include <adwaita.h>
#include "clipium-store.h"

G_BEGIN_DECLS

#define CLIPIUM_TYPE_ENTRY_ROW (clipium_entry_row_get_type())
G_DECLARE_FINAL_TYPE(ClipiumEntryRow, clipium_entry_row, CLIPIUM, ENTRY_ROW, GtkListBoxRow)

ClipiumEntryRow *clipium_entry_row_new    (const ClipiumEntry *entry);
guint64          clipium_entry_row_get_id (ClipiumEntryRow *row);
const char      *clipium_entry_row_get_preview(ClipiumEntryRow *row);

G_END_DECLS
