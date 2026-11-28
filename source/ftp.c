#include "ftp.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

void log_line(const char *format, ...);

/* Set while the single-file (TAR/NUP) server runs: its sessions are short,
   so every command and reply goes to KaiDump.log for diagnosis. */
static volatile bool g_ftp_trace;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static Result io_error(void) {
    return MAKERESULT(Module_Libnx, LibnxError_IoError);
}

static Result initialize_sockets(void) {
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.tcp_tx_buf_size = 256 * 1024;
    config.tcp_rx_buf_size = 256 * 1024;
    config.tcp_tx_buf_max_size = 1024 * 1024;
    config.tcp_rx_buf_max_size = 1024 * 1024;
    config.sb_efficiency = 32;
    /* One session per concurrently blocking socket call: the accept loop
       plus each client worker (see FTP_MAX_CLIENTS). */
    config.num_bsd_sessions = 12;
    config.bsd_service_type = BsdServiceType_Auto;
    Result result = socketInitialize(&config);
    return R_SUCCEEDED(result) ? result : socketInitializeDefault();
}

Result ftpGetLocalAddress(u32 *address) {
    if (!address)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *address = 0;

    Result result = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(result)) return result;
    result = nifmGetCurrentIpAddress(address);
    nifmExit();
    if (R_SUCCEEDED(result) && !*address)
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return result;
}

static bool send_all(int socket_fd, const void *data, size_t size) {
    const uint8_t *bytes = data;
    while (size) {
        ssize_t sent = send(socket_fd, bytes, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= (size_t)sent;
    }
    return true;
}

#define FTP_TRANSFER_BUFFER_SIZE (4 * 1024 * 1024)
#define FTP_SYNC_BUFFER_SIZE (1024 * 1024)
#define FTP_SOCKET_BUFFER_SIZE (1024 * 1024)

/* Every access to dump data goes through this lock. The virtual tree and
   the NUP archive mount packages on demand and are not thread-safe, and
   several FTP clients can now be served at once. Initialized per server. */
static pthread_mutex_t g_source_mutex;

static void source_lock(void) { pthread_mutex_lock(&g_source_mutex); }
static void source_unlock(void) { pthread_mutex_unlock(&g_source_mutex); }

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool thread_started;
    bool stopping;
    bool request_pending;
    bool response_ready;
    unsigned slot;
    uint64_t offset;
    size_t wanted;
    size_t got;
    Result result;
    void *file;
    FtpReadCallback read_file;
    uint8_t *buffers[2];
} FtpReader;

static void *reader_main(void *argument) {
    FtpReader *reader = argument;
    pthread_mutex_lock(&reader->mutex);
    while (!reader->stopping) {
        while (!reader->stopping && !reader->request_pending)
            pthread_cond_wait(&reader->condition, &reader->mutex);
        if (reader->stopping) break;

        unsigned slot = reader->slot;
        uint64_t offset = reader->offset;
        size_t wanted = reader->wanted;
        reader->request_pending = false;
        pthread_mutex_unlock(&reader->mutex);

        size_t got = 0;
        source_lock();
        Result result = reader->read_file(
            reader->file, offset, reader->buffers[slot], wanted, &got);
        source_unlock();

        pthread_mutex_lock(&reader->mutex);
        reader->slot = slot;
        reader->got = got;
        reader->result = result;
        reader->response_ready = true;
        pthread_cond_broadcast(&reader->condition);
    }
    pthread_mutex_unlock(&reader->mutex);
    return NULL;
}

static bool reader_start(FtpReader *reader, void *file,
                         FtpReadCallback read_file) {
    memset(reader, 0, sizeof(*reader));
    reader->file = file;
    reader->read_file = read_file;
    reader->buffers[0] = malloc(FTP_TRANSFER_BUFFER_SIZE);
    reader->buffers[1] = malloc(FTP_TRANSFER_BUFFER_SIZE);
    if (!reader->buffers[0] || !reader->buffers[1]) goto fail;
    if (pthread_mutex_init(&reader->mutex, NULL) != 0) goto fail;
    if (pthread_cond_init(&reader->condition, NULL) != 0) {
        pthread_mutex_destroy(&reader->mutex);
        goto fail;
    }
    if (pthread_create(&reader->thread, NULL, reader_main, reader) != 0) {
        pthread_cond_destroy(&reader->condition);
        pthread_mutex_destroy(&reader->mutex);
        goto fail;
    }
    reader->thread_started = true;
    return true;

fail:
    free(reader->buffers[0]);
    free(reader->buffers[1]);
    memset(reader, 0, sizeof(*reader));
    return false;
}

static void reader_request(FtpReader *reader, unsigned slot, uint64_t offset,
                           size_t wanted) {
    pthread_mutex_lock(&reader->mutex);
    reader->slot = slot;
    reader->offset = offset;
    reader->wanted = wanted;
    reader->response_ready = false;
    reader->request_pending = true;
    pthread_cond_broadcast(&reader->condition);
    pthread_mutex_unlock(&reader->mutex);
}

static void reader_wait(FtpReader *reader, unsigned *slot, size_t *got,
                        Result *result) {
    pthread_mutex_lock(&reader->mutex);
    while (!reader->response_ready)
        pthread_cond_wait(&reader->condition, &reader->mutex);
    *slot = reader->slot;
    *got = reader->got;
    *result = reader->result;
    reader->response_ready = false;
    pthread_mutex_unlock(&reader->mutex);
}

static void reader_stop(FtpReader *reader) {
    if (reader->thread_started) {
        pthread_mutex_lock(&reader->mutex);
        reader->stopping = true;
        pthread_cond_broadcast(&reader->condition);
        pthread_mutex_unlock(&reader->mutex);
        pthread_join(reader->thread, NULL);
        pthread_cond_destroy(&reader->condition);
        pthread_mutex_destroy(&reader->mutex);
    }
    free(reader->buffers[0]);
    free(reader->buffers[1]);
    memset(reader, 0, sizeof(*reader));
}

