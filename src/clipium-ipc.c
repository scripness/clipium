#include "clipium-ipc.h"
#include "clipium-config.h"
#include <string.h>
/* Minimal hand-rolled JSON to avoid json-glib dependency */

struct _ClipiumIpc {
    GSocketService         *service;
    ClipiumStore           *store;
    ClipiumDb              *db;
    ClipiumIpcShowCallback  show_cb;
    gpointer                show_cb_data;
    char                   *socket_path;
};

/* --- Minimal JSON helpers --- */

static char *
json_escape_string(const char *s)
{
    if (!s) return g_strdup("null");
    GString *out = g_string_new("\"");
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20)
                    g_string_append_printf(out, "\\u%04x", (unsigned)*p);
                else
                    g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* Extract a string value for a key from JSON (very simple parser) */
static char *
json_get_string(const char *json, const char *key)
{
    g_autofree char *pattern = g_strdup_printf("\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    /* Skip whitespace and colon */
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;

    if (*pos == '"') {
        pos++;
        GString *val = g_string_new(NULL);
        while (*pos && *pos != '"') {
            if (*pos == '\\' && *(pos + 1)) {
                pos++;
                switch (*pos) {
                    case 'n': g_string_append_c(val, '\n'); break;
                    case 'r': g_string_append_c(val, '\r'); break;
                    case 't': g_string_append_c(val, '\t'); break;
                    case '"': g_string_append_c(val, '"'); break;
                    case '\\': g_string_append_c(val, '\\'); break;
                    default: g_string_append_c(val, *pos); break;
                }
            } else {
                g_string_append_c(val, *pos);
            }
            pos++;
        }
        return g_string_free(val, FALSE);
    }
    if (strncmp(pos, "null", 4) == 0)
        return NULL;

    return NULL;
}

static gint64
json_get_int(const char *json, const char *key, gint64 default_val)
{
    g_autofree char *pattern = g_strdup_printf("\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return default_val;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;

    if (*pos == '-' || (*pos >= '0' && *pos <= '9'))
        return g_ascii_strtoll(pos, NULL, 10);
    if (strncmp(pos, "true", 4) == 0)
        return 1;
    if (strncmp(pos, "false", 5) == 0)
        return 0;

    return default_val;
}

/* --- Format time ago --- */

static char *
format_time_ago(gint64 timestamp)
{
    gint64 now = g_get_real_time();
    gint64 diff = (now - timestamp) / G_USEC_PER_SEC;

    if (diff < 0) return g_strdup("now");
    if (diff < 5) return g_strdup("now");
    if (diff < 60) return g_strdup_printf("%" G_GINT64_FORMAT "s", diff);
    if (diff < 3600) return g_strdup_printf("%" G_GINT64_FORMAT "m", diff / 60);
    if (diff < 86400) return g_strdup_printf("%" G_GINT64_FORMAT "h", diff / 3600);
    return g_strdup_printf("%" G_GINT64_FORMAT "d", diff / 86400);
}

/* --- Entry to JSON --- */

static char *
entry_to_json(const ClipiumEntry *e)
{
    g_autofree char *preview_escaped = json_escape_string(e->preview);
    g_autofree char *mime_escaped = json_escape_string(e->mime_type);
    g_autofree char *hash_escaped = json_escape_string(e->hash);
    g_autofree char *time_ago = format_time_ago(e->timestamp);
    g_autofree char *time_escaped = json_escape_string(time_ago);

    /* Base64-encode content */
    gsize content_len;
    const guchar *content_data = g_bytes_get_data(e->content, &content_len);
    g_autofree char *content_b64 = g_base64_encode(content_data, content_len);
    g_autofree char *content_escaped = json_escape_string(content_b64);

    return g_strdup_printf(
        "{\"id\":%" G_GUINT64_FORMAT ",\"preview\":%s,\"mime\":%s,\"hash\":%s,"
        "\"timestamp\":%" G_GINT64_FORMAT ",\"pinned\":%s,\"size\":%" G_GSIZE_FORMAT ","
        "\"time_ago\":%s,\"content\":%s}",
        e->id, preview_escaped, mime_escaped, hash_escaped,
        e->timestamp, e->pinned ? "true" : "false", e->size,
        time_escaped, content_escaped);
}

/* --- Command handler --- */

static char *
handle_command(ClipiumIpc *ipc, const char *json_str)
{
    g_autofree char *cmd = json_get_string(json_str, "cmd");
    if (!cmd)
        return g_strdup("{\"ok\":false,\"error\":\"missing cmd\"}");

    if (g_str_equal(cmd, "ingest")) {
        g_autofree char *content_b64 = json_get_string(json_str, "content");
        g_autofree char *mime = json_get_string(json_str, "mime");

        if (!content_b64 || !mime)
            return g_strdup("{\"ok\":false,\"error\":\"missing content or mime\"}");

        gsize decoded_len;
        guchar *decoded = g_base64_decode(content_b64, &decoded_len);
        if (!decoded || decoded_len == 0) {
            g_free(decoded);
            return g_strdup("{\"ok\":false,\"error\":\"empty content\"}");
        }

        GBytes *content = g_bytes_new_take(decoded, decoded_len);
        guint64 new_id = clipium_store_add(ipc->store, content, mime);

        if (new_id > 0 && ipc->db) {
            ClipiumEntry *entry = clipium_store_get(ipc->store, new_id);
            if (entry)
                clipium_db_save_async(ipc->db, entry);
        }

        g_bytes_unref(content);
        return g_strdup_printf("{\"ok\":true,\"id\":%" G_GUINT64_FORMAT "}", new_id);
    }

    if (g_str_equal(cmd, "list")) {
        gint64 limit = json_get_int(json_str, "limit", 50);
        gint64 offset = json_get_int(json_str, "offset", 0);

        GArray *entries = clipium_store_list(ipc->store, (guint)limit, (guint)offset);
        GString *json = g_string_new("{\"ok\":true,\"count\":");
        g_string_append_printf(json, "%u,\"entries\":[", entries->len);

        for (guint i = 0; i < entries->len; i++) {
            if (i > 0) g_string_append_c(json, ',');
            ClipiumEntry *e = g_array_index(entries, ClipiumEntry *, i);
            g_autofree char *ej = entry_to_json(e);
            g_string_append(json, ej);
        }

        g_string_append(json, "]}");
        g_array_free(entries, TRUE);
        return g_string_free(json, FALSE);
    }

    if (g_str_equal(cmd, "search")) {
        g_autofree char *query = json_get_string(json_str, "query");
        gint64 limit = json_get_int(json_str, "limit", 50);

        if (!query)
            return g_strdup("{\"ok\":false,\"error\":\"missing query\"}");

        GArray *entries = clipium_store_search(ipc->store, query, (guint)limit);
        GString *json = g_string_new("{\"ok\":true,\"count\":");
        g_string_append_printf(json, "%u,\"entries\":[", entries->len);

        for (guint i = 0; i < entries->len; i++) {
            if (i > 0) g_string_append_c(json, ',');
            ClipiumEntry *e = g_array_index(entries, ClipiumEntry *, i);
            g_autofree char *ej = entry_to_json(e);
            g_string_append(json, ej);
        }

        g_string_append(json, "]}");
        g_array_free(entries, TRUE);
        return g_string_free(json, FALSE);
    }

    if (g_str_equal(cmd, "delete")) {
        gint64 id = json_get_int(json_str, "id", -1);
        if (id < 0)
            return g_strdup("{\"ok\":false,\"error\":\"missing id\"}");

        gboolean ok = clipium_store_delete(ipc->store, (guint64)id);
        if (ok && ipc->db)
            clipium_db_delete(ipc->db, (guint64)id);

        return g_strdup_printf("{\"ok\":%s}", ok ? "true" : "false");
    }

    if (g_str_equal(cmd, "clear")) {
        clipium_store_clear(ipc->store);
        if (ipc->db)
            clipium_db_clear(ipc->db);
        return g_strdup("{\"ok\":true}");
    }

    if (g_str_equal(cmd, "show")) {
        if (ipc->show_cb)
            ipc->show_cb(ipc->show_cb_data);
        return g_strdup("{\"ok\":true}");
    }

    if (g_str_equal(cmd, "status")) {
        guint count = clipium_store_count(ipc->store);
        return g_strdup_printf(
            "{\"ok\":true,\"entries\":%u,\"max_entries\":%u,\"version\":\"%s\"}",
            count, CLIPIUM_MAX_ENTRIES, CLIPIUM_VERSION);
    }

    if (g_str_equal(cmd, "pin")) {
        gint64 id = json_get_int(json_str, "id", -1);
        gint64 pinned = json_get_int(json_str, "pinned", 1);
        if (id < 0)
            return g_strdup("{\"ok\":false,\"error\":\"missing id\"}");

        gboolean ok = clipium_store_pin(ipc->store, (guint64)id, (gboolean)pinned);
        if (ok && ipc->db)
            clipium_db_update_pin(ipc->db, (guint64)id, (gboolean)pinned);

        return g_strdup_printf("{\"ok\":%s}", ok ? "true" : "false");
    }

    return g_strdup("{\"ok\":false,\"error\":\"unknown command\"}");
}

/* --- Send response with length prefix --- */

static gboolean
send_response(GOutputStream *out, const char *json)
{
    gsize len = strlen(json);
    guchar hdr[4] = {
        (guchar)((len >> 24) & 0xFF),
        (guchar)((len >> 16) & 0xFF),
        (guchar)((len >>  8) & 0xFF),
        (guchar)((len      ) & 0xFF),
    };

    GError *err = NULL;
    if (!g_output_stream_write_all(out, hdr, 4, NULL, NULL, &err)) {
        g_warning("Failed to write header: %s", err->message);
        g_error_free(err);
        return FALSE;
    }
    if (!g_output_stream_write_all(out, json, len, NULL, NULL, &err)) {
        g_warning("Failed to write body: %s", err->message);
        g_error_free(err);
        return FALSE;
    }
    return TRUE;
}

/* --- Connection handler --- */

static gboolean
on_incoming(GSocketService    *service,
            GSocketConnection *connection,
            GObject           *source_object,
            gpointer           user_data)
{
    (void)service;
    (void)source_object;
    ClipiumIpc *ipc = user_data;

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    /* Read 4-byte header */
    guchar hdr[4];
    gsize bytes_read;
    GError *err = NULL;

    if (!g_input_stream_read_all(in, hdr, 4, &bytes_read, NULL, &err) || bytes_read != 4) {
        g_clear_error(&err);
        return TRUE;
    }

    guint32 msg_len = ((guint32)hdr[0] << 24) | ((guint32)hdr[1] << 16) |
                      ((guint32)hdr[2] << 8)  | (guint32)hdr[3];

    if (msg_len > CLIPIUM_IPC_MAX_MSG) {
        send_response(out, "{\"ok\":false,\"error\":\"message too large\"}");
        return TRUE;
    }

    char *buf = g_malloc(msg_len + 1);
    if (!g_input_stream_read_all(in, buf, msg_len, &bytes_read, NULL, &err) ||
        bytes_read != msg_len) {
        g_free(buf);
        g_clear_error(&err);
        return TRUE;
    }
    buf[msg_len] = '\0';

    g_autofree char *response = handle_command(ipc, buf);
    g_free(buf);

    send_response(out, response);
    return FALSE;  /* Let the service close the connection */
}

/* --- Public API --- */

ClipiumIpc *
clipium_ipc_server_start(const char             *socket_path,
                          ClipiumStore           *store,
                          ClipiumDb              *db,
                          ClipiumIpcShowCallback  show_cb,
                          gpointer                show_cb_data)
{
    /* Remove stale socket */
    if (g_file_test(socket_path, G_FILE_TEST_EXISTS))
        g_unlink(socket_path);

    GError *err = NULL;
    GSocketAddress *addr = g_unix_socket_address_new(socket_path);
    GSocketService *service = g_threaded_socket_service_new(4);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),
                                        addr, G_SOCKET_TYPE_STREAM,
                                        G_SOCKET_PROTOCOL_DEFAULT,
                                        NULL, NULL, &err)) {
        g_warning("Failed to bind IPC socket: %s", err->message);
        g_error_free(err);
        g_object_unref(addr);
        g_object_unref(service);
        return NULL;
    }
    g_object_unref(addr);

    ClipiumIpc *ipc = g_new0(ClipiumIpc, 1);
    ipc->service = service;
    ipc->store = store;
    ipc->db = db;
    ipc->show_cb = show_cb;
    ipc->show_cb_data = show_cb_data;
    ipc->socket_path = g_strdup(socket_path);

    g_signal_connect(service, "run", G_CALLBACK(on_incoming), ipc);
    g_socket_service_start(service);

    g_message("IPC server listening on %s", socket_path);
    return ipc;
}

