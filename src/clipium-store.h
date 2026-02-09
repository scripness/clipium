#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
    guint64    id;
    GBytes    *content;
    char      *mime_type;
    char      *preview;
    char      *hash;
    gint64     timestamp;
    gboolean   pinned;
    gsize      size;
} ClipiumEntry;

typedef struct {
    GArray     *entries;    /* ClipiumEntry[], newest-first */
    GHashTable *by_hash;   /* char* hash → guint index */
    GHashTable *by_id;     /* guint64 id → guint index */
    guint64     next_id;
    guint       max_entries;
    GMutex      lock;
} ClipiumStore;

ClipiumStore  *clipium_store_new          (guint max_entries);
void           clipium_store_free         (ClipiumStore *store);

/* Returns the new entry's ID, or 0 if deduplicated (existing bumped to top) */
guint64        clipium_store_add          (ClipiumStore *store,
                                           GBytes       *content,
                                           const char   *mime_type);

/* Load an entry from DB (with pre-computed fields) */
void           clipium_store_load_entry   (ClipiumStore *store,
                                           guint64       id,
                                           GBytes       *content,
                                           const char   *mime_type,
                                           const char   *hash,
                                           const char   *preview,
                                           gint64        timestamp,
                                           gboolean      pinned,
                                           gsize         size);

ClipiumEntry  *clipium_store_get          (ClipiumStore *store, guint64 id);
GArray        *clipium_store_list         (ClipiumStore *store, guint limit, guint offset);
GArray        *clipium_store_search       (ClipiumStore *store, const char *query, guint limit);
gboolean       clipium_store_delete       (ClipiumStore *store, guint64 id);
void           clipium_store_clear        (ClipiumStore *store);
gboolean       clipium_store_pin          (ClipiumStore *store, guint64 id, gboolean pinned);
guint          clipium_store_count        (ClipiumStore *store);

void           clipium_entry_clear        (ClipiumEntry *entry);
char          *clipium_entry_make_preview (GBytes *content, const char *mime_type);
char          *clipium_entry_compute_hash (GBytes *content);

G_END_DECLS
