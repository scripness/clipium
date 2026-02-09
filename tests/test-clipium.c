#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sqlite3.h>

#include "clipium-store.h"
#include "clipium-fuzzy.h"
#include "clipium-db.h"
#include "clipium-config.h"

/* ======== Store Tests ======== */

static void
test_store_new_free(void)
{
    ClipiumStore *store = clipium_store_new(100);
    g_assert_nonnull(store);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);
    clipium_store_free(store);
}

static void
test_store_add_single(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *content = g_bytes_new_static("hello world", 11);

    guint64 id = clipium_store_add(store, content, "text/plain");
    g_assert_cmpuint(id, >, 0);
    g_assert_cmpuint(clipium_store_count(store), ==, 1);

    ClipiumEntry *entry = clipium_store_get(store, id);
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->mime_type, ==, "text/plain");
    g_assert_cmpstr(entry->preview, ==, "hello world");
    g_assert_cmpuint(entry->size, ==, 11);
    g_assert_false(entry->pinned);

    g_bytes_unref(content);
    clipium_store_free(store);
}

static void
test_store_add_empty_rejected(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *empty = g_bytes_new_static("", 0);

    guint64 id = clipium_store_add(store, empty, "text/plain");
    g_assert_cmpuint(id, ==, 0);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);

    g_bytes_unref(empty);
    clipium_store_free(store);
}

static void
test_store_dedup(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *content = g_bytes_new_static("duplicate", 9);

    guint64 id1 = clipium_store_add(store, content, "text/plain");
    g_assert_cmpuint(id1, >, 0);

    /* Second add of same content should dedup (return 0) */
    guint64 id2 = clipium_store_add(store, content, "text/plain");
    g_assert_cmpuint(id2, ==, 0);

    /* Still only one entry */
    g_assert_cmpuint(clipium_store_count(store), ==, 1);

    g_bytes_unref(content);
    clipium_store_free(store);
}

static void
test_store_ordering(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *c1 = g_bytes_new_static("first", 5);
    GBytes *c2 = g_bytes_new_static("second", 6);
    GBytes *c3 = g_bytes_new_static("third", 5);

    clipium_store_add(store, c1, "text/plain");
    clipium_store_add(store, c2, "text/plain");
    guint64 id3 = clipium_store_add(store, c3, "text/plain");

    /* Newest (id3) should be at index 0 */
    GArray *list = clipium_store_list(store, 10, 0);
    g_assert_cmpuint(list->len, ==, 3);
    ClipiumEntry *first = g_array_index(list, ClipiumEntry *, 0);
    g_assert_cmpuint(first->id, ==, id3);
    g_array_free(list, TRUE);

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    g_bytes_unref(c3);
    clipium_store_free(store);
}

static void
test_store_eviction(void)
{
    ClipiumStore *store = clipium_store_new(3);
    GBytes *c1 = g_bytes_new_static("aaa", 3);
    GBytes *c2 = g_bytes_new_static("bbb", 3);
    GBytes *c3 = g_bytes_new_static("ccc", 3);
    GBytes *c4 = g_bytes_new_static("ddd", 3);

    guint64 id1 = clipium_store_add(store, c1, "text/plain");
    clipium_store_add(store, c2, "text/plain");
    clipium_store_add(store, c3, "text/plain");
    g_assert_cmpuint(clipium_store_count(store), ==, 3);

    /* Adding a 4th should evict the oldest (id1) */
    clipium_store_add(store, c4, "text/plain");
    g_assert_cmpuint(clipium_store_count(store), ==, 3);

    /* id1 should be gone */
    ClipiumEntry *evicted = clipium_store_get(store, id1);
    g_assert_null(evicted);

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    g_bytes_unref(c3);
    g_bytes_unref(c4);
    clipium_store_free(store);
}

