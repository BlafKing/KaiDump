#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <switch.h>

enum {
    NUP_KIND_BASE = 0,
    NUP_KIND_UPDATE = 1,
    NUP_KIND_ADDON = 2,
    NUP_KIND_ADDON_UPDATE = 3,
};

enum {
    NUP_LAYOUT_UNSPECIFIED = 0,
    NUP_LAYOUT_GAME,
    NUP_LAYOUT_BASE,
    NUP_LAYOUT_UPDATE,
    NUP_LAYOUT_DLC,
};

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint32_t source_id;
    uint8_t kind;
    uint8_t flags;
    uint8_t layout;
} NupInput;

typedef struct {
    const char *name;
    uint64_t size;
    uint64_t source_offset;
} NupExternalFile;

typedef Result (*NupReadCallback)(void *source, uint64_t offset, void *buffer,
                                  size_t size, size_t *bytes_read);

typedef struct {
    char exefs[256];
    char romfs[256];
    const NupExternalFile *exefs_files;
    size_t exefs_file_count;
    NupReadCallback read_exefs;
    void *read_exefs_user;
} NupMount;

typedef bool (*NupCancelCallback)(void *user);
typedef Result (*NupMountCallback)(const NupInput *input, NupMount *mount,
                                   void *user);
typedef void (*NupUnmountCallback)(void *user);

typedef struct NupArchive NupArchive;

typedef struct {
    char name[FS_MAX_PATH];
    uint64_t size;
    bool directory;
} NupTreeEntry;

Result nupCreateDirect(const char *output_path, const NupInput *inputs,
                       size_t input_count, NupMountCallback mount,
                       NupUnmountCallback unmount, void *mount_user,
                       void *copy_buffer, size_t buffer_size,
                       NupCancelCallback cancelled, void *cancel_user,
                       uint64_t *total_bytes, uint64_t *written_bytes,
                       uint64_t *packed_files);

Result nupPrepareVirtual(NupArchive **out_archive, const NupInput *inputs,
                         size_t input_count, NupMountCallback mount,
                         NupUnmountCallback unmount, void *mount_user,
                         void *read_buffer, size_t buffer_size,
                         NupCancelCallback cancelled, void *cancel_user,
                         uint64_t *total_bytes, uint64_t *hashed_bytes,
                         uint64_t *packed_files);

Result nupPrepareTree(NupArchive **out_archive, const NupInput *inputs,
                      size_t input_count, NupMountCallback mount,
                      NupUnmountCallback unmount, void *mount_user,
                      NupCancelCallback cancelled, void *cancel_user,
                      uint64_t *file_count);

uint64_t nupArchiveSize(const NupArchive *archive);
Result nupArchiveRead(NupArchive *archive, uint64_t offset, void *buffer,
                      size_t size, size_t *bytes_read);
bool nupTreeStat(NupArchive *archive, const char *path, NupTreeEntry *entry);
bool nupTreeList(NupArchive *archive, const char *path, size_t index,
                 NupTreeEntry *entry);
Result nupTreeRead(NupArchive *archive, const char *path, uint64_t offset,
                   void *buffer, size_t size, size_t *bytes_read);
uint64_t nupTreeTotalSize(const NupArchive *archive);
void nupArchiveDeactivate(NupArchive *archive);
void nupArchiveClose(NupArchive *archive);
