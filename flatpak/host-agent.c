/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Long lived bridge between Kitty running inside Flatpak and a ptyxis-agent
 * running on the host.
 *
 * Unlike host-bridge.c, which relays bytes between two PTYs, this helper hands
 * the host PTY itself back to Kitty over SCM_RIGHTS. Kitty then drives the host
 * PTY directly, so there is no second PTY, no copy loop and no polling: job
 * control, window resizing and termios all work natively because the file
 * descriptor Kitty holds is a real PTY on the host.
 *
 * Kitty speaks a small SOCK_SEQPACKET protocol on the socket passed as
 * --socket-fd. Every datagram is a sequence of NUL terminated fields, the first
 * of which is the verb. Requests are answered in order, one reply per request:
 *
 *   spawn <provider> <name> <cwd> <argc> <argv...> <envc> <k=v...>
 *       + SCM_RIGHTS[exit_fd]        -> ok <token> + SCM_RIGHTS[pty]
 *   signal <token> <signum>          -> ok
 *   cwd <token> + SCM_RIGHTS[pty]    -> ok <path>
 *   foreground <token>
 *       + SCM_RIGHTS[pty]            -> ok <has_foreground> <pid> <cmdline> <kind>
 *   shell                            -> ok <path>
 *   containers                       -> ok <count> <provider> <id> <display>...
 *   release <token>                  -> ok
 *
 * Failures are reported as "err <message>". When a spawned process exits, its
 * raw wait status is written to the exit_fd that came with the spawn request;
 * Kitty gives that descriptor to a stub process (see --wait-fd below) so that
 * the host process still shows up as an ordinary reapable child.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define AGENT_PATH "/org/gnome/Ptyxis/Agent"
#define AGENT_IFACE "org.gnome.Ptyxis.Agent"
#define CONTAINER_IFACE "org.gnome.Ptyxis.Container"
#define PROCESS_IFACE "org.gnome.Ptyxis.Process"

#define MAX_MESSAGE (1024 * 1024)

typedef struct {
    char *object_path;
    int exit_fd;
    guint token;
} Process;

typedef struct {
    GDBusConnection *connection;
    GSubprocess *agent;
    GMainLoop *loop;
    GHashTable *by_token;   /* guint -> Process (owner) */
    GHashTable *by_path;    /* char* -> Process (borrowed) */
    GHashTable *containers; /* "provider\0name" -> object path */
    guint next_token;
    int socket_fd;
} Host;

static void
warn(const char *context, GError *error) {
    fprintf(stderr, "kitty-host-agent: %s: %s\n", context, error ? error->message : "unknown error");
}

/* ------------------------------------------------------------------ */
/* Message encoding                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *data;
    size_t length;
    size_t offset;
} Reader;

static const char *
read_field(Reader *reader) {
    const char *start = reader->data + reader->offset;
    const char *end;

    if (reader->offset >= reader->length) return NULL;
    end = memchr(start, 0, reader->length - reader->offset);
    if (!end) return NULL;
    reader->offset = (size_t)(end - reader->data) + 1;
    return start;
}

static gboolean
read_uint(Reader *reader, guint *out) {
    const char *field = read_field(reader);
    char *end = NULL;
    guint64 value;

    if (!field) return FALSE;
    errno = 0;
    value = g_ascii_strtoull(field, &end, 10);
    if (errno != 0 || !end || *end || value > G_MAXUINT) return FALSE;
    *out = (guint)value;
    return TRUE;
}

static void
add_field(GByteArray *out, const char *value) {
    if (!value) value = "";
    g_byte_array_append(out, (const guint8 *)value, (guint)strlen(value) + 1);
}

static void
add_int_field(GByteArray *out, gint64 value) {
    char buffer[32];
    g_snprintf(buffer, sizeof buffer, "%" G_GINT64_FORMAT, value);
    add_field(out, buffer);
}

/* ------------------------------------------------------------------ */
/* Socket I/O                                                          */
/* ------------------------------------------------------------------ */

