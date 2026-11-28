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

/* Package meta TLV types, numbered as in SPEC.md §5 (from 0x0001), which is
   what readers such as Lichii use. The reference Python tool (format.py)
   numbers them from 0 instead, so its `nup info` shows these shifted. */
enum {
    NUP_TLV_NPDM = 0x0001,
    NUP_TLV_NACP = 0x0002,
    NUP_TLV_ICON = 0x0003,
    NUP_TLV_DISPLAY_VERSION = 0x0004,
    NUP_TLV_TITLE_NAME = 0x0005,
    NUP_TLV_PUBLISHER = 0x0006,
    NUP_TLV_BUILD_ID = 0x0007,
    NUP_TLV_SOURCE_DUMP = 0x0008,
    NUP_TLV_LOCALIZED_TITLES = 0x0009,
    NUP_TLV_LOCALIZED_ICONS = 0x000A,
};

enum {
    NUP_LAYOUT_UNSPECIFIED = 0,
    NUP_LAYOUT_GAME,
    NUP_LAYOUT_BASE,
    NUP_LAYOUT_UPDATE,
    NUP_LAYOUT_DLC,
    NUP_LAYOUT_GAME_PROGRAM,
    NUP_LAYOUT_FOLDER,  /* tree output only: under NupInput.folder */
};

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint32_t source_id;
    uint8_t kind;
    uint8_t flags;
    uint8_t layout;
    uint8_t program_index;
    char folder[40];
} NupInput;

typedef struct {
    const char *name;
    uint64_t size;
    uint64_t source_offset;
} NupExternalFile;

/* A file that tree output (extracted, TAR) adds next to a package's exefs/
   and romfs/: control.nacp and every icon_<Language>.dat. */
#define NUP_MAX_CONTROL_FILES 34
typedef struct {
    char name[64];
    const uint8_t *data;
    uint32_t size;
} NupControlFile;

typedef Result (*NupReadCallback)(void *source, uint64_t offset, void *buffer,
                                  size_t size, size_t *bytes_read);

typedef struct {
    char exefs[256];
    char romfs[256];
    const NupExternalFile *exefs_files;
    size_t exefs_file_count;
    NupReadCallback read_exefs;
    void *read_exefs_user;
    /* Optional application-level meta TLVs for this package (NUP SPEC §5:
       name, icon, display version, ...), already serialized and sorted by
       type. The NPDM, build ID and provenance TLVs are added by nup. */
    const void *meta;
    size_t meta_size;
    /* Set when the mounted content is not the version the input asked for:
       the base game because its update could not be opened, or a DLC with
       its update merged. */
    uint32_t version;
    bool version_set;
    /* Optional files for tree output, placed next to exefs/ and romfs/.
       nup copies them; NUP output ignores them. */
    const NupControlFile *control_files;
    size_t control_file_count;
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

/* Placed next to game_switch1/ and game_switch2/ in extracted and TAR
   output of Switch 2 Editions. */
#define NUP_EDITION_README \
    "This game is a Nintendo Switch 2 Edition and contains two versions:\n" \
    "\n" \
    "game_switch1/  the Nintendo Switch 1 version\n" \
    "game_switch2/  the Nintendo Switch 2 version\n" \
    "\n" \
    "Some games ship the Switch 2 version as a complete game of its own.\n" \
    "Others keep most of their data in the Switch 1 version, which the\n" \
    "Switch 2 version then needs as well. Keep both folders together.\n" \
    "\n" \
    "Dumped with KaiDump.\n"

/* Finds a TLV in a serialized meta block (NUP SPEC §5). */
bool nupMetaFind(const void *meta, size_t meta_size, uint16_t type,
                 const uint8_t **data, uint32_t *size);

/* NUP language tag for an NACP language name ("AmericanEnglish" -> "en-us"),
   or NULL for a language nup does not know. */
const char *nupLanguageTag(const char *nacp_language);

uint64_t nupArchiveSize(const NupArchive *archive);
Result nupArchiveRead(NupArchive *archive, uint64_t offset, void *buffer,
                      size_t size, size_t *bytes_read);
bool nupTreeStat(NupArchive *archive, const char *path, NupTreeEntry *entry);
bool nupTreeList(NupArchive *archive, const char *path, size_t index,
                 NupTreeEntry *entry);
Result nupTreeRead(NupArchive *archive, const char *path, uint64_t offset,
                   void *buffer, size_t size, size_t *bytes_read);
uint64_t nupTreeTotalSize(const NupArchive *archive);
/* Files that exist only in the tree (control.nacp, icons, README.txt), by
   index; false past the last one. */
bool nupTreeMemoryFile(const NupArchive *archive, size_t index,
                       const char **path, const uint8_t **data,
                       uint32_t *size);
void nupArchiveDeactivate(NupArchive *archive);
void nupArchiveClose(NupArchive *archive);