static void
test_store_eviction_pinned(void)
{
    ClipiumStore *store = clipium_store_new(3);
    GBytes *c1 = g_bytes_new_static("aaa", 3);
    GBytes *c2 = g_bytes_new_static("bbb", 3);
    GBytes *c3 = g_bytes_new_static("ccc", 3);
    GBytes *c4 = g_bytes_new_static("ddd", 3);

    guint64 id1 = clipium_store_add(store, c1, "text/plain");
    guint64 id2 = clipium_store_add(store, c2, "text/plain");
    clipium_store_add(store, c3, "text/plain");

    /* Pin the oldest */
    clipium_store_pin(store, id1, TRUE);

    /* Adding 4th should evict id2 (oldest non-pinned), not id1 */
    clipium_store_add(store, c4, "text/plain");
    g_assert_cmpuint(clipium_store_count(store), ==, 3);

    g_assert_nonnull(clipium_store_get(store, id1)); /* pinned, still there */
    g_assert_null(clipium_store_get(store, id2));    /* evicted */

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    g_bytes_unref(c3);
    g_bytes_unref(c4);
    clipium_store_free(store);
}

static void
test_store_delete(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *content = g_bytes_new_static("delete me", 9);

    guint64 id = clipium_store_add(store, content, "text/plain");
    g_assert_cmpuint(clipium_store_count(store), ==, 1);

    gboolean ok = clipium_store_delete(store, id);
    g_assert_true(ok);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);

    /* Double delete should fail */
    ok = clipium_store_delete(store, id);
    g_assert_false(ok);

    g_bytes_unref(content);
    clipium_store_free(store);
}

static void
test_store_clear(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *c1 = g_bytes_new_static("aaa", 3);
    GBytes *c2 = g_bytes_new_static("bbb", 3);

    clipium_store_add(store, c1, "text/plain");
    clipium_store_add(store, c2, "text/plain");
    g_assert_cmpuint(clipium_store_count(store), ==, 2);

    clipium_store_clear(store);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    clipium_store_free(store);
}

static void
test_store_pin(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *content = g_bytes_new_static("pin me", 6);

    guint64 id = clipium_store_add(store, content, "text/plain");
    ClipiumEntry *entry = clipium_store_get(store, id);
    g_assert_false(entry->pinned);

    gboolean ok = clipium_store_pin(store, id, TRUE);
    g_assert_true(ok);
    entry = clipium_store_get(store, id);
    g_assert_true(entry->pinned);

    ok = clipium_store_pin(store, id, FALSE);
    g_assert_true(ok);
    entry = clipium_store_get(store, id);
    g_assert_false(entry->pinned);

    /* Pin non-existent */
    ok = clipium_store_pin(store, 9999, TRUE);
    g_assert_false(ok);

    g_bytes_unref(content);
    clipium_store_free(store);
}

static void
test_store_list_offset_limit(void)
{
    ClipiumStore *store = clipium_store_new(100);
    for (int i = 0; i < 10; i++) {
        char buf[16];
        g_snprintf(buf, sizeof(buf), "item-%d", i);
        GBytes *c = g_bytes_new(buf, strlen(buf));
        clipium_store_add(store, c, "text/plain");
        g_bytes_unref(c);
    }
    g_assert_cmpuint(clipium_store_count(store), ==, 10);

    /* Get 3 items starting at offset 2 */
    GArray *list = clipium_store_list(store, 3, 2);
    g_assert_cmpuint(list->len, ==, 3);
    g_array_free(list, TRUE);

    /* Offset past end */
    list = clipium_store_list(store, 10, 100);
    g_assert_cmpuint(list->len, ==, 0);
    g_array_free(list, TRUE);

    clipium_store_free(store);
}

static void
test_store_search(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *c1 = g_bytes_new_static("hello world", 11);
    GBytes *c2 = g_bytes_new_static("goodbye", 7);
    GBytes *c3 = g_bytes_new_static("hello there", 11);

    clipium_store_add(store, c1, "text/plain");
    clipium_store_add(store, c2, "text/plain");
    clipium_store_add(store, c3, "text/plain");

    GArray *results = clipium_store_search(store, "hello", 10);
    g_assert_cmpuint(results->len, ==, 2);
    g_array_free(results, TRUE);

    results = clipium_store_search(store, "goodbye", 10);
    g_assert_cmpuint(results->len, ==, 1);
    g_array_free(results, TRUE);

    results = clipium_store_search(store, "zzz", 10);
    g_assert_cmpuint(results->len, ==, 0);
    g_array_free(results, TRUE);

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    g_bytes_unref(c3);
    clipium_store_free(store);
}