static gboolean
send_message(int fd, GByteArray *message, int pass_fd) {
    struct msghdr header = {0};
    struct iovec iov = {.iov_base = message->data, .iov_len = message->len};
    union {
        struct cmsghdr align;
        char buffer[CMSG_SPACE(sizeof(int))];
    } control = {0};

    header.msg_iov = &iov;
    header.msg_iovlen = 1;
    if (pass_fd > -1) {
        struct cmsghdr *cmsg;
        header.msg_control = control.buffer;
        header.msg_controllen = sizeof control.buffer;
        cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));
    }
    for (;;) {
        if (sendmsg(fd, &header, MSG_NOSIGNAL) >= 0) return TRUE;
        if (errno == EINTR) continue;
        return FALSE;
    }
}

#define MAX_PASSED_FDS 2

/* Returns the number of bytes read, 0 on orderly shutdown, -1 on error. */
static ssize_t
receive_message(int fd, char *buffer, size_t capacity, int *fds, size_t *n_fds) {
    struct msghdr header = {0};
    struct iovec iov = {.iov_base = buffer, .iov_len = capacity};
    union {
        struct cmsghdr align;
        char buffer[CMSG_SPACE(sizeof(int) * MAX_PASSED_FDS)];
    } control = {0};
    struct cmsghdr *cmsg;
    ssize_t count;

    *n_fds = 0;
    header.msg_iov = &iov;
    header.msg_iovlen = 1;
    header.msg_control = control.buffer;
    header.msg_controllen = sizeof control.buffer;
    do { count = recvmsg(fd, &header, MSG_CMSG_CLOEXEC); } while (count < 0 && errno == EINTR);
    if (count <= 0) return count;

    for (cmsg = CMSG_FIRSTHDR(&header); cmsg; cmsg = CMSG_NXTHDR(&header, cmsg)) {
        size_t payload;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        payload = cmsg->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i * sizeof(int) < payload; i++) {
            int value;
            memcpy(&value, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (*n_fds < MAX_PASSED_FDS) fds[(*n_fds)++] = value;
            else close(value);
        }
    }
    if (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        for (size_t i = 0; i < *n_fds; i++) close(fds[i]);
        *n_fds = 0;
        errno = EMSGSIZE;
        return -1;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* ptyxis-agent connection                                             */
/* ------------------------------------------------------------------ */

static char *
find_agent_path(void) {
    g_autoptr(GKeyFile) info = g_key_file_new();
    g_autofree char *app_path = NULL;
    const char *override = g_getenv("KITTY_HOST_AGENT_PATH");

    if (override && *override) return g_strdup(override);
    if (g_key_file_load_from_file(info, "/.flatpak-info", G_KEY_FILE_NONE, NULL)) app_path = g_key_file_get_string(info, "Instance", "app-path", NULL);
    if (app_path && *app_path) return g_build_filename(app_path, "libexec", "ptyxis-agent", NULL);
    return g_strdup("/usr/libexec/ptyxis-agent");
}

static gboolean
connect_agent(Host *host, GError **error) {
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSocketConnection) stream = NULL;
    g_autoptr(GSocket) socket = NULL;
    g_autofree char *agent_path = find_agent_path();
    g_autofree char *guid = NULL;
    int pair[2] = {-1, -1};

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "socketpair: %s", g_strerror(errno));
        return FALSE;
    }

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_fd(launcher, pair[1], 3);
    pair[1] = -1;
    host->agent = g_subprocess_launcher_spawn(launcher, error, "flatpak-spawn", "--host", "--watch-bus", "--forward-fd=3", agent_path, "--socket-fd=3", NULL);
    if (!host->agent) {
        close(pair[0]);
        return FALSE;
    }

    socket = g_socket_new_from_fd(pair[0], error);
    if (!socket) {
        close(pair[0]);
        return FALSE;
    }
    pair[0] = -1;
    stream = g_socket_connection_factory_create_connection(socket);
    guid = g_dbus_generate_guid();
    host->connection = g_dbus_connection_new_sync(
        G_IO_STREAM(stream), guid, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS | G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, NULL, NULL, error);
    return host->connection != NULL;
}

