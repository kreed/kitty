/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bridge a Kitty PTY to a host/container PTY created by ptyxis-agent.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define AGENT_PATH "/org/gnome/Ptyxis/Agent"
#define AGENT_IFACE "org.gnome.Ptyxis.Agent"
#define CONTAINER_IFACE "org.gnome.Ptyxis.Container"
#define PROCESS_IFACE "org.gnome.Ptyxis.Process"

typedef struct {
    GDBusConnection *connection;
    GSubprocess *agent;
    char *process_path;
    int wait_status;
    gboolean exited;
} Bridge;

static volatile sig_atomic_t resize_pending = 1;
static volatile sig_atomic_t signal_pending = 0;

static void
handle_signal(int signum) {
    if (signum == SIGWINCH) resize_pending = 1;
    else signal_pending = signum;
}

static void
print_error(const char *context, GError *error) {
    fprintf(stderr, "kitty-chrome-host-pty: %s: %s\n", context, error ? error->message : "unknown error");
}

static char *
find_agent_path(void) {
    g_autoptr(GKeyFile) info = g_key_file_new();
    g_autofree char *app_path = NULL;
    const char *override = g_getenv("KITTY_CHROME_AGENT");

    if (override && *override) return g_strdup(override);
    if (g_key_file_load_from_file(info, "/.flatpak-info", G_KEY_FILE_NONE, NULL)) app_path = g_key_file_get_string(info, "Instance", "app-path", NULL);
    if (app_path && *app_path) return g_build_filename(app_path, "libexec", "ptyxis-agent", NULL);
    return g_strdup("/usr/libexec/ptyxis-agent");
}

