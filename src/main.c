#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "clipium-app.h"
#include "clipium-config.h"
#include "clipium-ipc.h"

/* --- _ingest mode: read stdin, send to daemon via IPC --- */

static int
do_ingest(void)
{
    /* Read all of stdin */
    GString *buf = g_string_new(NULL);
    char tmp[4096];
    ssize_t n;

    while ((n = read(STDIN_FILENO, tmp, sizeof(tmp))) > 0)
        g_string_append_len(buf, tmp, n);

    if (buf->len == 0) {
        g_string_free(buf, TRUE);
        return 0;
    }

    /* Trim trailing newline (wl-paste adds one) */
    if (buf->len > 0 && buf->str[buf->len - 1] == '\n')
        g_string_truncate(buf, buf->len - 1);

    if (buf->len == 0) {
        g_string_free(buf, TRUE);
        return 0;
    }

    /* Base64 encode */
    g_autofree char *b64 = g_base64_encode((const guchar *)buf->str, buf->len);
    g_string_free(buf, TRUE);

    /* Build JSON command */
    g_autofree char *cmd = g_strdup_printf(
        "{\"cmd\":\"ingest\",\"content\":\"%s\",\"mime\":\"text/plain\"}", b64);

    /* Send to daemon */
    g_autofree char *sock = clipium_socket_path();
    g_autofree char *resp = clipium_ipc_send_command(sock, cmd);

    if (!resp) {
        g_printerr("clipium: daemon not running\n");
        return 1;
    }

    return 0;
}

/* --- CLI command helpers --- */

static int
do_cli_command(const char *json_cmd)
{
    g_autofree char *sock = clipium_socket_path();
    g_autofree char *resp = clipium_ipc_send_command(sock, json_cmd);

    if (!resp) {
        g_printerr("clipium: daemon not running (socket: %s)\n", sock);
        return 1;
    }

    printf("%s\n", resp);
    return 0;
}

static int
do_show(void)
{
    return do_cli_command("{\"cmd\":\"show\"}");
}

static int
do_list(int argc, char **argv)
{
    int limit = 50;
    if (argc > 2)
        limit = atoi(argv[2]);

    g_autofree char *cmd = g_strdup_printf("{\"cmd\":\"list\",\"limit\":%d}", limit);
    return do_cli_command(cmd);
}

static int
do_search(int argc, char **argv)
{
    if (argc < 3) {
        g_printerr("Usage: clipium search <query>\n");
        return 1;
    }

    /* Escape the query for JSON */
    GString *escaped = g_string_new(NULL);
    for (const char *p = argv[2]; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(escaped, '\\');
        g_string_append_c(escaped, *p);
    }

    g_autofree char *cmd = g_strdup_printf(
        "{\"cmd\":\"search\",\"query\":\"%s\"}", escaped->str);
    g_string_free(escaped, TRUE);

    return do_cli_command(cmd);
}

static int
do_delete(int argc, char **argv)
{
    if (argc < 3) {
        g_printerr("Usage: clipium delete <id>\n");
        return 1;
    }

    long id = atol(argv[2]);
    g_autofree char *cmd = g_strdup_printf("{\"cmd\":\"delete\",\"id\":%ld}", id);
    return do_cli_command(cmd);
}

static int
do_clear(void)
{
    return do_cli_command("{\"cmd\":\"clear\"}");
}

static int
do_status(void)
{
    return do_cli_command("{\"cmd\":\"status\"}");
}

/* --- Print usage --- */

static void
print_usage(void)
{
    g_print(
        "Clipium %s — Clipboard manager for Wayland/GNOME\n"
        "\n"
        "Usage:\n"
        "  clipium                Start daemon (or activate existing)\n"
        "  clipium show           Show clipboard popup\n"
        "  clipium list [N]       List last N entries (default 50)\n"
        "  clipium search <q>     Fuzzy search entries\n"
        "  clipium delete <id>    Delete entry by ID\n"
        "  clipium clear          Clear all entries\n"
        "  clipium status         Show daemon status\n"
        "  clipium _ingest        (internal) Ingest clipboard from stdin\n"
        "  clipium --version      Show version\n"
        "  clipium --help         Show this help\n",
        CLIPIUM_VERSION);
}

/* --- Main --- */

int
main(int argc, char **argv)
{
    /* Check for special modes first */
    if (argc >= 2) {
        if (g_str_equal(argv[1], "--version") || g_str_equal(argv[1], "-v")) {
            g_print("clipium %s\n", CLIPIUM_VERSION);
            return 0;
        }
        if (g_str_equal(argv[1], "--help") || g_str_equal(argv[1], "-h")) {
            print_usage();
            return 0;
        }
        if (g_str_equal(argv[1], "_ingest"))
            return do_ingest();
        if (g_str_equal(argv[1], "show"))
            return do_show();
        if (g_str_equal(argv[1], "list"))
            return do_list(argc, argv);
        if (g_str_equal(argv[1], "search"))
            return do_search(argc, argv);
        if (g_str_equal(argv[1], "delete"))
            return do_delete(argc, argv);
        if (g_str_equal(argv[1], "clear"))
            return do_clear();
        if (g_str_equal(argv[1], "status"))
            return do_status();

        /* Unknown command — might be GTK args, fall through to daemon mode */
        if (argv[1][0] == '-' && !g_str_has_prefix(argv[1], "--")) {
            g_printerr("Unknown option: %s\n", argv[1]);
            print_usage();
            return 1;
        }
    }

    /* Daemon mode: start the GTK application */
    ClipiumApp *app = clipium_app_new();
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