static GVariant *
call_with_fds(
    Host *host,
    const char *path,
    const char *interface,
    const char *method,
    GVariant *parameters,
    const GVariantType *reply_type,
    GUnixFDList *in_fds,
    GUnixFDList **out_fds,
    GError **error) {
    return g_dbus_connection_call_with_unix_fd_list_sync(
        host->connection, NULL, path, interface, method, parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, -1, in_fds, out_fds, NULL, error);
}

static GVariant *
call(Host *host, const char *path, const char *interface, const char *method, GVariant *parameters, const GVariantType *reply_type, GError **error) {
    return g_dbus_connection_call_sync(host->connection, NULL, path, interface, method, parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
}

static const char *
lookup_container(Host *host, const char *provider, const char *name, GError **error) {
    g_autofree char *key = g_strdup_printf("%s\n%s", provider, name);
    g_autoptr(GVariant) reply = NULL;
    g_auto(GStrv) paths = NULL;
    const char *cached = g_hash_table_lookup(host->containers, key);

    if (cached) return cached;

    reply = call(host, AGENT_PATH, AGENT_IFACE, "ListContainers", NULL, G_VARIANT_TYPE("(ao)"), error);
    if (!reply) return NULL;
    g_variant_get(reply, "(^ao)", &paths);

    for (size_t i = 0; paths[i]; i++) {
        g_autoptr(GVariant) props_reply = NULL;
        g_autoptr(GVariant) props = NULL;
        const char *found_provider = NULL, *id = NULL, *display_name = NULL;

        props_reply = call(host, paths[i], "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", CONTAINER_IFACE), G_VARIANT_TYPE("(a{sv})"), NULL);
        if (!props_reply) continue;
        g_variant_get(props_reply, "(@a{sv})", &props);
        g_variant_lookup(props, "Provider", "&s", &found_provider);
        g_variant_lookup(props, "Id", "&s", &id);
        g_variant_lookup(props, "DisplayName", "&s", &display_name);

        if (g_strcmp0(found_provider, provider) == 0 && (g_strcmp0(id, name) == 0 || g_strcmp0(display_name, name) == 0)) {
            char *value = g_strdup(paths[i]);
            g_hash_table_insert(host->containers, g_steal_pointer(&key), value);
            return value;
        }
    }

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no %s container named %s", provider, name);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Process bookkeeping                                                 */
/* ------------------------------------------------------------------ */

static void
process_free(gpointer data) {
    Process *process = data;
    if (process->exit_fd > -1) close(process->exit_fd);
    g_free(process->object_path);
    g_free(process);
}

static void
report_exit(Host *host, Process *process, int status) {
    if (process->exit_fd > -1) {
        int32_t value = status;
        const char *buffer = (const char *)&value;
        size_t remaining = sizeof value;
        while (remaining) {
            ssize_t written = write(process->exit_fd, buffer, remaining);
            if (written > 0) {
                buffer += written;
                remaining -= (size_t)written;
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else break;
        }
        close(process->exit_fd);
        process->exit_fd = -1;
    }
    g_hash_table_remove(host->by_path, process->object_path);
    g_hash_table_remove(host->by_token, GUINT_TO_POINTER(process->token));
}

static void
on_process_exited(
    GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface,
    const char *signal_name,
    GVariant *parameters,
    gpointer user_data) {
    Host *host = user_data;
    const char *path = NULL;
    Process *process;
    int status = 0;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)signal_name;

    g_variant_get(parameters, "(&oi)", &path, &status);
    process = g_hash_table_lookup(host->by_path, path);
    if (process) report_exit(host, process, status);
}

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

static void
add_error(GByteArray *out, GError *error) {
    add_field(out, "err");
    add_field(out, error ? error->message : "unknown error");
}

static int
handle_spawn(Host *host, Reader *reader, int exit_fd, int stdin_fd, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) pty_fds = NULL;
    g_autoptr(GUnixFDList) spawn_fds = g_unix_fd_list_new();
    g_autoptr(GVariant) pty_reply = NULL;
    g_autoptr(GVariant) producer_reply = NULL;
    g_autoptr(GVariant) spawn_reply = NULL;
    g_autoptr(GUnixFDList) producer_fds = NULL;
    GVariantBuilder argv_builder, fd_builder, env_builder;
    const char *provider, *name, *cwd, *container_path;
    char *object_path = NULL;
    Process *process;
    guint count, i, has_stdin = 0;
    int consumer_fd = -1, producer_fd = -1, handle, stdin_handle;

    provider = read_field(reader);
    name = read_field(reader);
    cwd = read_field(reader);
    if (!provider || !name || !cwd || !read_uint(reader, &has_stdin)) {
        add_field(out, "err");
        add_field(out, "malformed spawn request");
        return -1;
    }
    if (has_stdin && stdin_fd < 0) {
        add_field(out, "err");
        add_field(out, "spawn request is missing its stdin descriptor");
        return -1;
    }

    container_path = lookup_container(host, provider, name, &error);
    if (!container_path) {
        add_error(out, error);
        return -1;
    }

    g_variant_builder_init(&argv_builder, G_VARIANT_TYPE("aay"));
    if (!read_uint(reader, &count)) {
        g_variant_builder_clear(&argv_builder);
        goto malformed;
    }
    for (i = 0; i < count; i++) {
        const char *argument = read_field(reader);
        if (!argument) {
            g_variant_builder_clear(&argv_builder);
            goto malformed;
        }
        g_variant_builder_add_value(&argv_builder, g_variant_new_bytestring(argument));
    }

    g_variant_builder_init(&env_builder, G_VARIANT_TYPE("a{ss}"));
    if (!read_uint(reader, &count)) {
        g_variant_builder_clear(&argv_builder);
        g_variant_builder_clear(&env_builder);
        goto malformed;
    }
    for (i = 0; i < count; i++) {
        const char *entry = read_field(reader);
        const char *equals = entry ? strchr(entry, '=') : NULL;
        g_autofree char *key = NULL;
        if (!equals) {
            g_variant_builder_clear(&argv_builder);
            g_variant_builder_clear(&env_builder);
            goto malformed;
        }
        key = g_strndup(entry, (size_t)(equals - entry));
        g_variant_builder_add(&env_builder, "{ss}", key, equals + 1);
    }

    /* The agent creates the PTY on the host so that the process it spawns can
     * make it its controlling terminal. Kitty keeps the consumer end. */
    pty_reply = call_with_fds(host, AGENT_PATH, AGENT_IFACE, "CreatePty", NULL, G_VARIANT_TYPE("(h)"), NULL, &pty_fds, &error);
    if (!pty_reply) {
        g_variant_builder_clear(&argv_builder);
        g_variant_builder_clear(&env_builder);
        add_error(out, error);
        return -1;
    }
    g_variant_get(pty_reply, "(h)", &handle);
    consumer_fd = g_unix_fd_list_get(pty_fds, handle, &error);
    if (consumer_fd < 0) {
        g_variant_builder_clear(&argv_builder);
        g_variant_builder_clear(&env_builder);
        add_error(out, error);
        return -1;
    }

    handle = g_unix_fd_list_append(spawn_fds, consumer_fd, &error);
    if (handle < 0) goto fd_error;
    producer_reply = call_with_fds(
        host, AGENT_PATH, AGENT_IFACE, "CreatePtyProducer", g_variant_new("(h)", handle), G_VARIANT_TYPE("(h)"), spawn_fds, &producer_fds, &error);
    if (!producer_reply) goto fd_error;
    g_variant_get(producer_reply, "(h)", &handle);
    producer_fd = g_unix_fd_list_get(producer_fds, handle, &error);
    if (producer_fd < 0) goto fd_error;

    g_clear_object(&spawn_fds);
    spawn_fds = g_unix_fd_list_new();
    handle = g_unix_fd_list_append(spawn_fds, producer_fd, &error);
    if (handle < 0) goto fd_error;
    stdin_handle = handle;
    if (has_stdin) {
        stdin_handle = g_unix_fd_list_append(spawn_fds, stdin_fd, &error);
        if (stdin_handle < 0) goto fd_error;
    }

    g_variant_builder_init(&fd_builder, G_VARIANT_TYPE("a{uh}"));
    g_variant_builder_add(&fd_builder, "{uh}", 0, stdin_handle);
    g_variant_builder_add(&fd_builder, "{uh}", 1, handle);
    g_variant_builder_add(&fd_builder, "{uh}", 2, handle);

    spawn_reply = call_with_fds(
        host,
        container_path,
        CONTAINER_IFACE,
        "Spawn",
        g_variant_new(
            "(@ay@aay@a{uh}@a{ss})",
            g_variant_new_bytestring(cwd),
            g_variant_builder_end(&argv_builder),
            g_variant_builder_end(&fd_builder),
            g_variant_builder_end(&env_builder)),
        G_VARIANT_TYPE("(o)"),
        spawn_fds,
        NULL,
        &error);
    close(producer_fd);
    producer_fd = -1;
    if (!spawn_reply) {
        close(consumer_fd);
        add_error(out, error);
        return -1;
    }
    g_variant_get(spawn_reply, "(o)", &object_path);

    process = g_new0(Process, 1);
    process->object_path = object_path;
    process->exit_fd = exit_fd;
    process->token = host->next_token++;
    g_hash_table_insert(host->by_token, GUINT_TO_POINTER(process->token), process);
    g_hash_table_insert(host->by_path, process->object_path, process);

    add_field(out, "ok");
    add_int_field(out, process->token);
    return consumer_fd;

malformed:
    add_field(out, "err");
    add_field(out, "malformed spawn request");
    return -1;

fd_error:
    g_variant_builder_clear(&argv_builder);
    g_variant_builder_clear(&env_builder);
    if (producer_fd > -1) close(producer_fd);
    if (consumer_fd > -1) close(consumer_fd);
    add_error(out, error);
    return -1;
}

