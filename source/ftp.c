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
    config.num_bsd_sessions = 8;
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
        Result result = reader->read_file(
            reader->file, offset, reader->buffers[slot], wanted, &got);

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
    return length > 0 && (size_t)length < sizeof(response) &&
           send_all(socket_fd, response, (size_t)length);
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

static int open_listener(uint16_t port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 2) < 0) {
        close(listener);
        return -1;
    }
    return listener;
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

static int accept_data(int listener, FtpStopCallback stop, void *user) {
    for (;;) {
        if (stop && stop(user)) return -1;
        int ready = wait_readable(listener, 200);
        if (ready > 0) return accept(listener, NULL, NULL);
        if (ready < 0 && errno != EINTR) return -1;
    }
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
                          FtpStopCallback stop, FtpStatusCallback status,
                          void *user, const char *label) {
    bool ok = offset <= file_size;
    int socket_buffer = FTP_SOCKET_BUFFER_SIZE;
    setsockopt(data_socket, SOL_SOCKET, SO_SNDBUF,
               &socket_buffer, sizeof(socket_buffer));
    int enabled = 1;
    setsockopt(data_socket, IPPROTO_TCP, TCP_NODELAY,
               &enabled, sizeof(enabled));
    struct timeval send_timeout = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(data_socket, SOL_SOCKET, SO_SNDTIMEO,
               &send_timeout, sizeof(send_timeout));

    FtpReader reader = {0};
    bool read_ahead = ok && reader_start(&reader, file, read_file);
    uint8_t *sync_buffer = NULL;
    if (ok && !read_ahead) {
        sync_buffer = malloc(FTP_SYNC_BUFFER_SIZE);
        if (!sync_buffer) ok = false;
        else if (status)
            status(user, "FTP read-ahead unavailable; using safe mode", offset);
    }
    uint64_t transferred = offset;
    uint64_t next_status = transferred + 16 * 1024 * 1024;
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
            result = read_file(file, transferred, sync_buffer, wanted, &got);
        }
        if (R_FAILED(result) || got == 0) {
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
        if (status && (transferred >= next_status || transferred == file_size)) {
            status(user, label, transferred);
            next_status = transferred + 16 * 1024 * 1024;
        }
        if (stop && stop(user)) {
            failure = "FTP transfer cancelled";
            ok = false;
            break;
        }
    }
    if (reader.thread_started) reader_stop(&reader);
    free(sync_buffer);
    if (!ok && status)
        status(user, failure ? failure : "FTP transfer failed", transferred);
    return ok;
}

