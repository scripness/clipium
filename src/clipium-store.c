#include "clipium-store.h"
#include "clipium-fuzzy.h"
#include "clipium-config.h"
#include <string.h>

/* Wrapper for g_array_set_clear_func (GDestroyNotify expects gpointer) */
static void
entry_clear_notify(gpointer data)
{
    clipium_entry_clear((ClipiumEntry *)data);
}

/* --- Helpers --- */

char *
clipium_entry_compute_hash(GBytes *content)
{
    gsize len;
    const guchar *data = g_bytes_get_data(content, &len);
    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_return_val_if_fail(cs != NULL, NULL);
    g_checksum_update(cs, data, (gssize)len);
    char *hash = g_strdup(g_checksum_get_string(cs));
    g_checksum_free(cs);
    return hash;
}

char *
clipium_entry_make_preview(GBytes *content, const char *mime_type)
{
    g_return_val_if_fail(mime_type != NULL, g_strdup("[unknown]"));

    if (!g_str_has_prefix(mime_type, "text/")) {
        gsize len = g_bytes_get_size(content);
        if (len < 1024)
            return g_strdup_printf("[%s %" G_GSIZE_FORMAT "B]", mime_type, len);
        else if (len < 1024 * 1024)
            return g_strdup_printf("[%s %" G_GSIZE_FORMAT "KB]", mime_type, len / 1024);
        else
            return g_strdup_printf("[%s %.1fMB]", mime_type, (double)len / (1024.0 * 1024.0));
    }

    gsize len;
    const char *data = g_bytes_get_data(content, &len);

    /* Build single-line preview up to CLIPIUM_PREVIEW_LEN chars */
    GString *preview = g_string_sized_new(CLIPIUM_PREVIEW_LEN + 4);
    gsize i = 0;
    guint chars = 0;

    while (i < len && chars < CLIPIUM_PREVIEW_LEN) {
        gunichar ch = g_utf8_get_char_validated(data + i, (gssize)(len - i));
        if (ch == (gunichar)-1 || ch == (gunichar)-2)
            break;
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            g_string_append_c(preview, ' ');
        } else {
            g_string_append_unichar(preview, ch);
        }
        i = (gsize)(g_utf8_next_char(data + i) - data);
        chars++;
    }

    /* Trim trailing whitespace — must update GString len after g_strchomp */
    g_strchomp(preview->str);
    preview->len = strlen(preview->str);

    if (i < len)
        g_string_append(preview, "…");

    return g_string_free(preview, FALSE);
}

void
clipium_entry_clear(ClipiumEntry *entry)
{
    g_clear_pointer(&entry->content, g_bytes_unref);
    g_clear_pointer(&entry->mime_type, g_free);
    g_clear_pointer(&entry->preview, g_free);
    g_clear_pointer(&entry->hash, g_free);
}

/* --- Rebuild index helpers --- */

static void
store_rebuild_indices(ClipiumStore *store)
{
    g_hash_table_remove_all(store->by_hash);
    g_hash_table_remove_all(store->by_id);

    for (guint i = 0; i < store->entries->len; i++) {
        ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, i);
        g_hash_table_insert(store->by_hash, e->hash, GUINT_TO_POINTER(i));
        g_hash_table_insert(store->by_id, GSIZE_TO_POINTER((gsize)e->id), GUINT_TO_POINTER(i));
    }
}

/* --- Public API --- */

ClipiumStore *
clipium_store_new(guint max_entries)
{
    ClipiumStore *store = g_new0(ClipiumStore, 1);
    store->entries = g_array_new(FALSE, TRUE, sizeof(ClipiumEntry));
    g_array_set_clear_func(store->entries, entry_clear_notify);
    store->by_hash = g_hash_table_new(g_str_hash, g_str_equal);
    store->by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    store->next_id = 1;
    store->max_entries = max_entries;
    g_mutex_init(&store->lock);
    return store;
}