static void
test_store_dedup_bumps_to_top(void)
{
    ClipiumStore *store = clipium_store_new(100);
    GBytes *c1 = g_bytes_new_static("aaa", 3);
    GBytes *c2 = g_bytes_new_static("bbb", 3);

    guint64 id1 = clipium_store_add(store, c1, "text/plain");
    clipium_store_add(store, c2, "text/plain");
    (void)id1;

    /* Re-add c1 — should dedup but bump to top */
    clipium_store_add(store, c1, "text/plain");

    GArray *list = clipium_store_list(store, 10, 0);
    g_assert_cmpuint(list->len, ==, 2);
    /* First entry should have "aaa" preview (bumped to top) */
    ClipiumEntry *top = g_array_index(list, ClipiumEntry *, 0);
    g_assert_cmpstr(top->preview, ==, "aaa");
    g_array_free(list, TRUE);

    g_bytes_unref(c1);
    g_bytes_unref(c2);
    clipium_store_free(store);
}

/* ======== Entry Helper Tests ======== */

static void
test_entry_compute_hash(void)
{
    GBytes *content = g_bytes_new_static("test", 4);
    char *hash = clipium_entry_compute_hash(content);
    g_assert_nonnull(hash);
    /* SHA-256 hex is 64 chars */
    g_assert_cmpuint(strlen(hash), ==, 64);

    /* Same content produces same hash */
    char *hash2 = clipium_entry_compute_hash(content);
    g_assert_cmpstr(hash, ==, hash2);

    g_free(hash);
    g_free(hash2);
    g_bytes_unref(content);
}

static void
test_entry_compute_hash_deterministic(void)
{
    GBytes *c1 = g_bytes_new_static("hello", 5);
    GBytes *c2 = g_bytes_new_static("hello", 5);
    GBytes *c3 = g_bytes_new_static("world", 5);

    char *h1 = clipium_entry_compute_hash(c1);
    char *h2 = clipium_entry_compute_hash(c2);
    char *h3 = clipium_entry_compute_hash(c3);

    g_assert_cmpstr(h1, ==, h2);
    g_assert_cmpstr(h1, !=, h3);

    g_free(h1);
    g_free(h2);
    g_free(h3);
    g_bytes_unref(c1);
    g_bytes_unref(c2);
    g_bytes_unref(c3);
}

static void
test_entry_make_preview_text(void)
{
    GBytes *content = g_bytes_new_static("Hello\nWorld\tFoo", 15);
    char *preview = clipium_entry_make_preview(content, "text/plain");
    g_assert_nonnull(preview);
    /* Newlines and tabs should be replaced with spaces */
    g_assert_null(strchr(preview, '\n'));
    g_assert_null(strchr(preview, '\t'));
    g_assert_true(g_str_has_prefix(preview, "Hello"));
    g_free(preview);
    g_bytes_unref(content);
}

static void
test_entry_make_preview_binary(void)
{
    GBytes *content = g_bytes_new_static("\x89PNG\r\n\x1a\n", 8);
    char *preview = clipium_entry_make_preview(content, "image/png");
    g_assert_nonnull(preview);
    /* Should show format like "[image/png 8B]" */
    g_assert_true(g_str_has_prefix(preview, "[image/png"));
    g_free(preview);
    g_bytes_unref(content);
}

static void
test_entry_make_preview_truncates(void)
{
    /* Create a long text */
    GString *long_text = g_string_new(NULL);
    for (int i = 0; i < 200; i++)
        g_string_append_c(long_text, 'A');

    GBytes *content = g_bytes_new(long_text->str, long_text->len);
    char *preview = clipium_entry_make_preview(content, "text/plain");
    g_assert_nonnull(preview);
    /* Preview should be truncated to ~CLIPIUM_PREVIEW_LEN + ellipsis */
    g_assert_true(strlen(preview) <= CLIPIUM_PREVIEW_LEN + 4);
    g_assert_true(g_str_has_suffix(preview, "\xe2\x80\xa6")); /* UTF-8 ellipsis */

    g_free(preview);
    g_bytes_unref(content);
    g_string_free(long_text, TRUE);
}

