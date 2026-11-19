#include "source_io.h"

#include <errno.h>
#include <stdint.h>

void log_line(const char *format, ...);

static bool g_source_read_error;

void sourceIoResetError(void) {
    g_source_read_error = false;
}

bool sourceIoHadReadError(void) {
    return g_source_read_error;
}

Result sourceReadExact(FILE *file, const char *path, uint64_t offset,
                       void *buffer, size_t size) {
    if (!file || (!buffer && size) || offset > INT64_MAX)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!size) return 0;

    clearerr(file);
    errno = 0;
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        log_line("source-read-seek path=%s offset=%llu errno=%d",
                 path ? path : "(unknown)",
                 (unsigned long long)offset, errno);
        g_source_read_error = true;
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    errno = 0;
    size_t done = fread(buffer, 1, size, file);
    if (done == size) return 0;

    int first_error = errno;
    log_line("source-read-retry path=%s offset=%llu wanted=%zu got=%zu error=%u eof=%u errno=%d",
             path ? path : "(unknown)", (unsigned long long)offset,
             size, done, ferror(file) != 0, feof(file) != 0, first_error);

    while (done < size) {
        uint64_t current = offset + done;
        if (current < offset || current > INT64_MAX) {
            g_source_read_error = true;
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }

        clearerr(file);
        errno = 0;
        if (fseeko(file, (off_t)current, SEEK_SET) != 0) {
            log_line("source-read-retry-seek path=%s offset=%llu errno=%d",
                     path ? path : "(unknown)",
                     (unsigned long long)current, errno);
            g_source_read_error = true;
            return MAKERESULT(Module_Libnx, LibnxError_IoError);
        }

        size_t wanted = size - done;
        if (wanted > 4096) wanted = 4096;
        errno = 0;
        size_t got = fread((unsigned char *)buffer + done, 1, wanted, file);
        if (got != wanted) {
            log_line("source-read-retry-failed path=%s offset=%llu wanted=%zu got=%zu error=%u eof=%u errno=%d",
                     path ? path : "(unknown)",
                     (unsigned long long)current, wanted, got,
                     ferror(file) != 0, feof(file) != 0, errno);
            g_source_read_error = true;
            return MAKERESULT(Module_Libnx, LibnxError_IoError);
        }
        done += got;
    }

    log_line("source-read-retry-recovered path=%s offset=%llu size=%zu first-errno=%d",
             path ? path : "(unknown)", (unsigned long long)offset,
             size, first_error);
    return 0;
}