static bool reply(int socket_fd, const char *format, ...) {
    char response[768];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(response, sizeof(response), format, arguments);
    va_end(arguments);
    bool sent = length > 0 && (size_t)length < sizeof(response) &&
                send_all(socket_fd, response, (size_t)length);
    if (g_ftp_trace && length > 0) {
        const char *end = strpbrk(response, "\r\n");
        log_line("ftp-reply socket=%d sent=%d %.*s", socket_fd, sent ? 1 : 0,
                 end ? (int)(end - response) : length, response);
    }
    return sent;
}

static int wait_readable(int socket_fd, int milliseconds) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000,
    };
    return select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
}

static int wait_writable(int socket_fd, int milliseconds) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_fd, &write_set);
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000,
    };
    return select(socket_fd + 1, NULL, &write_set, NULL, &timeout);
}

static bool send_transfer(int socket_fd, const void *data, size_t size,
                          FtpStopCallback stop, void *user) {
    const uint8_t *bytes = data;
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    while (size) {
        ssize_t sent = send(socket_fd, bytes, size, MSG_NOSIGNAL);
        if (sent > 0) {
            bytes += sent;
            size -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stop && stop(user)) {
                errno = ECANCELED;
                return false;
            }
            int ready = wait_writable(socket_fd, 100);
            if (ready >= 0) continue;
            if (errno == EINTR) continue;
        }
        return false;
    }
    return true;
}

/* Step and errno of the last open_listener failure, for the log. */
static const char *g_listener_step = "";
static int g_listener_errno = 0;

static int open_listener(uint16_t port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        g_listener_step = "socket";
        g_listener_errno = errno;
        return -1;
    }
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0) {
        g_listener_step = "bind";
        g_listener_errno = errno;
        close(listener);
        return -1;
    }
    if (listen(listener, 2) < 0) {
        g_listener_step = "listen";
        g_listener_errno = errno;
        close(listener);
        return -1;
    }
    return listener;
}

/* The control port. If it is taken (for example by the game kaiHEN was
   launched through), try the next few ports; *port returns the one used. */
#define FTP_PORT_ATTEMPTS 10

static int open_server_listener(uint16_t *port,
                                FtpStatusCallback status_callback,
                                void *user) {
    const uint16_t first = *port;
    for (uint16_t i = 0; i < FTP_PORT_ATTEMPTS; ++i) {
        const uint16_t candidate = first + i;
        int listener = open_listener(candidate);
        if (listener >= 0) {
            *port = candidate;
            return listener;
        }
        if (status_callback) {
            char message[128];
            snprintf(message, sizeof(message),
                     "FTP port %u unavailable (%s failed, errno %d)",
                     candidate, g_listener_step, g_listener_errno);
            status_callback(user, message, 0, 0);
        }
        /* Only a port that cannot be bound is worth retrying elsewhere. */
        if (strcmp(g_listener_step, "bind")) break;
    }
    return -1;
}

static void report_wait_error(FtpStatusCallback status_callback,
                              void *user) {
    if (!status_callback) return;
    char message[128];
    snprintf(message, sizeof(message), "FTP server error (select errno %d)",
             errno);
    status_callback(user, message, 0, 0);
}

static int open_passive_listener(uint16_t *port) {
    int listener = open_listener(0);
    if (listener < 0) return -1;
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &size) < 0) {
        close(listener);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return listener;
}

#define FTP_DATA_ACCEPT_TIMEOUT_MS 30000

static int accept_data(int listener, FtpStopCallback stop, void *user) {
    for (int waited = 0; waited < FTP_DATA_ACCEPT_TIMEOUT_MS; waited += 200) {
        if (stop && stop(user)) return -1;
        int ready = wait_readable(listener, 200);
        if (ready > 0) return accept(listener, NULL, NULL);
        if (ready < 0 && errno != EINTR) return -1;
    }
    return -1;
}

/* Server and per-client state -------------------------------------------
 * The main thread accepts connections and owns the UI; each client runs in
 * its own worker thread and reports through a small message queue and
 * byte counters that the main thread turns into status updates. */
#define FTP_MAX_CLIENTS 8
#define FTP_MESSAGE_QUEUE 32
#define FTP_MESSAGE_SIZE 160
#define FTP_WORKER_STACK_SIZE (256 * 1024)

typedef struct FtpServer FtpServer;

typedef struct {
    FtpServer *server;
    pthread_t thread;
    int client;
    bool active;
    bool finished;
    bool transferring;
    uint64_t offset;
    uint64_t position;
    uint8_t *sync_buffer;
} FtpWorker;

typedef void (*FtpClientHandler)(int client, struct in_addr ip,
                                 FtpWorker *worker, void *context);
static void server_message(FtpServer *server, const char *message);

struct FtpServer {
    pthread_mutex_t mutex;
    bool stopping;
    FtpClientHandler handler;
    void *context;
    struct in_addr ip;
    FtpWorker workers[FTP_MAX_CLIENTS];
    char messages[FTP_MESSAGE_QUEUE][FTP_MESSAGE_SIZE];
    unsigned message_first;
    unsigned message_count;
    uint64_t completed_bytes;
    uint64_t completed_files;
    bool ascii_warned;
};

/* KaiDump always sends files byte for byte, but in ASCII mode (TYPE A) the
   client rewrites line endings on its side. FileZilla's "Auto" setting
   does this for text files and, by default, for files without an
   extension, which corrupts ExeFS binaries such as main and sdk. Listings
   are still allowed in ASCII mode; downloads are refused (see RETR).
   Returns whether the connection is now in ASCII mode. */