static Process *
lookup_process(Host *host, Reader *reader) {
    guint token;
    if (!read_uint(reader, &token)) return NULL;
    return g_hash_table_lookup(host->by_token, GUINT_TO_POINTER(token));
}

static void
handle_signal_request(Host *host, Reader *reader, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = NULL;
    Process *process = lookup_process(host, reader);
    guint signum;

    if (!process) {
        add_field(out, "err");
        add_field(out, "unknown process");
        return;
    }
    if (!read_uint(reader, &signum)) {
        add_field(out, "err");
        add_field(out, "malformed signal request");
        return;
    }
    reply = call(host, process->object_path, PROCESS_IFACE, "SendSignal", g_variant_new("(i)", (gint32)signum), G_VARIANT_TYPE("()"), &error);
    if (!reply) {
        add_error(out, error);
        return;
    }
    add_field(out, "ok");
}

static void
handle_cwd(Host *host, Reader *reader, int pty_fd, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) in_fds = g_unix_fd_list_new();
    g_autoptr(GVariant) reply = NULL;
    g_autoptr(GVariant) path = NULL;
    Process *process = lookup_process(host, reader);
    int handle;

    if (!process || pty_fd < 0) {
        add_field(out, "err");
        add_field(out, "unknown process");
        return;
    }
    handle = g_unix_fd_list_append(in_fds, pty_fd, &error);
    if (handle < 0) {
        add_error(out, error);
        return;
    }
    reply = call_with_fds(
        host, process->object_path, PROCESS_IFACE, "GetWorkingDirectory", g_variant_new("(h)", handle), G_VARIANT_TYPE("(ay)"), in_fds, NULL, &error);
    if (!reply) {
        add_error(out, error);
        return;
    }
    path = g_variant_get_child_value(reply, 0);
    add_field(out, "ok");
    add_field(out, g_variant_get_bytestring(path));
}