static void
test_entry_make_preview_trailing_whitespace(void)
{
    GBytes *content = g_bytes_new_static("hello   \n\n\n", 11);
    char *preview = clipium_entry_make_preview(content, "text/plain");
    g_assert_nonnull(preview);
    /* Trailing whitespace should be trimmed */
    g_assert_false(g_str_has_suffix(preview, " "));
    g_free(preview);
    g_bytes_unref(content);
}

/* ======== Fuzzy Match Tests ======== */

static void
test_fuzzy_match_exact(void)
{
    int score = clipium_fuzzy_match("hello", "hello");
    g_assert_cmpint(score, >, 0);
}

static void
test_fuzzy_match_substring(void)
{
    int score = clipium_fuzzy_match("hlo", "hello");
    g_assert_cmpint(score, >, 0);
}

static void
test_fuzzy_match_no_match(void)
{
    int score = clipium_fuzzy_match("xyz", "hello");
    g_assert_cmpint(score, ==, -1);
}

static void
test_fuzzy_match_case_insensitive(void)
{
    int score = clipium_fuzzy_match("HELLO", "hello world");
    g_assert_cmpint(score, >, 0);
}

static void
test_fuzzy_match_empty_query(void)
{
    int score = clipium_fuzzy_match("", "hello");
    g_assert_cmpint(score, ==, 0);
}

static void
test_fuzzy_match_empty_target(void)
{
    int score = clipium_fuzzy_match("hello", "");
    g_assert_cmpint(score, ==, -1);
}

static void
test_fuzzy_match_null_args(void)
{
    g_assert_cmpint(clipium_fuzzy_match(NULL, "hello"), ==, 0);
    g_assert_cmpint(clipium_fuzzy_match("hello", NULL), ==, -1);
}

static void
test_fuzzy_match_scoring(void)
{
    /* Exact prefix match should score higher than scattered match */
    int score_prefix = clipium_fuzzy_match("hel", "hello world");
    int score_scatter = clipium_fuzzy_match("hld", "hello world");
    g_assert_cmpint(score_prefix, >, score_scatter);
}

static void
test_fuzzy_match_separator_bonus(void)
{
    /* Match after separator should score higher */
    int score_sep = clipium_fuzzy_match("w", "hello world");
    int score_mid = clipium_fuzzy_match("o", "hello world");
    /* 'w' starts a word (after space), so gets separator bonus */
    g_assert_cmpint(score_sep, >, score_mid);
}

/* ======== Database Tests ======== */

static ClipiumDb *
create_temp_db(void)
{
    g_autofree char *path = g_build_filename(g_get_tmp_dir(), "clipium-test-XXXXXX.db", NULL);
    /* Use mkstemp pattern manually for uniqueness */
    g_autofree char *real_path = g_strdup_printf("%s/clipium-test-%d.db", g_get_tmp_dir(), g_random_int());
    ClipiumDb *db = clipium_db_open(real_path);
    if (db)
        clipium_db_init(db);
    return db;
}

static void
test_db_open_close(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);
    g_autofree char *path = g_strdup(db->path);
    clipium_db_close(db);
    g_unlink(path);
}

static void
test_db_save_and_load(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    /* Create and save an entry */
    GBytes *content = g_bytes_new_static("test content", 12);
    ClipiumEntry entry = {
        .id        = 1,
        .content   = content,
        .mime_type = "text/plain",
        .preview   = "test content",
        .hash      = "abc123",
        .timestamp = g_get_real_time(),
        .pinned    = FALSE,
        .size      = 12,
    };

    clipium_db_save(db, &entry);

    /* Load into a store */
    ClipiumStore *store = clipium_store_new(100);
    gboolean ok = clipium_db_load_all(db, store);
    g_assert_true(ok);
    g_assert_cmpuint(clipium_store_count(store), ==, 1);

    ClipiumEntry *loaded = clipium_store_get(store, 1);
    g_assert_nonnull(loaded);
    g_assert_cmpstr(loaded->mime_type, ==, "text/plain");
    g_assert_cmpstr(loaded->preview, ==, "test content");
    g_assert_cmpuint(loaded->size, ==, 12);

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(content);
    clipium_store_free(store);
    clipium_db_close(db);
    g_unlink(path);
}