static void serve_client(int client, const char *name, uint64_t file_size,
                         void *file, FtpReadCallback read_file,
                         struct in_addr ip,
                         FtpStopCallback stop, FtpStatusCallback status,
                         void *user) {
    size_t name_size = strlen(name);
    bool tar = name_size >= 4 && !strcmp(name + name_size - 4, ".tar");
    const char *sending_status = tar ? "Sending TAR archive"
                                     : "Sending NUP archive";
    const char *complete_status = tar ? "TAR transfer complete"
                                      : "NUP transfer complete";
    int passive = -1;
    uint64_t restart = 0;
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

            if (!strcmp(line, "USER")) reply(client, "331 Any password accepted\r\n");
            else if (!strcmp(line, "PASS")) reply(client, "230 Logged in\r\n");
            else if (!strcmp(line, "SYST")) reply(client, "215 UNIX Type: L8\r\n");
            else if (!strcmp(line, "TYPE")) reply(client, "200 Type set\r\n");
            else if (!strcmp(line, "MODE")) reply(client, "200 Mode set\r\n");
            else if (!strcmp(line, "STRU")) reply(client, "200 Structure set\r\n");
            else if (!strcmp(line, "CLNT")) reply(client, "200 Client noted\r\n");
            else if (!strcmp(line, "NOOP")) reply(client, "200 OK\r\n");
            else if (!strcmp(line, "PWD")) reply(client, "257 \"/\"\r\n");
            else if (!strcmp(line, "CWD")) reply(client, "250 Directory changed\r\n");
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
                if (!requested_file(argument, name)) {
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
                        stop, status, user, sending_status);
                    if (data >= 0) close(data);
                    restart = 0;
                    if (status && ok)
                        status(user, complete_status, file_size);
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

Result ftpServeVirtualFile(const char *name, uint64_t size, void *file,
                           FtpReadCallback read_file, uint16_t port,
                           FtpStopCallback should_stop,
                           FtpStatusCallback status_callback, void *user) {
    if (!name || !file || !read_file)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result result = initialize_sockets();
    if (R_FAILED(result)) return result;
    bool nifm_ready = false;
    result = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(result)) nifm_ready = true;
    u32 raw_ip = 0;
    if (R_SUCCEEDED(result)) result = nifmGetCurrentIpAddress(&raw_ip);
    if (R_SUCCEEDED(result) && !raw_ip)
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    int listener = R_SUCCEEDED(result) ? open_listener(port) : -1;
    if (R_SUCCEEDED(result) && listener < 0) result = io_error();

    struct in_addr ip = {.s_addr = raw_ip};
    if (R_SUCCEEDED(result) && status_callback) {
        char message[256];
        snprintf(message, sizeof(message), "ftp://%s:%u/%s",
                 inet_ntoa(ip), port, name);
        status_callback(user, message, 0);
    }

    while (R_SUCCEEDED(result) && (!should_stop || !should_stop(user))) {
        int ready = wait_readable(listener, 200);
        if (ready < 0 && errno != EINTR) {
            result = io_error();
            break;
        }
        if (ready <= 0) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        if (status_callback) status_callback(user, "FTP client connected", 0);
        serve_client(client, name, size, file, read_file, ip,
                     should_stop, status_callback, user);
        close(client);
        if (status_callback) {
            char message[256];
            snprintf(message, sizeof(message), "ftp://%s:%u/%s",
                     inet_ntoa(ip), port, name);
            status_callback(user, message, 0);
        }
    }

    if (listener >= 0) close(listener);
    if (nifm_ready) nifmExit();
    socketExit();
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

static void serve_directory_client(int client, const char *root,
                                   struct in_addr ip, FtpStopCallback stop,
                                   FtpStatusCallback status, void *user) {
    int passive = -1;
    uint64_t restart = 0;
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
            else if (!strcmp(line, "TYPE") || !strcmp(line, "MODE") ||
                     !strcmp(line, "STRU") || !strcmp(line, "CLNT") ||
                     !strcmp(line, "OPTS") || !strcmp(line, "NOOP"))
                reply(client, "200 OK\r\n");
            else if (!strcmp(line, "PWD"))
                reply(client, "257 \"%s\"\r\n", current);
            else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP")) {
                if (!strcmp(line, "CDUP"))
                    path_ok = normalize_path(current, "..", path, sizeof(path)) &&
                        device_path(root, path, local, sizeof(local));
                if (path_ok && stat(local, &st) == 0 && S_ISDIR(st.st_mode)) {
                    snprintf(current, sizeof(current), "%s", path);
                    reply(client, "250 Directory changed\r\n");
                } else reply(client, "550 Directory unavailable\r\n");
            } else if (!strcmp(line, "FEAT")) {
                reply(client, "211-Features\r\n EPSV\r\n PASV\r\n MDTM\r\n MLST type*;size*;perm*;\r\n REST STREAM\r\n SIZE\r\n UTF8\r\n211 End\r\n");
            } else if (!strcmp(line, "SIZE")) {
                if (path_ok && stat(local, &st) == 0 && !S_ISDIR(st.st_mode))
                    reply(client, "213 %llu\r\n", (unsigned long long)st.st_size);
                else reply(client, "550 File unavailable\r\n");
            } else if (!strcmp(line, "MDTM")) {
                if (path_ok && stat(local, &st) == 0)
                    reply(client, "213 20260101000000\r\n");
                else reply(client, "550 Path unavailable\r\n");
            } else if (!strcmp(line, "MLST")) {
                if (path_ok && stat(local, &st) == 0) {
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
                    bool ok = data >= 0 && path_ok && send_directory_listing(
                        data, root, path, !strcmp(line, "MLSD"),
                        !strcmp(line, "NLST"));
                    if (data >= 0) close(data);
                    reply(client, ok ? "226 Transfer complete\r\n"
                                     : "426 Transfer aborted\r\n");
                }
            } else if (!strcmp(line, "RETR")) {
                if (!path_ok || stat(local, &st) != 0 || S_ISDIR(st.st_mode))
                    reply(client, "550 File unavailable\r\n");
                else if (restart > (uint64_t)st.st_size) {
                    restart = 0;
                    reply(client, "551 Restart position is past end of file\r\n");
                } else if (passive < 0) reply(client, "425 Use PASV or EPSV first\r\n");
                else {
                    FILE *handle = fopen(local, "rb");
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
                            (uint64_t)st.st_size, restart, stop, status, user,
                            "Sending extracted file");
                        fclose(handle);
                        if (data >= 0) close(data);
                        restart = 0;
                        if (status && ok)
                            status(user, "Extracted file transfer complete",
                                   (uint64_t)st.st_size);
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
    Result result = initialize_sockets();
    if (R_FAILED(result)) return result;
    bool nifm_ready = false;
    result = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(result)) nifm_ready = true;
    u32 raw_ip = 0;
    if (R_SUCCEEDED(result)) result = nifmGetCurrentIpAddress(&raw_ip);
    if (R_SUCCEEDED(result) && !raw_ip)
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    int listener = R_SUCCEEDED(result) ? open_listener(port) : -1;
    if (R_SUCCEEDED(result) && listener < 0) result = io_error();

    struct in_addr ip = {.s_addr = raw_ip};
    if (R_SUCCEEDED(result) && status_callback) {
        char message[256];
        snprintf(message, sizeof(message), "ftp://%s:%u/", inet_ntoa(ip), port);
        status_callback(user, message, 0);
    }
    while (R_SUCCEEDED(result) && (!should_stop || !should_stop(user))) {
        int ready = wait_readable(listener, 200);
        if (ready < 0 && errno != EINTR) {
            result = io_error();
            break;
        }
        if (ready <= 0) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        if (status_callback) status_callback(user, "FTP client connected", 0);
        serve_directory_client(client, root, ip, should_stop,
                               status_callback, user);
        close(client);
        if (status_callback) {
            char message[256];
            snprintf(message, sizeof(message), "ftp://%s:%u/",
                     inet_ntoa(ip), port);
            status_callback(user, message, 0);
        }
    }
    if (listener >= 0) close(listener);
    if (nifm_ready) nifmExit();
    socketExit();
    return result;
}