void
clipium_store_free(ClipiumStore *store)
{
    if (!store) return;
    /* Caller must ensure no other threads access the store */
    g_hash_table_destroy(store->by_hash);
    g_hash_table_destroy(store->by_id);
    g_array_free(store->entries, TRUE);
    g_mutex_clear(&store->lock);
    g_free(store);
}

guint64
clipium_store_add(ClipiumStore *store, GBytes *content, const char *mime_type)
{
    g_return_val_if_fail(store && content && mime_type, 0);

    gsize size = g_bytes_get_size(content);
    if (size == 0)
        return 0;

    g_autofree char *hash = clipium_entry_compute_hash(content);

    g_mutex_lock(&store->lock);

    /* Dedup: if hash exists, bump existing entry to top */
    gpointer idx_ptr;
    if (g_hash_table_lookup_extended(store->by_hash, hash, NULL, &idx_ptr)) {
        guint idx = GPOINTER_TO_UINT(idx_ptr);
        if (idx < store->entries->len) {
            ClipiumEntry existing = g_array_index(store->entries, ClipiumEntry, idx);
            /* Update timestamp */
            existing.timestamp = g_get_real_time();
            /* Zero out the slot so clear func doesn't free our data */
            ClipiumEntry *slot = &g_array_index(store->entries, ClipiumEntry, idx);
            memset(slot, 0, sizeof(ClipiumEntry));
            g_array_remove_index(store->entries, idx);
            /* Prepend */
            g_array_prepend_val(store->entries, existing);
            store_rebuild_indices(store);
        }
        g_mutex_unlock(&store->lock);
        return 0;
    }

    /* Create new entry */
    ClipiumEntry entry = {
        .id        = store->next_id++,
        .content   = g_bytes_ref(content),
        .mime_type = g_strdup(mime_type),
        .preview   = clipium_entry_make_preview(content, mime_type),
        .hash      = g_strdup(hash),
        .timestamp = g_get_real_time(),
        .pinned    = FALSE,
        .size      = size,
    };

    guint64 new_id = entry.id;

    /* Prepend (newest first) */
    g_array_prepend_val(store->entries, entry);

    /* Evict oldest non-pinned if over capacity */
    while (store->entries->len > store->max_entries) {
        gboolean found = FALSE;
        for (int i = (int)store->entries->len - 1; i >= 0; i--) {
            ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, (guint)i);
            if (!e->pinned) {
                g_array_remove_index(store->entries, (guint)i);
                found = TRUE;
                break;
            }
        }
        if (!found) break;
    }

    store_rebuild_indices(store);
    g_mutex_unlock(&store->lock);

    return new_id;
}

void
clipium_store_load_entry(ClipiumStore *store,
                         guint64       id,
                         GBytes       *content,
                         const char   *mime_type,
                         const char   *hash,
                         const char   *preview,
                         gint64        timestamp,
                         gboolean      pinned,
                         gsize         size)
{
    g_mutex_lock(&store->lock);

    ClipiumEntry entry = {
        .id        = id,
        .content   = g_bytes_ref(content),
        .mime_type = g_strdup(mime_type),
        .preview   = g_strdup(preview),
        .hash      = g_strdup(hash),
        .timestamp = timestamp,
        .pinned    = pinned,
        .size      = size,
    };

    g_array_append_val(store->entries, entry);

    if (id >= store->next_id)
        store->next_id = id + 1;

    store_rebuild_indices(store);
    g_mutex_unlock(&store->lock);
}

/* NOTE: Returns pointer into internal array. Safe only when called from the
 * main thread (all store mutations are serialized on the main thread).
 * Do NOT hold this pointer across any call that may modify the store. */
ClipiumEntry *
clipium_store_get(ClipiumStore *store, guint64 id)
{
    g_mutex_lock(&store->lock);
    gpointer idx_ptr;
    if (g_hash_table_lookup_extended(store->by_id, GSIZE_TO_POINTER((gsize)id), NULL, &idx_ptr)) {
        guint idx = GPOINTER_TO_UINT(idx_ptr);
        if (idx < store->entries->len) {
            ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, idx);
            g_mutex_unlock(&store->lock);
            return e;
        }
    }
    g_mutex_unlock(&store->lock);
    return NULL;
}