static bool worker_type(FtpWorker *worker, const char *argument) {
    const bool ascii = argument[0] == 'A' || argument[0] == 'a';
    if (ascii && !__atomic_exchange_n(&worker->server->ascii_warned, true,
                                      __ATOMIC_ACQ_REL))
        server_message(worker->server,
                       "FTP client uses ASCII mode; set it to binary transfers to download");
    return ascii;
}

#define FTP_ASCII_REFUSED \
    "550 Binary mode required: set your FTP client to binary transfers " \
    "(TYPE I); ASCII mode would alter the files\r\n"

static bool worker_stop(void *user) {
    FtpWorker *worker = user;
    return __atomic_load_n(&worker->server->stopping, __ATOMIC_ACQUIRE);
}

static void server_message(FtpServer *server, const char *message) {
    pthread_mutex_lock(&server->mutex);
    if (server->message_count == FTP_MESSAGE_QUEUE) {
        server->message_first = (server->message_first + 1) % FTP_MESSAGE_QUEUE;
        --server->message_count;
    }
    unsigned at = (server->message_first + server->message_count) %
                  FTP_MESSAGE_QUEUE;
    snprintf(server->messages[at], FTP_MESSAGE_SIZE, "%s", message);
    ++server->message_count;
    pthread_mutex_unlock(&server->mutex);
}

static void worker_message(FtpWorker *worker, const char *message) {
    server_message(worker->server, message);
}

static void worker_transfer_begin(FtpWorker *worker, uint64_t offset) {
    pthread_mutex_lock(&worker->server->mutex);
    worker->offset = offset;
    worker->position = offset;
    worker->transferring = true;
    pthread_mutex_unlock(&worker->server->mutex);
}

static void worker_transfer_progress(FtpWorker *worker, uint64_t position) {
    pthread_mutex_lock(&worker->server->mutex);
    worker->position = position;
    pthread_mutex_unlock(&worker->server->mutex);
}

/* Counts what was actually sent, including the part of a failed transfer. */
static void worker_transfer_end(FtpWorker *worker, uint64_t position,
                                bool complete) {
    FtpServer *server = worker->server;
    pthread_mutex_lock(&server->mutex);
    if (position > worker->offset)
        server->completed_bytes += position - worker->offset;
    if (complete) ++server->completed_files;
    worker->transferring = false;
    worker->offset = worker->position = 0;
    pthread_mutex_unlock(&server->mutex);
}

static const char *command_argument(char *line) {
    char *space = strchr(line, ' ');
    if (!space) return "";
    *space++ = 0;
    while (*space == ' ') ++space;
    return space;
}

static bool requested_file(const char *argument, const char *name) {
    while (*argument == '/') ++argument;
    return !strcmp(argument, name);
}

static bool send_listing(int data_socket, const char *name,
                         uint64_t file_size, bool machine) {
    char line[1024];
    int length;
    if (machine) {
        length = snprintf(line, sizeof(line),
                          "type=file;size=%llu;perm=r; %s\r\n",
                          (unsigned long long)file_size, name);
    } else {
        length = snprintf(line, sizeof(line),
                          "-r--r--r-- 1 owner group %llu Jan 01 00:00 %s\r\n",
                          (unsigned long long)file_size, name);
    }
    return length > 0 && (size_t)length < sizeof(line) &&
           send_all(data_socket, line, (size_t)length);
}

static bool send_name(int data_socket, const char *name) {
    char line[512];
    int length = snprintf(line, sizeof(line), "%s\r\n", name);
    return length > 0 && (size_t)length < sizeof(line) &&
           send_all(data_socket, line, (size_t)length);
}

