#include "clipium-db.h"
#include "clipium-config.h"
#include <string.h>

ClipiumDb *
clipium_db_open(const char *path)
{
    ClipiumDb *cdb = g_new0(ClipiumDb, 1);
    cdb->path = g_strdup(path);
    g_mutex_init(&cdb->lock);

    int rc = sqlite3_open(path, &cdb->db);
    if (rc != SQLITE_OK) {
        g_warning("Failed to open database %s: %s", path, sqlite3_errmsg(cdb->db));
        sqlite3_close(cdb->db);
        g_mutex_clear(&cdb->lock);
        g_free(cdb->path);
        g_free(cdb);
        return NULL;
    }

    return cdb;
}

void
clipium_db_close(ClipiumDb *db)
{
    if (!db) return;
    g_mutex_lock(&db->lock);
    if (db->db)
        sqlite3_close(db->db);
    db->db = NULL;
    g_mutex_unlock(&db->lock);
    g_mutex_clear(&db->lock);
    g_free(db->path);
    g_free(db);
}

gboolean
clipium_db_init(ClipiumDb *db)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&db->lock);

    const char *pragmas[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "PRAGMA cache_size=-8000;",
        "PRAGMA busy_timeout=5000;",
        NULL
    };

    for (int i = 0; pragmas[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(db->db, pragmas[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            g_warning("PRAGMA failed: %s", err);
            sqlite3_free(err);
            g_mutex_unlock(&db->lock);
            return FALSE;
        }
    }

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id INTEGER PRIMARY KEY,"
        "  content BLOB NOT NULL,"
        "  mime_type TEXT NOT NULL,"
        "  hash TEXT UNIQUE NOT NULL,"
        "  preview TEXT,"
        "  timestamp INTEGER NOT NULL,"
        "  pinned INTEGER DEFAULT 0,"
        "  size INTEGER NOT NULL"
        ");";

    char *err = NULL;
    int rc = sqlite3_exec(db->db, create_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_warning("CREATE TABLE failed: %s", err);
        sqlite3_free(err);
        g_mutex_unlock(&db->lock);
        return FALSE;
    }

    g_mutex_unlock(&db->lock);
    return TRUE;
}

gboolean
clipium_db_load_all(ClipiumDb *db, ClipiumStore *store)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&db->lock);

    const char *sql = "SELECT id, content, mime_type, hash, preview, timestamp, pinned, size "
                      "FROM clips ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("Failed to prepare load query: %s", sqlite3_errmsg(db->db));
        g_mutex_unlock(&db->lock);
        return FALSE;
    }

    guint count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        guint64 id = (guint64)sqlite3_column_int64(stmt, 0);
        const void *blob = sqlite3_column_blob(stmt, 1);
        int blob_len = sqlite3_column_bytes(stmt, 1);
        const char *mime = (const char *)sqlite3_column_text(stmt, 2);
        const char *hash = (const char *)sqlite3_column_text(stmt, 3);
        const char *preview = (const char *)sqlite3_column_text(stmt, 4);
        gint64 timestamp = sqlite3_column_int64(stmt, 5);
        gboolean pinned = sqlite3_column_int(stmt, 6) != 0;
        gsize size = (gsize)sqlite3_column_int64(stmt, 7);

        /* Skip rows with NULL required fields */
        if (!mime || !hash) {
            g_warning("Skipping corrupt row id=%" G_GUINT64_FORMAT " (NULL mime or hash)", id);
            continue;
        }

        GBytes *content = g_bytes_new(blob, (gsize)blob_len);
        clipium_store_load_entry(store, id, content, mime, hash, preview,
                                 timestamp, pinned, size);
        g_bytes_unref(content);
        count++;
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&db->lock);
    g_message("Loaded %u entries from database", count);
    return TRUE;
}

void
clipium_db_save(ClipiumDb *db, const ClipiumEntry *entry)
{
    g_return_if_fail(db != NULL && entry != NULL && entry->content != NULL);

    g_mutex_lock(&db->lock);

    if (!db->db) {
        g_mutex_unlock(&db->lock);
        return;
    }

    const char *sql = "INSERT OR REPLACE INTO clips "
                      "(id, content, mime_type, hash, preview, timestamp, pinned, size) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("Failed to prepare save: %s", sqlite3_errmsg(db->db));
        g_mutex_unlock(&db->lock);
        return;
    }

    gsize content_len;
    const guchar *content_data = g_bytes_get_data(entry->content, &content_len);

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)entry->id);
    sqlite3_bind_blob(stmt, 2, content_data, (int)content_len, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, entry->mime_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, entry->hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, entry->preview, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, entry->timestamp);
    sqlite3_bind_int(stmt, 7, entry->pinned ? 1 : 0);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)entry->size);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        g_warning("Failed to save entry %" G_GUINT64_FORMAT ": %s", entry->id, sqlite3_errmsg(db->db));

    sqlite3_finalize(stmt);
    g_mutex_unlock(&db->lock);
}

/* Async save using GTask */
typedef struct {
    ClipiumDb *db;
    ClipiumEntry entry_copy;
} SaveTaskData;

static void
save_task_data_free(gpointer data)
{
    SaveTaskData *d = data;
    clipium_entry_clear(&d->entry_copy);
    g_free(d);
}

static void
save_task_thread(GTask        *task,
                 gpointer      source_object,
                 gpointer      task_data,
                 GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    SaveTaskData *d = task_data;
    clipium_db_save(d->db, &d->entry_copy);
    g_task_return_boolean(task, TRUE);
}

void
clipium_db_save_async(ClipiumDb *db, const ClipiumEntry *entry)
{
    g_return_if_fail(db != NULL && entry != NULL && entry->content != NULL);

    SaveTaskData *d = g_new0(SaveTaskData, 1);
    d->db = db;
    d->entry_copy = (ClipiumEntry){
        .id        = entry->id,
        .content   = g_bytes_ref(entry->content),
        .mime_type = g_strdup(entry->mime_type),
        .preview   = g_strdup(entry->preview),
        .hash      = g_strdup(entry->hash),
        .timestamp = entry->timestamp,
        .pinned    = entry->pinned,
        .size      = entry->size,
    };

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, save_task_data_free);
    g_task_run_in_thread(task, save_task_thread);
    g_object_unref(task);
}

gboolean
clipium_db_delete(ClipiumDb *db, guint64 id)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&db->lock);
    if (!db->db) { g_mutex_unlock(&db->lock); return FALSE; }

    const char *sql = "DELETE FROM clips WHERE id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { g_mutex_unlock(&db->lock); return FALSE; }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&db->lock);
    return rc == SQLITE_DONE;
}

gboolean
clipium_db_clear(ClipiumDb *db)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&db->lock);
    if (!db->db) { g_mutex_unlock(&db->lock); return FALSE; }

    char *err = NULL;
    int rc = sqlite3_exec(db->db, "DELETE FROM clips;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_warning("Failed to clear: %s", err);
        sqlite3_free(err);
        g_mutex_unlock(&db->lock);
        return FALSE;
    }
    g_mutex_unlock(&db->lock);
    return TRUE;
}

gboolean
clipium_db_update_pin(ClipiumDb *db, guint64 id, gboolean pinned)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&db->lock);
    if (!db->db) { g_mutex_unlock(&db->lock); return FALSE; }

    const char *sql = "UPDATE clips SET pinned = ? WHERE id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { g_mutex_unlock(&db->lock); return FALSE; }

    sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&db->lock);
    return rc == SQLITE_DONE;
}
