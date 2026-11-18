#include "virtual_file.h"

#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

typedef struct {
    uint64_t position;
} VirtualFileState;

typedef struct {
    bool emitted;
} VirtualDirectoryState;

typedef struct {
    char device[32];
    char name[256];
    uint64_t size;
    void *source;
    VirtualFileReadCallback read_source;
    bool mounted;
} VirtualFileDevice;

static VirtualFileDevice g_virtual;

static const char *relative_path(const char *path) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    while (*path == '/') ++path;
    return path;
}

static bool is_root_path(const char *path) {
    return !relative_path(path)[0];
}

static bool is_file_path(const char *path) {
    return !strcmp(relative_path(path), g_virtual.name);
}

static void fill_file_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (off_t)g_virtual.size;
}

static void fill_directory_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 1;
}

static int virtual_open(struct _reent *r, void *state, const char *path,
                        int flags, int mode) {
    (void)mode;
    if (!is_file_path(path)) {
        r->_errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        r->_errno = EROFS;
        return -1;
    }
    ((VirtualFileState *)state)->position = 0;
    return 0;
}

static int virtual_close(struct _reent *r, void *state) {
    (void)r;
    (void)state;
    return 0;
}

static ssize_t virtual_read(struct _reent *r, void *state, char *buffer,
                            size_t size) {
    VirtualFileState *file = state;
    if (file->position >= g_virtual.size || !size) return 0;
    uint64_t remaining = g_virtual.size - file->position;
    if ((uint64_t)size > remaining) size = (size_t)remaining;

    size_t got = 0;
    Result result = g_virtual.read_source(
        g_virtual.source, file->position, buffer, size, &got);
    if (R_FAILED(result)) {
        r->_errno = EIO;
        return -1;
    }
    file->position += got;
    return (ssize_t)got;
}

static ssize_t virtual_write(struct _reent *r, void *state, const char *buffer,
                             size_t size) {
    (void)state;
    (void)buffer;
    (void)size;
    r->_errno = EROFS;
    return -1;
}

static off_t virtual_seek(struct _reent *r, void *state, off_t position,
                          int origin) {
    VirtualFileState *file = state;
    int64_t base = origin == SEEK_SET ? 0
        : origin == SEEK_CUR ? (int64_t)file->position
        : origin == SEEK_END ? (int64_t)g_virtual.size : -1;
    if (base < 0 || position < -base) {
        r->_errno = EINVAL;
        return -1;
    }
    uint64_t next = (uint64_t)(base + position);
    if (next > g_virtual.size) {
        r->_errno = EINVAL;
        return -1;
    }
    file->position = next;
    return (off_t)next;
}

static int virtual_fstat(struct _reent *r, void *state, struct stat *st) {
    (void)r;
    (void)state;
    fill_file_stat(st);
    return 0;
}

static int virtual_stat(struct _reent *r, const char *path, struct stat *st) {
    if (is_root_path(path)) {
        fill_directory_stat(st);
        return 0;
    }
    if (is_file_path(path)) {
        fill_file_stat(st);
        return 0;
    }
    r->_errno = ENOENT;
    return -1;
}

static DIR_ITER *virtual_diropen(struct _reent *r, DIR_ITER *iterator,
                                 const char *path) {
    if (!is_root_path(path)) {
        r->_errno = ENOENT;
        return NULL;
    }
    ((VirtualDirectoryState *)iterator->dirStruct)->emitted = false;
    return iterator;
}

static int virtual_dirreset(struct _reent *r, DIR_ITER *iterator) {
    (void)r;
    ((VirtualDirectoryState *)iterator->dirStruct)->emitted = false;
    return 0;
}

static int virtual_dirnext(struct _reent *r, DIR_ITER *iterator,
                           char *filename, struct stat *st) {
    VirtualDirectoryState *directory = iterator->dirStruct;
    if (directory->emitted) {
        r->_errno = ENOENT;
        return -1;
    }
    directory->emitted = true;
    strcpy(filename, g_virtual.name);
    fill_file_stat(st);
    return 0;
}

static int virtual_dirclose(struct _reent *r, DIR_ITER *iterator) {
    (void)r;
    (void)iterator;
    return 0;
}

static int virtual_statvfs(struct _reent *r, const char *path,
                           struct statvfs *st) {
    (void)r;
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = (g_virtual.size + 4095) / 4096;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = 1;
    st->f_namemax = sizeof(g_virtual.name) - 1;
    return 0;
}

static int virtual_read_only(struct _reent *r, const char *path) {
    (void)path;
    r->_errno = EROFS;
    return -1;
}

static int virtual_unlink(struct _reent *r, const char *path) {
    return virtual_read_only(r, path);
}

static int virtual_mkdir(struct _reent *r, const char *path, int mode) {
    (void)mode;
    return virtual_read_only(r, path);
}

static int virtual_rmdir(struct _reent *r, const char *path) {
    return virtual_read_only(r, path);
}

static devoptab_t g_device = {
    .name = NULL,
    .structSize = sizeof(VirtualFileState),
    .open_r = virtual_open,
    .close_r = virtual_close,
    .write_r = virtual_write,
    .read_r = virtual_read,
    .seek_r = virtual_seek,
    .fstat_r = virtual_fstat,
    .stat_r = virtual_stat,
    .unlink_r = virtual_unlink,
    .mkdir_r = virtual_mkdir,
    .dirStateSize = sizeof(VirtualDirectoryState),
    .diropen_r = virtual_diropen,
    .dirreset_r = virtual_dirreset,
    .dirnext_r = virtual_dirnext,
    .dirclose_r = virtual_dirclose,
    .statvfs_r = virtual_statvfs,
    .rmdir_r = virtual_rmdir,
};

Result virtualFileMount(const char *device, const char *name, uint64_t size,
                        void *source, VirtualFileReadCallback read_source) {
    if (!device || !device[0] || strlen(device) >= sizeof(g_virtual.device) ||
        !name || !name[0] || strlen(name) >= sizeof(g_virtual.name) ||
        strchr(name, '/') || strchr(name, '\\') || !source || !read_source)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (g_virtual.mounted)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    snprintf(g_virtual.device, sizeof(g_virtual.device), "%s", device);
    snprintf(g_virtual.name, sizeof(g_virtual.name), "%s", name);
    g_virtual.size = size;
    g_virtual.source = source;
    g_virtual.read_source = read_source;
    g_device.name = g_virtual.device;
    if (AddDevice(&g_device) < 0) {
        memset(&g_virtual, 0, sizeof(g_virtual));
        return MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }
    g_virtual.mounted = true;
    return 0;
}

void virtualFileUnmount(void) {
    if (!g_virtual.mounted) return;
    char qualified_name[sizeof(g_virtual.device) + 2];
    snprintf(qualified_name, sizeof(qualified_name), "%s:", g_virtual.device);
    RemoveDevice(qualified_name);
    memset(&g_virtual, 0, sizeof(g_virtual));
    g_device.name = NULL;
}