static bool transfer_file(int data_socket, void *file, FtpReadCallback read_file,
                          uint64_t file_size, uint64_t offset,
                          FtpWorker *worker, const char *label) {
    FtpStopCallback stop = worker_stop;
    void *user = worker;
    bool ok = offset <= file_size;
    worker_transfer_begin(worker, offset);
    worker_message(worker, label);
    int socket_buffer = FTP_SOCKET_BUFFER_SIZE;
    setsockopt(data_socket, SOL_SOCKET, SO_SNDBUF,
               &socket_buffer, sizeof(socket_buffer));
    int enabled = 1;
    setsockopt(data_socket, IPPROTO_TCP, TCP_NODELAY,
               &enabled, sizeof(enabled));
    struct timeval send_timeout = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(data_socket, SOL_SOCKET, SO_SNDTIMEO,
               &send_timeout, sizeof(send_timeout));

    /* Read-ahead (a reader thread and two 4 MiB buffers) only pays off for
       large files; small files are read directly into a buffer the worker
       keeps for its whole connection. */
    FtpReader reader = {0};
    const bool large = ok && file_size - offset > FTP_TRANSFER_BUFFER_SIZE;
    bool read_ahead = large && reader_start(&reader, file, read_file);
    if (ok && !read_ahead && !worker->sync_buffer) {
        worker->sync_buffer = malloc(FTP_SYNC_BUFFER_SIZE);
        if (!worker->sync_buffer) ok = false;
    }
    uint8_t *sync_buffer = worker->sync_buffer;
    if (ok && large && !read_ahead)
        worker_message(worker, "FTP read-ahead unavailable; using safe mode");
    uint64_t transferred = offset;
    unsigned next_slot = 0;
    if (read_ahead && transferred < file_size) {
        size_t wanted = file_size - transferred > FTP_TRANSFER_BUFFER_SIZE
            ? FTP_TRANSFER_BUFFER_SIZE : (size_t)(file_size - transferred);
        reader_request(&reader, next_slot, transferred, wanted);
    }
    const char *failure = NULL;
    while (ok && transferred < file_size) {
        if (stop && stop(user)) {
            failure = "FTP transfer cancelled";
            ok = false;
            break;
        }
        unsigned slot = 0;
        size_t got = 0;
        Result result = 0;
        uint8_t *data = sync_buffer;
        if (read_ahead) {
            reader_wait(&reader, &slot, &got, &result);
            data = reader.buffers[slot];
        } else {
            size_t wanted = file_size - transferred > FTP_SYNC_BUFFER_SIZE
                ? FTP_SYNC_BUFFER_SIZE : (size_t)(file_size - transferred);
            source_lock();
            result = read_file(file, transferred, sync_buffer, wanted, &got);
            source_unlock();
        }
        if (R_FAILED(result) || got == 0) {
            log_line("ftp-read-failed offset=%llu size=%llu result=0x%08x got=%zu",
                     (unsigned long long)transferred,
                     (unsigned long long)file_size, result, got);
            failure = R_FAILED(result)
                ? "Could not read the selected file"
                : "The selected file ended before its advertised size";
            ok = false;
            break;
        }

        uint64_t following = transferred + got;
        next_slot = slot ^ 1;
        if (read_ahead && following < file_size) {
            size_t wanted = file_size - following > FTP_TRANSFER_BUFFER_SIZE
                ? FTP_TRANSFER_BUFFER_SIZE : (size_t)(file_size - following);
            reader_request(&reader, next_slot, following, wanted);
        }

        if (!send_transfer(data_socket, data, got, stop, user)) {
            failure = errno == ECANCELED
                ? "FTP transfer cancelled"
                : "The FTP client closed the data connection";
            ok = false;
            break;
        }
        transferred = following;
        worker_transfer_progress(worker, transferred);
        if (stop && stop(user)) {
            failure = "FTP transfer cancelled";
            ok = false;
            break;
        }
    }
    if (reader.thread_started) reader_stop(&reader);
    if (!ok) worker_message(worker, failure ? failure : "FTP transfer failed");
    worker_transfer_end(worker, transferred, ok);
    return ok;
}

typedef struct {
    const char *name;
    uint64_t size;
    void *file;
    FtpReadCallback read_file;
} VirtualFileContext;