static void
handle_foreground(Host *host, Reader *reader, int pty_fd, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) in_fds = g_unix_fd_list_new();
    g_autoptr(GVariant) reply = NULL;
    Process *process = lookup_process(host, reader);
    const char *cmdline = NULL, *leader_kind = NULL;
    gboolean has_foreground = FALSE;
    gint32 pid = -1;
    int handle;

    if (!process || pty_fd < 0) {
        add_field(out, "err");
        add_field(out, "unknown process");
        return;
    }
    handle = g_unix_fd_list_append(in_fds, pty_fd, &error);
    if (handle < 0) {
        add_error(out, error);
        return;
    }
    reply = call_with_fds(
        host, process->object_path, PROCESS_IFACE, "HasForegroundProcess", g_variant_new("(h)", handle), G_VARIANT_TYPE("(biss)"), in_fds, NULL, &error);
    if (!reply) {
        add_error(out, error);
        return;
    }
    g_variant_get(reply, "(bi&s&s)", &has_foreground, &pid, &cmdline, &leader_kind);
    add_field(out, "ok");
    add_int_field(out, has_foreground ? 1 : 0);
    add_int_field(out, pid);
    add_field(out, cmdline);
    add_field(out, leader_kind);
}

static void
handle_shell(Host *host, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = call(host, AGENT_PATH, AGENT_IFACE, "GetPreferredShell", NULL, G_VARIANT_TYPE("(ay)"), &error);
    g_autoptr(GVariant) value = NULL;

    if (!reply) {
        add_error(out, error);
        return;
    }
    value = g_variant_get_child_value(reply, 0);
    add_field(out, "ok");
    add_field(out, g_variant_get_bytestring(value));
}