GArray *
clipium_store_list(ClipiumStore *store, guint limit, guint offset)
{
    g_mutex_lock(&store->lock);

    GArray *result = g_array_new(FALSE, FALSE, sizeof(ClipiumEntry *));
    guint count = 0;

    for (guint i = offset; i < store->entries->len && count < limit; i++, count++) {
        ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, i);
        g_array_append_val(result, e);
    }

    g_mutex_unlock(&store->lock);
    return result;
}

GArray *
clipium_store_search(ClipiumStore *store, const char *query, guint limit)
{
    g_mutex_lock(&store->lock);

    /* Collect matches with scores */
    typedef struct { ClipiumEntry *entry; int score; } Match;
    GArray *matches = g_array_new(FALSE, FALSE, sizeof(Match));

    for (guint i = 0; i < store->entries->len; i++) {
        ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, i);
        int score = clipium_fuzzy_match(query, e->preview);
        if (score >= 0) {
            Match m = { .entry = e, .score = score };
            g_array_append_val(matches, m);
        }
    }

    /* Sort by score descending (insertion sort, fine for small N) */
    for (guint i = 1; i < matches->len; i++) {
        Match key = g_array_index(matches, Match, i);
        int j = (int)i - 1;
        while (j >= 0 && g_array_index(matches, Match, (guint)j).score < key.score) {
            g_array_index(matches, Match, (guint)(j + 1)) = g_array_index(matches, Match, (guint)j);
            j--;
        }
        g_array_index(matches, Match, (guint)(j + 1)) = key;
    }

    GArray *result = g_array_new(FALSE, FALSE, sizeof(ClipiumEntry *));
    guint count = 0;
    for (guint i = 0; i < matches->len && count < limit; i++, count++) {
        Match *m = &g_array_index(matches, Match, i);
        g_array_append_val(result, m->entry);
    }

    g_array_free(matches, TRUE);
    g_mutex_unlock(&store->lock);
    return result;
}

gboolean
clipium_store_delete(ClipiumStore *store, guint64 id)
{
    g_mutex_lock(&store->lock);
    gpointer idx_ptr;
    if (g_hash_table_lookup_extended(store->by_id, GSIZE_TO_POINTER((gsize)id), NULL, &idx_ptr)) {
        guint idx = GPOINTER_TO_UINT(idx_ptr);
        g_array_remove_index(store->entries, idx);
        store_rebuild_indices(store);
        g_mutex_unlock(&store->lock);
        return TRUE;
    }
    g_mutex_unlock(&store->lock);
    return FALSE;
}

void
clipium_store_clear(ClipiumStore *store)
{
    g_mutex_lock(&store->lock);
    g_array_set_size(store->entries, 0);
    g_hash_table_remove_all(store->by_hash);
    g_hash_table_remove_all(store->by_id);
    g_mutex_unlock(&store->lock);
}

gboolean
clipium_store_pin(ClipiumStore *store, guint64 id, gboolean pinned)
{
    g_mutex_lock(&store->lock);
    gpointer idx_ptr;
    if (g_hash_table_lookup_extended(store->by_id, GSIZE_TO_POINTER((gsize)id), NULL, &idx_ptr)) {
        guint idx = GPOINTER_TO_UINT(idx_ptr);
        ClipiumEntry *e = &g_array_index(store->entries, ClipiumEntry, idx);
        e->pinned = pinned;
        g_mutex_unlock(&store->lock);
        return TRUE;
    }
    g_mutex_unlock(&store->lock);
    return FALSE;
}

guint
clipium_store_count(ClipiumStore *store)
{
    g_mutex_lock(&store->lock);
    guint count = store->entries->len;
    g_mutex_unlock(&store->lock);
    return count;
}