static void serve_client(int client, struct in_addr ip, FtpWorker *worker,
                         void *context_pointer) {
    const VirtualFileContext *context = context_pointer;
    const char *name = context->name;
    const uint64_t file_size = context->size;
    void *file = context->file;
    FtpReadCallback read_file = context->read_file;
    FtpStopCallback stop = worker_stop;
    void *user = worker;
    size_t name_size = strlen(name);
    bool tar = name_size >= 4 && !strcmp(name + name_size - 4, ".tar");
    const char *sending_status = tar ? "Sending TAR archive"
                                     : "Sending NUP archive";
    const char *complete_status = tar ? "TAR transfer complete"
                                      : "NUP transfer complete";
    int passive = -1;
    uint64_t restart = 0;
    bool ascii = false;
    char input[2048] = {0};
    size_t used = 0;
    reply(client, "220 KaiDump read-only FTP ready\r\n");
    while (!stop || !stop(user)) {
        int ready = wait_readable(client, 200);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) break;
            continue;
        }
        ssize_t got = recv(client, input + used, sizeof(input) - used - 1, 0);
        if (got <= 0) break;
        used += (size_t)got;
        input[used] = 0;
        char *line_end;
        while ((line_end = strstr(input, "\r\n")) != NULL) {
            *line_end = 0;
            char line[1024];
            snprintf(line, sizeof(line), "%s", input);
            size_t consumed = (size_t)(line_end - input) + 2;
            memmove(input, input + consumed, used - consumed + 1);
            used -= consumed;
            const char *argument = command_argument(line);
            for (char *at = line; *at; ++at)
                if (*at >= 'a' && *at <= 'z') *at -= 'a' - 'A';
            if (strcmp(line, "NOOP"))
                log_line("ftp-command socket=%d %s %s", client, line,
                         strcmp(line, "PASS") ? argument : "***");

            if (!strcmp(line, "USER")) reply(client, "331 Any password accepted\r\n");
            else if (!strcmp(line, "PASS")) reply(client, "230 Logged in\r\n");
            else if (!strcmp(line, "SYST")) reply(client, "215 UNIX Type: L8\r\n");
            else if (!strcmp(line, "TYPE")) {
                ascii = worker_type(worker, argument);
                reply(client, "200 Type set\r\n");
            }
            else if (!strcmp(line, "MODE")) reply(client, "200 Mode set\r\n");
            else if (!strcmp(line, "STRU")) reply(client, "200 Structure set\r\n");
            else if (!strcmp(line, "CLNT")) reply(client, "200 Client noted\r\n");
            else if (!strcmp(line, "NOOP")) reply(client, "200 OK\r\n");
            else if (!strcmp(line, "PWD")) reply(client, "257 \"/\"\r\n");
            else if (!strcmp(line, "CWD")) {
                /* The only directory is the root. Accepting CWD into the
                   file makes clients such as KDE Dolphin (kio_ftp) treat
                   the archive as an empty folder and never download it. */
                const char *at = argument;
                while (*at == '/' || *at == '.') ++at;
                reply(client, *at ? "550 Not a directory\r\n"
                                  : "250 Directory changed\r\n");
            }
            else if (!strcmp(line, "CDUP")) reply(client, "250 Directory changed\r\n");
            else if (!strcmp(line, "FEAT"))
                reply(client, "211-Features\r\n EPSV\r\n PASV\r\n MDTM\r\n MLST type*;size*;perm*;\r\n REST STREAM\r\n SIZE\r\n UTF8\r\n211 End\r\n");
            else if (!strcmp(line, "OPTS")) reply(client, "200 UTF8 enabled\r\n");
            else if (!strcmp(line, "SIZE")) {
                if (requested_file(argument, name))
                    reply(client, "213 %llu\r\n", (unsigned long long)file_size);
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "MDTM")) {
                if (requested_file(argument, name))
                    reply(client, "213 20260101000000\r\n");
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "MLST")) {
                if (requested_file(argument, name))
                    reply(client,
                          "250-File details\r\n type=file;size=%llu;perm=r; %s\r\n250 End\r\n",
                          (unsigned long long)file_size, name);
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "STAT")) {
                if (!argument[0])
                    reply(client, "211 KaiDump FTP ready\r\n");
                else if (requested_file(argument, name))
                    reply(client,
                          "213-File status\r\n-r--r--r-- 1 owner group %llu Jan 01 00:00 %s\r\n213 End\r\n",
                          (unsigned long long)file_size, name);
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "REST")) {
                restart = strtoull(argument, NULL, 10);
                if (restart <= file_size)
                    reply(client, "350 Restart position accepted\r\n");
                else {
                    restart = 0;
                    reply(client, "551 Restart position is past end of file\r\n");
                }
            } else if (!strcmp(line, "PASV") || !strcmp(line, "EPSV")) {
                if (passive >= 0) close(passive);
                uint16_t data_port = 0;
                passive = open_passive_listener(&data_port);
                if (passive < 0) {
                    reply(client, "425 Cannot open passive listener\r\n");
                } else if (!strcmp(line, "EPSV")) {
                    reply(client, "229 Entering Extended Passive Mode (|||%u|)\r\n",
                          data_port);
                } else {
                    const uint8_t *bytes = (const uint8_t *)&ip.s_addr;
                    reply(client, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                          bytes[0], bytes[1], bytes[2], bytes[3],
                          data_port >> 8, data_port & 0xff);
                }
            } else if (!strcmp(line, "LIST") || !strcmp(line, "MLSD") ||
                       !strcmp(line, "NLST")) {
                if (passive < 0) {
                    reply(client, "425 Use PASV or EPSV first\r\n");
                } else {
                    reply(client, "150 Opening data connection\r\n");
                    int data = accept_data(passive, stop, user);
                    close(passive);
                    passive = -1;
                    bool ok = data >= 0 && (!strcmp(line, "NLST")
                        ? send_name(data, name)
                        : send_listing(data, name, file_size,
                                       !strcmp(line, "MLSD")));
                    if (data >= 0) close(data);
                    reply(client, ok ? "226 Transfer complete\r\n"
                                     : "426 Transfer aborted\r\n");
                }
            } else if (!strcmp(line, "RETR")) {
                if (ascii) {
                    restart = 0;
                    reply(client, FTP_ASCII_REFUSED);
                } else if (!requested_file(argument, name)) {
                    reply(client, "550 File unavailable\r\n");
                } else if (passive < 0) {
                    reply(client, "425 Use PASV or EPSV first\r\n");
                } else {
                    reply(client, "150 Opening binary data connection\r\n");
                    int data = accept_data(passive, stop, user);
                    close(passive);
                    passive = -1;
                    bool ok = data >= 0 && transfer_file(
                        data, file, read_file, file_size, restart,
                        worker, sending_status);
                    if (data < 0)
                        worker_message(worker, "FTP data connection was not opened");
                    if (data >= 0) close(data);
                    log_line("ftp-retr socket=%d data=%d restart=%llu ok=%d",
                             client, data, (unsigned long long)restart,
                             ok ? 1 : 0);
                    restart = 0;
                    if (ok) worker_message(worker, complete_status);
                    reply(client, ok ? "226 Transfer complete\r\n"
                                     : "426 Transfer aborted\r\n");
                }
            } else if (!strcmp(line, "QUIT")) {
                reply(client, "221 Goodbye\r\n");
                if (passive >= 0) close(passive);
                return;
            } else {
                reply(client, "502 Command not implemented\r\n");
            }
        }
        if (used == sizeof(input) - 1) break;
    }
    if (passive >= 0) close(passive);
}

static void *worker_main(void *argument) {
    FtpWorker *worker = argument;
    FtpServer *server = worker->server;
    worker_message(worker, "FTP client connected");
    server->handler(worker->client, server->ip, worker, server->context);
    close(worker->client);
    free(worker->sync_buffer);
    worker->sync_buffer = NULL;
    worker_message(worker, "FTP client disconnected");
    __atomic_store_n(&worker->finished, true, __ATOMIC_RELEASE);
    return NULL;
}

static uint64_t server_total_bytes(FtpServer *server) {
    uint64_t total = server->completed_bytes;
    for (int i = 0; i < FTP_MAX_CLIENTS; ++i) {
        const FtpWorker *worker = &server->workers[i];
        if (worker->active && worker->transferring &&
            worker->position > worker->offset)
            total += worker->position - worker->offset;
    }
    return total;
}

/* Main thread only: deliver queued messages and the running totals. */
static void server_flush(FtpServer *server, FtpStatusCallback status_callback,
                         void *user, uint64_t *reported_bytes,
                         uint64_t *reported_files) {
    char messages[FTP_MESSAGE_QUEUE][FTP_MESSAGE_SIZE];
    unsigned count = 0;
    pthread_mutex_lock(&server->mutex);
    for (; count < server->message_count; ++count)
        memcpy(messages[count],
               server->messages[(server->message_first + count) %
                                FTP_MESSAGE_QUEUE],
               FTP_MESSAGE_SIZE);
    server->message_first = 0;
    server->message_count = 0;
    const uint64_t bytes = server_total_bytes(server);
    const uint64_t files = server->completed_files;
    pthread_mutex_unlock(&server->mutex);

    if (!status_callback) return;
    for (unsigned i = 0; i < count; ++i)
        status_callback(user, messages[i], bytes, files);
    if (!count && (bytes != *reported_bytes || files != *reported_files))
        status_callback(user, NULL, bytes, files);
    *reported_bytes = bytes;
    *reported_files = files;
}