static void
handle_containers(Host *host, GByteArray *out) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = NULL;
    g_auto(GStrv) paths = NULL;
    GByteArray *entries = g_byte_array_new();
    guint count = 0;

    reply = call(host, AGENT_PATH, AGENT_IFACE, "ListContainers", NULL, G_VARIANT_TYPE("(ao)"), &error);
    if (!reply) {
        g_byte_array_unref(entries);
        add_error(out, error);
        return;
    }
    g_variant_get(reply, "(^ao)", &paths);
    for (size_t i = 0; paths[i]; i++) {
        g_autoptr(GVariant) props_reply = NULL;
        g_autoptr(GVariant) props = NULL;
        const char *provider = NULL, *id = NULL, *display_name = NULL;

        props_reply = call(host, paths[i], "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", CONTAINER_IFACE), G_VARIANT_TYPE("(a{sv})"), NULL);
        if (!props_reply) continue;
        g_variant_get(props_reply, "(@a{sv})", &props);
        g_variant_lookup(props, "Provider", "&s", &provider);
        g_variant_lookup(props, "Id", "&s", &id);
        g_variant_lookup(props, "DisplayName", "&s", &display_name);
        add_field(entries, provider);
        add_field(entries, id);
        add_field(entries, display_name);
        count++;
    }
    add_field(out, "ok");
    add_int_field(out, count);
    g_byte_array_append(out, entries->data, entries->len);
    g_byte_array_unref(entries);
}

static void
handle_release(Host *host, Reader *reader, GByteArray *out) {
    Process *process = lookup_process(host, reader);
    if (process) {
        g_hash_table_remove(host->by_path, process->object_path);
        g_hash_table_remove(host->by_token, GUINT_TO_POINTER(process->token));
    }
    add_field(out, "ok");
}

static gboolean
on_socket_ready(gint fd, GIOCondition condition, gpointer user_data) {
    Host *host = user_data;
    g_autoptr(GByteArray) out = g_byte_array_new();
    static char buffer[MAX_MESSAGE];
    Reader reader;
    const char *verb;
    ssize_t count;
    int received[MAX_PASSED_FDS], pass_fd = -1;
    size_t n_received = 0;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        g_main_loop_quit(host->loop);
        return G_SOURCE_REMOVE;
    }

    count = receive_message(fd, buffer, sizeof buffer, received, &n_received);
    if (count == 0) {
        g_main_loop_quit(host->loop);
        return G_SOURCE_REMOVE;
    }
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return G_SOURCE_CONTINUE;
        warn("recvmsg", NULL);
        g_main_loop_quit(host->loop);
        return G_SOURCE_REMOVE;
    }

    reader.data = buffer;
    reader.length = (size_t)count;
    reader.offset = 0;
    verb = read_field(&reader);

    if (g_strcmp0(verb, "spawn") == 0) {
        int exit_fd = n_received > 0 ? received[0] : -1;
        int stdin_fd = n_received > 1 ? received[1] : -1;
        pass_fd = handle_spawn(host, &reader, exit_fd, stdin_fd, out);
        /* On success the exit descriptor is owned by the Process record. */
        if (pass_fd < 0 && exit_fd > -1) close(exit_fd);
        if (stdin_fd > -1) close(stdin_fd);
        n_received = 0;
    } else if (g_strcmp0(verb, "signal") == 0) {
        handle_signal_request(host, &reader, out);
    } else if (g_strcmp0(verb, "cwd") == 0) {
        handle_cwd(host, &reader, n_received > 0 ? received[0] : -1, out);
    } else if (g_strcmp0(verb, "foreground") == 0) {
        handle_foreground(host, &reader, n_received > 0 ? received[0] : -1, out);
    } else if (g_strcmp0(verb, "shell") == 0) {
        handle_shell(host, out);
    } else if (g_strcmp0(verb, "containers") == 0) {
        handle_containers(host, out);
    } else if (g_strcmp0(verb, "release") == 0) {
        handle_release(host, &reader, out);
    } else {
        add_field(out, "err");
        add_field(out, "unknown request");
    }

    for (size_t i = 0; i < n_received; i++) close(received[i]);
    if (!send_message(fd, out, pass_fd)) {
        warn("sendmsg", NULL);
        if (pass_fd > -1) close(pass_fd);
        g_main_loop_quit(host->loop);
        return G_SOURCE_REMOVE;
    }
    if (pass_fd > -1) close(pass_fd);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Lifetime stub                                                       */