static void
test_db_delete(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    GBytes *content = g_bytes_new_static("delete me", 9);
    ClipiumEntry entry = {
        .id = 42, .content = content, .mime_type = "text/plain",
        .preview = "delete me", .hash = "hash42",
        .timestamp = g_get_real_time(), .pinned = FALSE, .size = 9,
    };
    clipium_db_save(db, &entry);

    gboolean ok = clipium_db_delete(db, 42);
    g_assert_true(ok);

    /* Verify it's gone */
    ClipiumStore *store = clipium_store_new(100);
    clipium_db_load_all(db, store);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(content);
    clipium_store_free(store);
    clipium_db_close(db);
    g_unlink(path);
}

static void
test_db_clear(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    GBytes *c1 = g_bytes_new_static("aaa", 3);
    GBytes *c2 = g_bytes_new_static("bbb", 3);
    ClipiumEntry e1 = { .id = 1, .content = c1, .mime_type = "text/plain",
                         .preview = "aaa", .hash = "h1",
                         .timestamp = 1, .pinned = FALSE, .size = 3 };
    ClipiumEntry e2 = { .id = 2, .content = c2, .mime_type = "text/plain",
                         .preview = "bbb", .hash = "h2",
                         .timestamp = 2, .pinned = FALSE, .size = 3 };
    clipium_db_save(db, &e1);
    clipium_db_save(db, &e2);

    gboolean ok = clipium_db_clear(db);
    g_assert_true(ok);

    ClipiumStore *store = clipium_store_new(100);
    clipium_db_load_all(db, store);
    g_assert_cmpuint(clipium_store_count(store), ==, 0);

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(c1);
    g_bytes_unref(c2);
    clipium_store_free(store);
    clipium_db_close(db);
    g_unlink(path);
}

static void
test_db_update_pin(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    GBytes *content = g_bytes_new_static("pin test", 8);
    ClipiumEntry entry = {
        .id = 10, .content = content, .mime_type = "text/plain",
        .preview = "pin test", .hash = "pinhash",
        .timestamp = g_get_real_time(), .pinned = FALSE, .size = 8,
    };
    clipium_db_save(db, &entry);

    gboolean ok = clipium_db_update_pin(db, 10, TRUE);
    g_assert_true(ok);

    /* Reload and verify pinned status */
    ClipiumStore *store = clipium_store_new(100);
    clipium_db_load_all(db, store);
    ClipiumEntry *loaded = clipium_store_get(store, 10);
    g_assert_nonnull(loaded);
    g_assert_true(loaded->pinned);

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(content);
    clipium_store_free(store);
    clipium_db_close(db);
    g_unlink(path);
}

static void
test_db_roundtrip_content(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    /* Test with binary-ish content */
    const char blob[] = "hello\x00world\x01\x02\x03";
    GBytes *content = g_bytes_new(blob, sizeof(blob));
    ClipiumEntry entry = {
        .id = 7, .content = content, .mime_type = "application/octet-stream",
        .preview = "[application/octet-stream 15B]", .hash = "blobhash",
        .timestamp = 1234567890, .pinned = TRUE, .size = sizeof(blob),
    };
    clipium_db_save(db, &entry);

    ClipiumStore *store = clipium_store_new(100);
    clipium_db_load_all(db, store);
    ClipiumEntry *loaded = clipium_store_get(store, 7);
    g_assert_nonnull(loaded);

    /* Verify binary content roundtrip */
    gsize loaded_len;
    const void *loaded_data = g_bytes_get_data(loaded->content, &loaded_len);
    g_assert_cmpuint(loaded_len, ==, sizeof(blob));
    g_assert_cmpmem(loaded_data, loaded_len, blob, sizeof(blob));
    g_assert_true(loaded->pinned);
    g_assert_cmpint(loaded->timestamp, ==, 1234567890);

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(content);
    clipium_store_free(store);
    clipium_db_close(db);
    g_unlink(path);
}

/* ======== Store + DB Integration ======== */

