#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <switch.h>

typedef bool (*FtpStopCallback)(void *user);
/* Called on the calling thread only. status may be NULL when only the
   totals changed. transferred and files are totals for the whole server
   session, across all clients, including partially sent files. */
typedef void (*FtpStatusCallback)(void *user, const char *status,
                                  uint64_t transferred, uint64_t files);
typedef Result (*FtpReadCallback)(void *file, uint64_t offset, void *buffer,
                                  size_t size, size_t *bytes_read);

Result ftpGetLocalAddress(u32 *address);
Result ftpServeVirtualFile(const char *name, uint64_t size, void *file,
                           FtpReadCallback read_file, uint16_t port,
                           FtpStopCallback should_stop,
                           FtpStatusCallback status_callback, void *user);
Result ftpServeDirectory(const char *root, uint16_t port,
                         FtpStopCallback should_stop,
                         FtpStatusCallback status_callback, void *user);