/* ------------------------------------------------------------------ */

/*
 * Kitty spawns one of these per host process so that a host process still has
 * a locally reapable PID. It does nothing but block until the agent reports the
 * real wait status, then reproduces it.
 */
static int
run_wait_mode(int fd) {
    int32_t status = 0;
    char *buffer = (char *)&status;
    size_t got = 0;

    while (got < sizeof status) {
        ssize_t count = read(fd, buffer + got, sizeof status - got);
        if (count > 0) got += (size_t)count;
        else if (count == 0) break;
        else if (errno == EINTR) continue;
        else break;
    }
    if (got < sizeof status) return 1;
    if (WIFSIGNALED(status)) {
        int signum = WTERMSIG(status);
        signal(signum, SIG_DFL);
        raise(signum);
        return 128 + signum;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

/* ------------------------------------------------------------------ */

static gboolean
parse_fd_option(const char *argument, const char *name, int *out) {
    size_t length = strlen(name);
    char *end = NULL;
    long value;

    if (strncmp(argument, name, length) != 0 || argument[length] != '=') return FALSE;
    errno = 0;
    value = strtol(argument + length + 1, &end, 10);
    if (errno != 0 || !end || *end || value < 0 || value > INT_MAX) return FALSE;
    *out = (int)value;
    return TRUE;
}

int
main(int argc, char **argv) {
    Host host = {0};
    g_autoptr(GError) error = NULL;
    int socket_fd = -1, wait_fd = -1;

    for (int i = 1; i < argc; i++) {
        if (parse_fd_option(argv[i], "--socket-fd", &socket_fd)) continue;
        if (parse_fd_option(argv[i], "--wait-fd", &wait_fd)) continue;
        fprintf(stderr, "usage: %s --socket-fd=FD | --wait-fd=FD\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (wait_fd > -1) return run_wait_mode(wait_fd);
    if (socket_fd < 0) {
        fprintf(stderr, "usage: %s --socket-fd=FD | --wait-fd=FD\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    host.socket_fd = socket_fd;
    host.next_token = 1;
    host.loop = g_main_loop_new(NULL, FALSE);
    host.by_token = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, process_free);
    host.by_path = g_hash_table_new(g_str_hash, g_str_equal);
    host.containers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    if (!connect_agent(&host, &error)) {
        warn("cannot start host agent", error);
        return EXIT_FAILURE;
    }

    g_dbus_connection_signal_subscribe(
        host.connection, NULL, AGENT_IFACE, "ProcessExited", AGENT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_process_exited, &host, NULL);

    g_unix_fd_add(socket_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_socket_ready, &host);
    g_main_loop_run(host.loop);

    g_hash_table_unref(host.by_token);
    g_hash_table_unref(host.by_path);
    g_hash_table_unref(host.containers);
    g_clear_object(&host.connection);
    if (host.agent) {
        g_subprocess_force_exit(host.agent);
        g_clear_object(&host.agent);
    }
    g_main_loop_unref(host.loop);
    return EXIT_SUCCESS;
}