static void
test_integration_store_db_roundtrip(void)
{
    ClipiumDb *db = create_temp_db();
    g_assert_nonnull(db);

    /* Add entries via store, save via db, reload into new store */
    ClipiumStore *store1 = clipium_store_new(100);
    GBytes *c1 = g_bytes_new_static("first item", 10);
    GBytes *c2 = g_bytes_new_static("second item", 11);

    guint64 id1 = clipium_store_add(store1, c1, "text/plain");
    guint64 id2 = clipium_store_add(store1, c2, "text/plain");

    ClipiumEntry *e1 = clipium_store_get(store1, id1);
    ClipiumEntry *e2 = clipium_store_get(store1, id2);
    clipium_db_save(db, e1);
    clipium_db_save(db, e2);

    /* Load into a fresh store */
    ClipiumStore *store2 = clipium_store_new(100);
    clipium_db_load_all(db, store2);
    g_assert_cmpuint(clipium_store_count(store2), ==, 2);

    ClipiumEntry *loaded1 = clipium_store_get(store2, id1);
    ClipiumEntry *loaded2 = clipium_store_get(store2, id2);
    g_assert_nonnull(loaded1);
    g_assert_nonnull(loaded2);
    g_assert_cmpstr(loaded1->preview, ==, "first item");
    g_assert_cmpstr(loaded2->preview, ==, "second item");

    g_autofree char *path = g_strdup(db->path);
    g_bytes_unref(c1);
    g_bytes_unref(c2);
    clipium_store_free(store1);
    clipium_store_free(store2);
    clipium_db_close(db);
    g_unlink(path);
}

/* ======== Main ======== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Store tests */
    g_test_add_func("/store/new-free", test_store_new_free);
    g_test_add_func("/store/add-single", test_store_add_single);
    g_test_add_func("/store/add-empty-rejected", test_store_add_empty_rejected);
    g_test_add_func("/store/dedup", test_store_dedup);
    g_test_add_func("/store/dedup-bumps-to-top", test_store_dedup_bumps_to_top);
    g_test_add_func("/store/ordering", test_store_ordering);
    g_test_add_func("/store/eviction", test_store_eviction);
    g_test_add_func("/store/eviction-pinned", test_store_eviction_pinned);
    g_test_add_func("/store/delete", test_store_delete);
    g_test_add_func("/store/clear", test_store_clear);
    g_test_add_func("/store/pin", test_store_pin);
    g_test_add_func("/store/list-offset-limit", test_store_list_offset_limit);
    g_test_add_func("/store/search", test_store_search);

    /* Entry helper tests */
    g_test_add_func("/entry/compute-hash", test_entry_compute_hash);
    g_test_add_func("/entry/compute-hash-deterministic", test_entry_compute_hash_deterministic);
    g_test_add_func("/entry/make-preview-text", test_entry_make_preview_text);
    g_test_add_func("/entry/make-preview-binary", test_entry_make_preview_binary);
    g_test_add_func("/entry/make-preview-truncates", test_entry_make_preview_truncates);
    g_test_add_func("/entry/make-preview-trailing-whitespace", test_entry_make_preview_trailing_whitespace);

    /* Fuzzy match tests */
    g_test_add_func("/fuzzy/exact", test_fuzzy_match_exact);
    g_test_add_func("/fuzzy/substring", test_fuzzy_match_substring);
    g_test_add_func("/fuzzy/no-match", test_fuzzy_match_no_match);
    g_test_add_func("/fuzzy/case-insensitive", test_fuzzy_match_case_insensitive);
    g_test_add_func("/fuzzy/empty-query", test_fuzzy_match_empty_query);
    g_test_add_func("/fuzzy/empty-target", test_fuzzy_match_empty_target);
    g_test_add_func("/fuzzy/null-args", test_fuzzy_match_null_args);
    g_test_add_func("/fuzzy/scoring", test_fuzzy_match_scoring);
    g_test_add_func("/fuzzy/separator-bonus", test_fuzzy_match_separator_bonus);

    /* Database tests */
    g_test_add_func("/db/open-close", test_db_open_close);
    g_test_add_func("/db/save-and-load", test_db_save_and_load);
    g_test_add_func("/db/delete", test_db_delete);
    g_test_add_func("/db/clear", test_db_clear);
    g_test_add_func("/db/update-pin", test_db_update_pin);
    g_test_add_func("/db/roundtrip-content", test_db_roundtrip_content);

    /* Integration tests */
    g_test_add_func("/integration/store-db-roundtrip", test_integration_store_db_roundtrip);

    return g_test_run();
}
