#include "virtual_tree.h"

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
    char path[FS_MAX_PATH];
    uint64_t position;
    uint64_t size;
} VirtualTreeFile;

typedef struct {
    char path[FS_MAX_PATH];
    size_t index;
} VirtualTreeDirectory;

typedef struct {
    char device[32];
    NupArchive *archive;
    bool mounted;
} VirtualTreeDevice;

static VirtualTreeDevice g_tree;

static const char *relative_path(const char *path) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    while (*path == '/') ++path;
    return path;
}

static bool copy_path(char *output, size_t output_size, const char *path) {
    path = relative_path(path);
    int length = snprintf(output, output_size, "%s", path);
    return length >= 0 && (size_t)length < output_size;
}

static void fill_stat(struct stat *st, const NupTreeEntry *entry) {
    memset(st, 0, sizeof(*st));
    st->st_mode = (entry->directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (off_t)entry->size;
}

static int tree_open(struct _reent *r, void *state, const char *path,
                     int flags, int mode) {
    (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        r->_errno = EROFS;
        return -1;
    }
    VirtualTreeFile *file = state;
    NupTreeEntry entry;
    if (!copy_path(file->path, sizeof(file->path), path) ||
        !nupTreeStat(g_tree.archive, file->path, &entry) || entry.directory) {
        r->_errno = ENOENT;
        return -1;
    }
    file->position = 0;
    file->size = entry.size;
    return 0;
}

static int tree_close(struct _reent *r, void *state) {
    (void)r;
    (void)state;
    return 0;
}

static ssize_t tree_read(struct _reent *r, void *state, char *buffer,
                         size_t size) {
    VirtualTreeFile *file = state;
    size_t got = 0;
    Result result = nupTreeRead(g_tree.archive, file->path, file->position,
                                buffer, size, &got);
    if (R_FAILED(result)) {
        r->_errno = EIO;
        return -1;
    }
    file->position += got;
    return (ssize_t)got;
}

static ssize_t tree_write(struct _reent *r, void *state, const char *buffer,
                          size_t size) {
    (void)state;
    (void)buffer;
    (void)size;
    r->_errno = EROFS;
    return -1;
}

static off_t tree_seek(struct _reent *r, void *state, off_t position,
                       int origin) {
    VirtualTreeFile *file = state;
    int64_t base = origin == SEEK_SET ? 0
        : origin == SEEK_CUR ? (int64_t)file->position
        : origin == SEEK_END ? (int64_t)file->size : -1;
    if (base < 0 || position < -base ||
        (uint64_t)(base + position) > file->size) {
        r->_errno = EINVAL;
        return -1;
    }
    file->position = (uint64_t)(base + position);
    return (off_t)file->position;
}

static int tree_fstat(struct _reent *r, void *state, struct stat *st) {
    (void)r;
    VirtualTreeFile *file = state;
    NupTreeEntry entry = {.size = file->size, .directory = false};
    fill_stat(st, &entry);
    return 0;
}

static int tree_stat(struct _reent *r, const char *path, struct stat *st) {
    NupTreeEntry entry;
    if (!nupTreeStat(g_tree.archive, relative_path(path), &entry)) {
        r->_errno = ENOENT;
        return -1;
    }
    fill_stat(st, &entry);
    return 0;
}

static DIR_ITER *tree_diropen(struct _reent *r, DIR_ITER *iterator,
                              const char *path) {
    VirtualTreeDirectory *directory = iterator->dirStruct;
    NupTreeEntry entry;
    if (!copy_path(directory->path, sizeof(directory->path), path) ||
        !nupTreeStat(g_tree.archive, directory->path, &entry) ||
        !entry.directory) {
        r->_errno = ENOENT;
        return NULL;
    }
    directory->index = 0;
    return iterator;
}

static int tree_dirreset(struct _reent *r, DIR_ITER *iterator) {
    (void)r;
    ((VirtualTreeDirectory *)iterator->dirStruct)->index = 0;
    return 0;
}

static int tree_dirnext(struct _reent *r, DIR_ITER *iterator,
                        char *filename, struct stat *st) {
    VirtualTreeDirectory *directory = iterator->dirStruct;
    NupTreeEntry entry;
    if (!nupTreeList(g_tree.archive, directory->path, directory->index,
                     &entry)) {
        r->_errno = ENOENT;
        return -1;
    }
    ++directory->index;
    strcpy(filename, entry.name);
    fill_stat(st, &entry);
    return 0;
}

static int tree_dirclose(struct _reent *r, DIR_ITER *iterator) {
    (void)r;
    (void)iterator;
    return 0;
}

static int tree_statvfs(struct _reent *r, const char *path,
                        struct statvfs *st) {
    (void)r;
    (void)path;
    uint64_t size = nupTreeTotalSize(g_tree.archive);
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = (size + 4095) / 4096;
    st->f_namemax = FS_MAX_PATH - 1;
    return 0;
}

static int tree_read_only(struct _reent *r, const char *path) {
    (void)path;
    r->_errno = EROFS;
    return -1;
}

static int tree_unlink(struct _reent *r, const char *path) {
    return tree_read_only(r, path);
}

static int tree_mkdir(struct _reent *r, const char *path, int mode) {
    (void)mode;
    return tree_read_only(r, path);
}

static int tree_rmdir(struct _reent *r, const char *path) {
    return tree_read_only(r, path);
}

static devoptab_t g_device = {
    .structSize = sizeof(VirtualTreeFile),
    .open_r = tree_open,
    .close_r = tree_close,
    .write_r = tree_write,
    .read_r = tree_read,
    .seek_r = tree_seek,
    .fstat_r = tree_fstat,
    .stat_r = tree_stat,
    .unlink_r = tree_unlink,
    .mkdir_r = tree_mkdir,
    .dirStateSize = sizeof(VirtualTreeDirectory),
    .diropen_r = tree_diropen,
    .dirreset_r = tree_dirreset,
    .dirnext_r = tree_dirnext,
    .dirclose_r = tree_dirclose,
    .statvfs_r = tree_statvfs,
    .rmdir_r = tree_rmdir,
};

Result virtualTreeMount(const char *device, NupArchive *archive) {
    if (!device || !device[0] || strlen(device) >= sizeof(g_tree.device) ||
        !archive)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (g_tree.mounted)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    snprintf(g_tree.device, sizeof(g_tree.device), "%s", device);
    g_tree.archive = archive;
    g_device.name = g_tree.device;
    if (AddDevice(&g_device) < 0) {
        memset(&g_tree, 0, sizeof(g_tree));
        g_device.name = NULL;
        return MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }
    g_tree.mounted = true;
    return 0;
}

void virtualTreeUnmount(void) {
    if (!g_tree.mounted) return;
    char qualified_name[sizeof(g_tree.device) + 2];
    snprintf(qualified_name, sizeof(qualified_name), "%s:", g_tree.device);
    RemoveDevice(qualified_name);
    memset(&g_tree, 0, sizeof(g_tree));
    g_device.name = NULL;
}