static void server_reap(FtpServer *server, bool wait) {
    for (int i = 0; i < FTP_MAX_CLIENTS; ++i) {
        FtpWorker *worker = &server->workers[i];
        if (!worker->active) continue;
        if (!wait && !__atomic_load_n(&worker->finished, __ATOMIC_ACQUIRE))
            continue;
        pthread_join(worker->thread, NULL);
        pthread_mutex_lock(&server->mutex);
        worker->active = false;
        pthread_mutex_unlock(&server->mutex);
    }
}

static bool server_start_worker(FtpServer *server, int client) {
    FtpWorker *worker = NULL;
    for (int i = 0; i < FTP_MAX_CLIENTS && !worker; ++i)
        if (!server->workers[i].active) worker = &server->workers[i];
    if (!worker) return false;

    pthread_mutex_lock(&server->mutex);
    *worker = (FtpWorker){
        .server = server,
        .client = client,
        .active = true,
    };
    pthread_mutex_unlock(&server->mutex);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, FTP_WORKER_STACK_SIZE);
    int created = pthread_create(&worker->thread, &attributes, worker_main,
                                 worker);
    pthread_attr_destroy(&attributes);
    if (created == 0) return true;

    pthread_mutex_lock(&server->mutex);
    worker->active = false;
    pthread_mutex_unlock(&server->mutex);
    return false;
}

static Result run_server(uint16_t port, const char *address_suffix,
                         FtpClientHandler handler, void *context,
                         FtpStopCallback should_stop,
                         FtpStatusCallback status_callback, void *user) {
    Result result = initialize_sockets();
    if (R_FAILED(result)) return result;
    bool nifm_ready = false;
    result = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(result)) nifm_ready = true;
    u32 raw_ip = 0;
    if (R_SUCCEEDED(result)) result = nifmGetCurrentIpAddress(&raw_ip);
    if (R_SUCCEEDED(result) && !raw_ip)
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    int listener = R_SUCCEEDED(result)
        ? open_server_listener(&port, status_callback, user) : -1;
    if (R_SUCCEEDED(result) && listener < 0) result = io_error();

    static FtpServer server;
    memset(&server, 0, sizeof(server));
    server.handler = handler;
    server.context = context;
    server.ip.s_addr = raw_ip;
    bool locks_ready = false;
    if (R_SUCCEEDED(result)) {
        if (pthread_mutex_init(&server.mutex, NULL) != 0) {
            result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else if (pthread_mutex_init(&g_source_mutex, NULL) != 0) {
            pthread_mutex_destroy(&server.mutex);
            result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else {
            locks_ready = true;
        }
    }

    if (R_SUCCEEDED(result) && status_callback) {
        char message[256];
        snprintf(message, sizeof(message), "ftp://%s:%u/%s",
                 inet_ntoa(server.ip), port, address_suffix);
        status_callback(user, message, 0, 0);
    }

    uint64_t reported_bytes = 0;
    uint64_t reported_files = 0;
    while (R_SUCCEEDED(result) && (!should_stop || !should_stop(user))) {
        server_flush(&server, status_callback, user, &reported_bytes,
                     &reported_files);
        server_reap(&server, false);
        int ready = wait_readable(listener, 200);
        if (ready < 0 && errno != EINTR) {
            report_wait_error(status_callback, user);
            result = io_error();
            break;
        }
        if (ready <= 0) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        /* A client that just disconnected may still hold its slot. */
        server_reap(&server, false);
        if (!server_start_worker(&server, client)) {
            reply(client, "421 Too many connections; try again shortly\r\n");
            close(client);
            server_message(&server, "FTP connection refused: too many clients");
        }
    }

    if (locks_ready) {
        __atomic_store_n(&server.stopping, true, __ATOMIC_RELEASE);
        if (status_callback)
            status_callback(user, "Stopping FTP server...", reported_bytes,
                            reported_files);
        server_reap(&server, true);
        server_flush(&server, status_callback, user, &reported_bytes,
                     &reported_files);
        pthread_mutex_destroy(&g_source_mutex);
        pthread_mutex_destroy(&server.mutex);
    }
    if (listener >= 0) close(listener);
    if (nifm_ready) nifmExit();
    socketExit();
    return result;
}

Result ftpServeVirtualFile(const char *name, uint64_t size, void *file,
                           FtpReadCallback read_file, uint16_t port,
                           FtpStopCallback should_stop,
                           FtpStatusCallback status_callback, void *user) {
    if (!name || !file || !read_file)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    VirtualFileContext context = {
        .name = name,
        .size = size,
        .file = file,
        .read_file = read_file,
    };
    g_ftp_trace = true;
    Result result = run_server(port, name, serve_client, &context,
                               should_stop, status_callback, user);
    g_ftp_trace = false;
    return result;
}

typedef struct {
    FILE *handle;
    uint64_t position;
} DirectoryFile;

static Result read_directory_file(void *source, uint64_t offset, void *buffer,
                                  size_t size, size_t *bytes_read) {
    DirectoryFile *file = source;
    *bytes_read = 0;
    if (file->position != offset &&
        fseeko(file->handle, (off_t)offset, SEEK_SET) != 0)
        return io_error();
    size_t got = fread(buffer, 1, size, file->handle);
    if (!got && size && ferror(file->handle)) return io_error();
    file->position = offset + got;
    *bytes_read = got;
    return 0;
}

static bool normalize_path(const char *current, const char *argument,
                           char *output, size_t output_size) {
    char combined[2048];
    if (!argument || !argument[0]) argument = current;
    int length = argument[0] == '/'
        ? snprintf(combined, sizeof(combined), "%s", argument)
        : snprintf(combined, sizeof(combined), "%s%s%s", current,
                   !strcmp(current, "/") ? "" : "/", argument);
    if (length < 0 || (size_t)length >= sizeof(combined)) return false;

    size_t used = 1;
    output[0] = '/';
    output[1] = 0;
    char *save = NULL;
    for (char *part = strtok_r(combined, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !part[0]) continue;
        if (!strcmp(part, "..")) {
            if (used > 1) {
                --used;
                while (used > 1 && output[used - 1] != '/') --used;
                output[used] = 0;
            }
            continue;
        }
        if (strchr(part, ':') || strchr(part, '\\')) return false;
        size_t part_size = strlen(part);
        size_t slash = used > 1 ? 1 : 0;
        if (used + slash + part_size >= output_size) return false;
        if (slash) output[used++] = '/';
        memcpy(output + used, part, part_size + 1);
        used += part_size;
    }
    return true;
}

static bool device_path(const char *root, const char *path,
                        char *output, size_t output_size) {
    int length = snprintf(output, output_size, "%s%s", root, path);
    return length >= 0 && (size_t)length < output_size;
}

static const char *path_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static bool send_directory_entry(int data, const char *name,
                                 const struct stat *st, bool machine,
                                 bool names_only) {
    if (names_only) return send_name(data, name);
    char line[2048];
    int length;
    if (machine) {
        length = S_ISDIR(st->st_mode)
            ? snprintf(line, sizeof(line), "type=dir;perm=el; %s\r\n", name)
            : snprintf(line, sizeof(line),
                       "type=file;size=%llu;perm=r; %s\r\n",
                       (unsigned long long)st->st_size, name);
    } else {
        length = snprintf(line, sizeof(line),
                          "%s 1 owner group %llu Jan 01 00:00 %s\r\n",
                          S_ISDIR(st->st_mode) ? "dr-xr-xr-x" : "-r--r--r--",
                          (unsigned long long)st->st_size, name);
    }
    return length > 0 && (size_t)length < sizeof(line) &&
           send_all(data, line, (size_t)length);
}

static bool send_directory_listing(int data, const char *root,
                                   const char *path, bool machine,
                                   bool names_only) {
    char local[FS_MAX_PATH + 64];
    if (!device_path(root, path, local, sizeof(local))) return false;
    struct stat st;
    if (stat(local, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode))
        return send_directory_entry(data, path_name(path), &st,
                                    machine, names_only);
    DIR *directory = opendir(local);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[FS_MAX_PATH + 64];
        int length = snprintf(child, sizeof(child), "%s%s%s", local,
                              local[strlen(local) - 1] == '/' ? "" : "/",
                              entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child) ||
            stat(child, &st) != 0) {
            ok = false;
            break;
        }
        ok = send_directory_entry(data, entry->d_name, &st,
                                  machine, names_only);
    }
    closedir(directory);
    return ok;
}