static gboolean
connect_agent(Bridge *bridge, GError **error) {
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
    bridge->agent = g_subprocess_launcher_spawn(launcher, error, "flatpak-spawn", "--host", "--watch-bus", "--forward-fd=3", agent_path, "--socket-fd=3", NULL);
    if (!bridge->agent) {
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
    bridge->connection = g_dbus_connection_new_sync(
        G_IO_STREAM(stream), guid, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS | G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, NULL, NULL, error);
    return bridge->connection != NULL;
}

static GVariant *
call_sync(Bridge *bridge, const char *path, const char *interface, const char *method, GVariant *parameters, const GVariantType *reply_type, GError **error) {
    return g_dbus_connection_call_sync(bridge->connection, NULL, path, interface, method, parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
}

static GVariant *
call_with_fds(
    Bridge *bridge,
    const char *path,
    const char *interface,
    const char *method,
    GVariant *parameters,
    const GVariantType *reply_type,
    GUnixFDList *in_fds,
    GUnixFDList **out_fds,
    GError **error) {
    return g_dbus_connection_call_with_unix_fd_list_sync(
        bridge->connection, NULL, path, interface, method, parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, -1, in_fds, out_fds, NULL, error);
}

static char *
find_container(Bridge *bridge, const char *wanted_provider, const char *wanted_name, GError **error) {
    g_autoptr(GVariant) reply = NULL;
    g_auto(GStrv) paths = NULL;

    reply = call_sync(bridge, AGENT_PATH, AGENT_IFACE, "ListContainers", NULL, G_VARIANT_TYPE("(ao)"), error);
    if (!reply) return NULL;
    g_variant_get(reply, "(^ao)", &paths);

    for (size_t i = 0; paths[i]; i++) {
        g_autoptr(GVariant) props_reply = NULL;
        g_autoptr(GVariant) props = NULL;
        const char *provider = NULL, *id = NULL, *display_name = NULL;

        props_reply =
            call_sync(bridge, paths[i], "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", CONTAINER_IFACE), G_VARIANT_TYPE("(a{sv})"), NULL);
        if (!props_reply) continue;
        g_variant_get(props_reply, "(@a{sv})", &props);
        g_variant_lookup(props, "Provider", "&s", &provider);
        g_variant_lookup(props, "Id", "&s", &id);
        g_variant_lookup(props, "DisplayName", "&s", &display_name);

        if (g_strcmp0(provider, wanted_provider) == 0 && (g_strcmp0(id, wanted_name) == 0 || g_strcmp0(display_name, wanted_name) == 0))
            return g_strdup(paths[i]);
    }

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No %s container named %s", wanted_provider, wanted_name);
    return NULL;
}

static int
get_fd_from_reply(GVariant *reply, GUnixFDList *fds, GError **error) {
    int handle = -1;
    g_variant_get(reply, "(h)", &handle);
    return g_unix_fd_list_get(fds, handle, error);
}

static int
create_pty(Bridge *bridge, GError **error) {
    g_autoptr(GUnixFDList) out_fds = NULL;
    g_autoptr(GVariant) reply = call_with_fds(bridge, AGENT_PATH, AGENT_IFACE, "CreatePty", NULL, G_VARIANT_TYPE("(h)"), NULL, &out_fds, error);
    return reply ? get_fd_from_reply(reply, out_fds, error) : -1;
}

static int
create_pty_producer(Bridge *bridge, int consumer_fd, GError **error) {
    g_autoptr(GUnixFDList) in_fds = g_unix_fd_list_new();
    g_autoptr(GUnixFDList) out_fds = NULL;
    g_autoptr(GVariant) reply = NULL;
    int handle = g_unix_fd_list_append(in_fds, consumer_fd, error);
    if (handle < 0) return -1;

    reply = call_with_fds(bridge, AGENT_PATH, AGENT_IFACE, "CreatePtyProducer", g_variant_new("(h)", handle), G_VARIANT_TYPE("(h)"), in_fds, &out_fds, error);
    return reply ? get_fd_from_reply(reply, out_fds, error) : -1;
}

static char *
preferred_shell(Bridge *bridge) {
    g_autoptr(GVariant) reply = call_sync(bridge, AGENT_PATH, AGENT_IFACE, "GetPreferredShell", NULL, G_VARIANT_TYPE("(ay)"), NULL);
    if (reply) {
        g_autoptr(GVariant) value = g_variant_get_child_value(reply, 0);
        const char *shell = g_variant_get_bytestring(value);
        if (shell && *shell) return g_strdup(shell);
    }
    return g_strdup("/bin/sh");
}

static void
add_environment(GVariantBuilder *builder) {
    const char *names[] = {"TERM", "COLORTERM", "LANG", "LC_ALL", "LC_CTYPE", "SSH_AUTH_SOCK", "SSH_AGENT_PID", NULL};
    for (size_t i = 0; names[i]; i++) {
        const char *value = g_getenv(names[i]);
        if (value) g_variant_builder_add(builder, "{ss}", names[i], value);
    }
}

static char *
spawn_process(Bridge *bridge, const char *container_path, int producer_fd, char **argv, GError **error) {
    g_autoptr(GUnixFDList) in_fds = g_unix_fd_list_new();
    g_autoptr(GVariant) reply = NULL;
    GVariantBuilder argv_builder, fd_builder, env_builder;
    g_autofree char *cwd = g_get_current_dir();
    char *process_path = NULL;
    int handle = g_unix_fd_list_append(in_fds, producer_fd, error);
    if (handle < 0) return NULL;

    g_variant_builder_init(&argv_builder, G_VARIANT_TYPE("aay"));
    for (size_t i = 0; argv[i]; i++) g_variant_builder_add_value(&argv_builder, g_variant_new_bytestring(argv[i]));

    g_variant_builder_init(&fd_builder, G_VARIANT_TYPE("a{uh}"));
    g_variant_builder_add(&fd_builder, "{uh}", 0, handle);
    g_variant_builder_add(&fd_builder, "{uh}", 1, handle);
    g_variant_builder_add(&fd_builder, "{uh}", 2, handle);

    g_variant_builder_init(&env_builder, G_VARIANT_TYPE("a{ss}"));
    add_environment(&env_builder);

    reply = call_with_fds(
        bridge,
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
        in_fds,
        NULL,
        error);
    if (!reply) return NULL;
    g_variant_get(reply, "(o)", &process_path);
    return process_path;
}

static void
process_exited(
    GDBusConnection *connection,
    const char *sender_name,
    const char *object_path,
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gpointer user_data) {
    Bridge *bridge = user_data;
    const char *path = NULL;
    int status = 0;
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    g_variant_get(parameters, "(&oi)", &path, &status);
    if (bridge->process_path && g_strcmp0(path, bridge->process_path) == 0) {
        bridge->wait_status = status;
        bridge->exited = TRUE;
    }
}

static void
resize_remote_pty(int remote_fd) {
    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) ioctl(remote_fd, TIOCSWINSZ, &size);
}