void
clipium_ipc_server_stop(ClipiumIpc *ipc)
{
    if (!ipc) return;
    g_socket_service_stop(ipc->service);
    g_object_unref(ipc->service);
    g_unlink(ipc->socket_path);
    g_free(ipc->socket_path);
    g_free(ipc);
}

/* --- Client --- */

char *
clipium_ipc_send_command(const char *socket_path, const char *json_cmd)
{
    GError *err = NULL;

    GSocketAddress *addr = g_unix_socket_address_new(socket_path);
    GSocketClient *client = g_socket_client_new();
    GSocketConnection *conn = g_socket_client_connect(
        client, G_SOCKET_CONNECTABLE(addr), NULL, &err);
    g_object_unref(addr);
    g_object_unref(client);

    if (!conn) {
        g_printerr("Failed to connect to daemon: %s\n", err->message);
        g_error_free(err);
        return NULL;
    }

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));

    /* Send length-prefixed message */
    gsize len = strlen(json_cmd);
    guchar hdr[4] = {
        (guchar)((len >> 24) & 0xFF),
        (guchar)((len >> 16) & 0xFF),
        (guchar)((len >>  8) & 0xFF),
        (guchar)((len      ) & 0xFF),
    };

    g_output_stream_write_all(out, hdr, 4, NULL, NULL, NULL);
    g_output_stream_write_all(out, json_cmd, len, NULL, NULL, NULL);

    /* Read response */
    guchar resp_hdr[4];
    gsize bytes_read;
    if (!g_input_stream_read_all(in, resp_hdr, 4, &bytes_read, NULL, NULL) || bytes_read != 4) {
        g_object_unref(conn);
        return NULL;
    }

    guint32 resp_len = ((guint32)resp_hdr[0] << 24) | ((guint32)resp_hdr[1] << 16) |
                       ((guint32)resp_hdr[2] << 8)  | (guint32)resp_hdr[3];

    if (resp_len > CLIPIUM_IPC_MAX_MSG) {
        g_object_unref(conn);
        return NULL;
    }

    char *resp = g_malloc(resp_len + 1);
    if (!g_input_stream_read_all(in, resp, resp_len, &bytes_read, NULL, NULL) ||
        bytes_read != resp_len) {
        g_free(resp);
        g_object_unref(conn);
        return NULL;
    }
    resp[resp_len] = '\0';

    g_object_unref(conn);
    return resp;
}