static int locked_stat(const char *path, struct stat *st) {
    source_lock();
    int result = stat(path, st);
    source_unlock();
    return result;
}

static void serve_directory_client(int client, struct in_addr ip,
                                   FtpWorker *worker, void *context) {
    const char *root = context;
    FtpStopCallback stop = worker_stop;
    void *user = worker;
    int passive = -1;
    uint64_t restart = 0;
    bool ascii = false;
    char current[FS_MAX_PATH] = "/";
    char input[2048] = {0};
    size_t used = 0;
    reply(client, "220 KaiDump read-only FTP ready\r\n");
    while (!stop || !stop(user)) {
        int ready = wait_readable(client, 200);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) break;
            continue;
        }
        ssize_t got = recv(client, input + used, sizeof(input) - used - 1, 0);
        if (got <= 0) break;
        used += (size_t)got;
        input[used] = 0;
        char *line_end;
        while ((line_end = strstr(input, "\r\n")) != NULL) {
            *line_end = 0;
            char line[1024];
            snprintf(line, sizeof(line), "%s", input);
            size_t consumed = (size_t)(line_end - input) + 2;
            memmove(input, input + consumed, used - consumed + 1);
            used -= consumed;
            const char *argument = command_argument(line);
            for (char *at = line; *at; ++at)
                if (*at >= 'a' && *at <= 'z') *at -= 'a' - 'A';

            char path[FS_MAX_PATH];
            char local[FS_MAX_PATH + 64];
            struct stat st;
            bool path_ok = normalize_path(current, argument, path, sizeof(path)) &&
                device_path(root, path, local, sizeof(local));

            if (!strcmp(line, "USER")) reply(client, "331 Any password accepted\r\n");
            else if (!strcmp(line, "PASS")) reply(client, "230 Logged in\r\n");
            else if (!strcmp(line, "SYST")) reply(client, "215 UNIX Type: L8\r\n");
            else if (!strcmp(line, "TYPE")) {
                ascii = worker_type(worker, argument);
                reply(client, "200 OK\r\n");
            } else if (!strcmp(line, "MODE") ||
                     !strcmp(line, "STRU") || !strcmp(line, "CLNT") ||
                     !strcmp(line, "OPTS") || !strcmp(line, "NOOP"))
                reply(client, "200 OK\r\n");
            else if (!strcmp(line, "PWD"))
                reply(client, "257 \"%s\"\r\n", current);
            else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP")) {
                if (!strcmp(line, "CDUP"))
                    path_ok = normalize_path(current, "..", path, sizeof(path)) &&
                        device_path(root, path, local, sizeof(local));
                if (path_ok && locked_stat(local, &st) == 0 && S_ISDIR(st.st_mode)) {
                    snprintf(current, sizeof(current), "%s", path);
                    reply(client, "250 Directory changed\r\n");
                } else reply(client, "550 Directory unavailable\r\n");
            } else if (!strcmp(line, "FEAT")) {
                reply(client, "211-Features\r\n EPSV\r\n PASV\r\n MDTM\r\n MLST type*;size*;perm*;\r\n REST STREAM\r\n SIZE\r\n UTF8\r\n211 End\r\n");
            } else if (!strcmp(line, "SIZE")) {
                if (path_ok && locked_stat(local, &st) == 0 && !S_ISDIR(st.st_mode))
                    reply(client, "213 %llu\r\n", (unsigned long long)st.st_size);
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "MDTM")) {
                if (path_ok && locked_stat(local, &st) == 0)
                    reply(client, "213 20260101000000\r\n");
                else reply(client, "550 Path unavailable\r\n");
            } else if (!strcmp(line, "MLST")) {
                if (path_ok && locked_stat(local, &st) == 0) {
                    if (S_ISDIR(st.st_mode))
                        reply(client, "250-Path details\r\n type=dir;perm=el; %s\r\n250 End\r\n", path_name(path));
                    else
                        reply(client, "250-File details\r\n type=file;size=%llu;perm=r; %s\r\n250 End\r\n",
                              (unsigned long long)st.st_size, path_name(path));
                } else reply(client, "550 Path unavailable\r\n");
            } else if (!strcmp(line, "REST")) {
                restart = strtoull(argument, NULL, 10);
                reply(client, "350 Restart position accepted\r\n");
            } else if (!strcmp(line, "PASV") || !strcmp(line, "EPSV")) {
                if (passive >= 0) close(passive);
                uint16_t data_port = 0;
                passive = open_passive_listener(&data_port);
                if (passive < 0) reply(client, "425 Cannot open passive listener\r\n");
                else if (!strcmp(line, "EPSV"))
                    reply(client, "229 Entering Extended Passive Mode (|||%u|)\r\n", data_port);
                else {
                    const uint8_t *bytes = (const uint8_t *)&ip.s_addr;
                    reply(client, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                          bytes[0], bytes[1], bytes[2], bytes[3],
                          data_port >> 8, data_port & 0xff);
                }
            } else if (!strcmp(line, "LIST") || !strcmp(line, "MLSD") ||
                       !strcmp(line, "NLST")) {
                if (passive < 0) reply(client, "425 Use PASV or EPSV first\r\n");
                else {
                    const char *list_argument = argument[0] == '-' ? "" : argument;
                    path_ok = normalize_path(current, list_argument, path,
                                             sizeof(path));
                    reply(client, "150 Opening data connection\r\n");
                    int data = accept_data(passive, stop, user);
                    close(passive);
                    passive = -1;
                    bool ok = false;
                    if (data >= 0 && path_ok) {
                        source_lock();
                        ok = send_directory_listing(
                            data, root, path, !strcmp(line, "MLSD"),
                            !strcmp(line, "NLST"));
                        source_unlock();
                    }
                    if (data >= 0) close(data);
                    reply(client, ok ? "226 Transfer complete\r\n"
                                     : "426 Transfer aborted\r\n");
                }
            } else if (!strcmp(line, "RETR")) {
                if (ascii) {
                    restart = 0;
                    reply(client, FTP_ASCII_REFUSED);
                } else if (!path_ok || locked_stat(local, &st) != 0 || S_ISDIR(st.st_mode))
                    reply(client, "550 File unavailable\r\n");
                else if (restart > (uint64_t)st.st_size) {
                    restart = 0;
                    reply(client, "551 Restart position is past end of file\r\n");
                } else if (passive < 0) reply(client, "425 Use PASV or EPSV first\r\n");
                else {
                    source_lock();
                    FILE *handle = fopen(local, "rb");
                    source_unlock();
                    if (!handle) {
                        reply(client, "550 File unavailable\r\n");
                        close(passive);
                        passive = -1;
                    } else {
                        reply(client, "150 Opening binary data connection\r\n");
                        int data = accept_data(passive, stop, user);
                        close(passive);
                        passive = -1;
                        DirectoryFile file = {.handle = handle};
                        bool ok = data >= 0 && transfer_file(
                            data, &file, read_directory_file,
                            (uint64_t)st.st_size, restart, worker,
                            "Sending extracted file");
                        if (data < 0)
                            worker_message(worker, "FTP data connection was not opened");
                        source_lock();
                        fclose(handle);
                        source_unlock();
                        if (data >= 0) close(data);
                        restart = 0;
                        if (ok)
                            worker_message(worker,
                                           "Extracted file transfer complete");
                        reply(client, ok ? "226 Transfer complete\r\n"
                                         : "426 Transfer aborted\r\n");
                    }
                }
            } else if (!strcmp(line, "STAT")) {
                reply(client, "211 KaiDump FTP ready\r\n");
            } else if (!strcmp(line, "QUIT")) {
                reply(client, "221 Goodbye\r\n");
                if (passive >= 0) close(passive);
                return;
            } else reply(client, "502 Command not implemented\r\n");
        }
        if (used == sizeof(input) - 1) break;
    }
    if (passive >= 0) close(passive);
}

Result ftpServeDirectory(const char *root, uint16_t port,
                         FtpStopCallback should_stop,
                         FtpStatusCallback status_callback, void *user) {
    if (!root || !root[0])
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return run_server(port, "", serve_directory_client, (void *)root,
                      should_stop, status_callback, user);
}