static gboolean
write_all(int fd, const char *buffer, ssize_t length) {
    while (length > 0) {
        ssize_t written = write(fd, buffer, (size_t)length);
        if (written > 0) {
            buffer += written;
            length -= written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return FALSE;
        }
    }
    return TRUE;
}

static void
send_remote_signal(Bridge *bridge, int signum) {
    if (!bridge->process_path) return;
    g_autoptr(GVariant) ignored =
        call_sync(bridge, bridge->process_path, PROCESS_IFACE, "SendSignal", g_variant_new("(i)", signum), G_VARIANT_TYPE("()"), NULL);
}

static int
relay_pty(Bridge *bridge, int remote_fd) {
    struct termios saved, raw;
    gboolean restore_termios = FALSE;
    gboolean remote_eof = FALSE;
    struct sigaction action = {.sa_handler = handle_signal};

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) restore_termios = TRUE;
    }

    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    while (!bridge->exited || !remote_eof) {
        struct pollfd fds[2] = {
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = remote_fd, .events = POLLIN | POLLHUP},
        };
        char buffer[16384];
        int result;

        while (g_main_context_iteration(NULL, FALSE)) {}
        if (resize_pending) {
            resize_pending = 0;
            resize_remote_pty(remote_fd);
        }
        if (signal_pending) {
            int signum = signal_pending;
            signal_pending = 0;
            send_remote_signal(bridge, signum);
        }
        if (bridge->exited && remote_eof) break;

        result = poll(fds, 2, 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0 && !write_all(remote_fd, buffer, count)) break;
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t count = read(remote_fd, buffer, sizeof(buffer));
            if (count > 0) {
                if (!write_all(STDOUT_FILENO, buffer, count)) break;
            } else if (count == 0 || errno == EIO) {
                remote_eof = TRUE;
            }
        }
    }

    if (restore_termios) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (WIFEXITED(bridge->wait_status)) return WEXITSTATUS(bridge->wait_status);
    if (WIFSIGNALED(bridge->wait_status)) return 128 + WTERMSIG(bridge->wait_status);
    return bridge->exited ? 1 : 0;
}

int
main(int argc, char **argv) {
    Bridge bridge = {0};
    g_autoptr(GError) error = NULL;
    g_autofree char *container_path = NULL;
    g_autofree char *shell = NULL;
    g_auto(GStrv) default_argv = NULL;
    const char *provider, *name;
    char **command;
    int consumer_fd = -1, producer_fd = -1, result = EXIT_FAILURE;

    if (argc < 3) {
        fprintf(stderr, "usage: %s PROVIDER NAME [--] [COMMAND...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    provider = argv[1];
    name = argv[2];
    command = argv + 3;
    if (*command && strcmp(*command, "--") == 0) command++;

    if (!connect_agent(&bridge, &error)) {
        print_error("cannot connect to host agent", error);
        goto out;
    }
    container_path = find_container(&bridge, provider, name, &error);
    if (!container_path) {
        print_error("cannot find container", error);
        goto out;
    }
    if (!*command) {
        shell = preferred_shell(&bridge);
        default_argv = g_new0(char *, 3);
        default_argv[0] = g_strdup(shell);
        default_argv[1] = g_strdup("-l");
        command = default_argv;
    }

    consumer_fd = create_pty(&bridge, &error);
    if (consumer_fd < 0) {
        print_error("cannot create host PTY", error);
        goto out;
    }
    producer_fd = create_pty_producer(&bridge, consumer_fd, &error);
    if (producer_fd < 0) {
        print_error("cannot open host PTY producer", error);
        goto out;
    }

    g_dbus_connection_signal_subscribe(
        bridge.connection, NULL, AGENT_IFACE, "ProcessExited", AGENT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, process_exited, &bridge, NULL);
    bridge.process_path = spawn_process(&bridge, container_path, producer_fd, command, &error);
    if (!bridge.process_path) {
        print_error("cannot spawn command", error);
        goto out;
    }
    close(producer_fd);
    producer_fd = -1;
    result = relay_pty(&bridge, consumer_fd);

out:
    if (producer_fd >= 0) close(producer_fd);
    if (consumer_fd >= 0) close(consumer_fd);
    g_clear_object(&bridge.connection);
    if (bridge.agent) {
        g_subprocess_force_exit(bridge.agent);
        g_clear_object(&bridge.agent);
    }
    g_free(bridge.process_path);
    return result;
}
