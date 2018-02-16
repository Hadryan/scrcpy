#include "server.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <SDL2/SDL_assert.h>

#include "config.h"
#include "log.h"
#include "net.h"

#define SOCKET_NAME "scrcpy"

#ifdef OVERRIDE_SERVER_JAR
# define DEFAULT_SERVER_JAR OVERRIDE_SERVER_JAR
#else
# define DEFAULT_SERVER_JAR PREFIX PREFIXED_SERVER_JAR
#endif

static const char *get_server_path(void) {
    const char *server_path = getenv("SCRCPY_SERVER_JAR");
    if (!server_path) {
        server_path = DEFAULT_SERVER_JAR;
    }
    return server_path;
}

static SDL_bool push_server(const char *serial) {
    process_t process = adb_push(serial, get_server_path(), "/data/local/tmp/scrcpy-server.jar");
    return process_check_success(process, "adb push");
}

static SDL_bool enable_tunnel(const char *serial, Uint16 local_port) {
    process_t process = adb_reverse(serial, SOCKET_NAME, local_port);
    return process_check_success(process, "adb reverse");
}

static SDL_bool disable_tunnel(const char *serial) {
    process_t process = adb_reverse_remove(serial, SOCKET_NAME);
    return process_check_success(process, "adb reverse --remove");
}

static process_t execute_server(const char *serial, Uint16 max_size, Uint32 bit_rate) {
    char max_size_string[6];
    char bit_rate_string[11];
    sprintf(max_size_string, "%"PRIu16, max_size);
    sprintf(bit_rate_string, "%"PRIu32, bit_rate);
    const char *const cmd[] = {
        "shell",
        "CLASSPATH=/data/local/tmp/scrcpy-server.jar",
        "app_process",
        "/", // unused
        "com.genymobile.scrcpy.ScrCpyServer",
        max_size_string,
        bit_rate_string,
    };
    return adb_execute(serial, cmd, sizeof(cmd) / sizeof(cmd[0]));
}

static socket_t listen_on_port(Uint16 port) {
#define IPV4_LOCALHOST 0x7F000001
    return net_listen(IPV4_LOCALHOST, port, 1);
}

void server_init(struct server *server) {
    *server = (struct server) SERVER_INITIALIZER;
}

SDL_bool server_start(struct server *server, const char *serial, Uint16 local_port,
                      Uint16 max_size, Uint32 bit_rate) {
    if (!push_server(serial)) {
        return SDL_FALSE;
    }

    if (!enable_tunnel(serial, local_port)) {
        return SDL_FALSE;
    }

    // At the application level, the device part is "the server" because it
    // serves video stream and control. However, at network level, the client
    // listens and the server connects to the client. That way, the client can
    // listen before starting the server app, so there is no need to try to
    // connect until the server socket is listening on the device.

    server->server_socket = listen_on_port(local_port);
    if (server->server_socket == INVALID_SOCKET) {
        LOGE("Could not listen on port %" PRIu16, local_port);
        disable_tunnel(serial);
        return SDL_FALSE;
    }

    // server will connect to our server socket
    server->process = execute_server(serial, max_size, bit_rate);
    if (server->process == PROCESS_NONE) {
        net_close(server->server_socket);
        disable_tunnel(serial);
        return SDL_FALSE;
    }

    server->adb_reverse_enabled = SDL_TRUE;

    return SDL_TRUE;
}

socket_t server_connect_to(struct server *server, const char *serial, Uint32 timeout_ms) {
    server->device_socket = net_accept(server->server_socket);
    if (server->device_socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    // we don't need the server socket anymore
    net_close(server->server_socket);
    server->server_socket = INVALID_SOCKET;

    // we don't need the adb tunnel anymore
    disable_tunnel(serial); // ignore failure
    server->adb_reverse_enabled = SDL_FALSE;

    return server->device_socket;
}

void server_stop(struct server *server, const char *serial) {
    SDL_assert(server->process != PROCESS_NONE);

    if (server->device_socket != INVALID_SOCKET) {
        // shutdown the socket to finish the device process gracefully
        net_shutdown(server->device_socket, SHUT_RDWR);
    }

    LOGD("Waiting the server to complete execution on the device...");
    cmd_simple_wait(server->process, NULL); // ignore exit code
    LOGD("Server terminated");

    if (server->adb_reverse_enabled) {
        // ignore failure
        disable_tunnel(serial);
    }
}

void server_destroy(struct server *server) {
    if (server->server_socket != INVALID_SOCKET) {
        net_close(server->server_socket);
    }
    if (server->device_socket != INVALID_SOCKET) {
        net_close(server->device_socket);
    }
}
