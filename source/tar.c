#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAR_BLOCK_SIZE 512
#define TAR_END_SIZE (TAR_BLOCK_SIZE * 2)
#define TAR_MAX_OCTAL_SIZE 077777777777ULL
#define TAR_CANCELED MAKERESULT(Module_Libnx, 200)

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6];
    char version[2];
    char user_name[32];
    char group_name[32];
    char device_major[8];
    char device_minor[8];
    char prefix[155];
    char padding[12];
} TarHeader;

_Static_assert(sizeof(TarHeader) == TAR_BLOCK_SIZE, "TAR header size");

typedef struct {
    char *path;
    char *archive_path;
    char *pax;
    size_t pax_size;
    uint64_t size;
    uint64_t start;
    uint64_t pax_data;
    uint64_t header;
    uint64_t data;
    uint64_t end;
    bool directory;
} TarEntry;

struct TarArchive {
    NupArchive *tree;
    TarEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    uint64_t size;
};

static Result io_error(void) {
    return MAKERESULT(Module_Libnx, LibnxError_IoError);
}

static uint64_t align_block(uint64_t value) {
    return (value + TAR_BLOCK_SIZE - 1) & ~(uint64_t)(TAR_BLOCK_SIZE - 1);
}

static bool grow_entries(TarArchive *archive) {
    if (archive->entry_count < archive->entry_capacity) return true;
    size_t capacity = archive->entry_capacity ? archive->entry_capacity * 2 : 128;
    TarEntry *entries = realloc(archive->entries, capacity * sizeof(*entries));
    if (!entries) return false;
    archive->entries = entries;
    archive->entry_capacity = capacity;
    return true;
}

static bool split_ustar_path(const char *path, char name[100],
                             char prefix[155]) {
    size_t length = strlen(path);
    if (length <= sizeof(((TarHeader *)0)->name)) {
        memcpy(name, path, length);
        return true;
    }
    if (length > sizeof(((TarHeader *)0)->name) +
                 sizeof(((TarHeader *)0)->prefix) + 1)
        return false;
    for (const char *slash = path + length; slash > path; --slash) {
        if (slash[-1] != '/') continue;
        size_t prefix_size = (size_t)(slash - path - 1);
        size_t name_size = length - (size_t)(slash - path);
        if (!name_size || prefix_size > sizeof(((TarHeader *)0)->prefix) ||
            name_size > sizeof(((TarHeader *)0)->name))
            continue;
        memcpy(prefix, path, prefix_size);
        memcpy(name, slash, name_size);
        return true;
    }
    return false;
}

static char *pax_record(const char *key, const char *value) {
    size_t body = strlen(key) + 1 + strlen(value) + 1;
    size_t digits = 1;
    size_t total;
    for (;;) {
        total = digits + 1 + body;
        size_t next_digits = 1;
        for (size_t value_length = total; value_length >= 10;
             value_length /= 10)
            ++next_digits;
        if (next_digits == digits) break;
        digits = next_digits;
    }
    char *record = malloc(total + 1);
    if (!record) return NULL;
    int written = snprintf(record, total + 1, "%zu %s=%s\n", total, key, value);
    if (written < 0 || (size_t)written != total) {
        free(record);
        return NULL;
    }
    return record;
}

static bool append_pax(char **pax, size_t *pax_size, const char *record) {
    size_t record_size = strlen(record);
    char *joined = realloc(*pax, *pax_size + record_size + 1);
    if (!joined) return false;
    memcpy(joined + *pax_size, record, record_size + 1);
    *pax = joined;
    *pax_size += record_size;
    return true;
}

static Result add_entry(TarArchive *archive, const char *path,
                        const NupTreeEntry *source, uint64_t *position) {
    if (!grow_entries(archive))
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    TarEntry *entry = &archive->entries[archive->entry_count];
    memset(entry, 0, sizeof(*entry));
    ++archive->entry_count;
    entry->path = strdup(path);
    if (!entry->path)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    size_t path_size = strlen(path);
    entry->archive_path = malloc(path_size + (source->directory ? 2 : 1));
    if (!entry->archive_path)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memcpy(entry->archive_path, path, path_size);
    if (source->directory) entry->archive_path[path_size++] = '/';
    entry->archive_path[path_size] = 0;
    entry->size = source->directory ? 0 : source->size;
    entry->directory = source->directory;
    entry->start = *position;

    char name[100] = {0};
    char prefix[155] = {0};
    if (!split_ustar_path(entry->archive_path, name, prefix)) {
        char *record = pax_record("path", entry->archive_path);
        if (!record || !append_pax(&entry->pax, &entry->pax_size, record)) {
            free(record);
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        free(record);
    }
    if (entry->size > TAR_MAX_OCTAL_SIZE) {
        char value[32];
        snprintf(value, sizeof(value), "%llu",
                 (unsigned long long)entry->size);
        char *record = pax_record("size", value);
        if (!record || !append_pax(&entry->pax, &entry->pax_size, record)) {
            free(record);
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        free(record);
    }

    if (entry->pax) {
        entry->pax_data = *position + TAR_BLOCK_SIZE;
        *position = align_block(entry->pax_data + entry->pax_size);
    } else {
        entry->pax_data = UINT64_MAX;
    }
    entry->header = *position;
    entry->data = entry->header + TAR_BLOCK_SIZE;
    if (entry->size > UINT64_MAX - entry->data)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    entry->end = align_block(entry->data + entry->size);
    *position = entry->end;
    return 0;
}

static Result walk_tree(TarArchive *archive, const char *parent,
                        uint64_t *position) {
    for (size_t index = 0;; ++index) {
        NupTreeEntry child;
        if (!nupTreeList(archive->tree, parent, index, &child)) break;
        size_t parent_size = strlen(parent);
        size_t name_size = strlen(child.name);
        size_t total = parent_size + (parent_size ? 1 : 0) + name_size + 1;
        if (total > FS_MAX_PATH)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        char path[FS_MAX_PATH];
        memcpy(path, parent, parent_size);
        if (parent_size) path[parent_size++] = '/';
        memcpy(path + parent_size, child.name, name_size + 1);

        Result result = add_entry(archive, path, &child, position);
        if (R_FAILED(result)) return result;
        if (child.directory) {
            result = walk_tree(archive, path, position);
            if (R_FAILED(result)) return result;
        }
    }
    return 0;
}

Result tarPrepare(TarArchive **out_archive, NupArchive *tree) {
    if (!out_archive || !tree)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_archive = NULL;
    TarArchive *archive = calloc(1, sizeof(*archive));
    if (!archive) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    archive->tree = tree;
    uint64_t position = 0;
    Result result = walk_tree(archive, "", &position);
    if (R_SUCCEEDED(result) && position <= UINT64_MAX - TAR_END_SIZE)
        archive->size = position + TAR_END_SIZE;
    else if (R_SUCCEEDED(result))
        result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (R_FAILED(result)) {
        tarArchiveClose(archive);
        return result;
    }
    *out_archive = archive;
    return 0;
}

uint64_t tarArchiveSize(const TarArchive *archive) {
    return archive ? archive->size : 0;
}

static void write_octal(char *field, size_t size, uint64_t value) {
    snprintf(field, size, "%0*llo", (int)size - 1,
             (unsigned long long)value);
}

static void fallback_name(const TarEntry *entry, char name[100]) {
    const char *end = entry->archive_path + strlen(entry->archive_path);
    while (end > entry->archive_path && end[-1] == '/') --end;
    const char *start = end;
    while (start > entry->archive_path && start[-1] != '/') --start;
    size_t length = (size_t)(end - start);
    if (length > 98) {
        start = end - 98;
        length = 98;
    }
    memcpy(name, start, length);
    if (entry->directory && length < 100) name[length] = '/';
}

static void make_header(const TarEntry *entry, bool pax, TarHeader *header) {
    memset(header, 0, sizeof(*header));
    uint64_t size = pax ? entry->pax_size : entry->size;
    if (pax) {
        snprintf(header->name, sizeof(header->name), "PaxHeaders.0/kaidump");
    } else if (!split_ustar_path(entry->archive_path, header->name,
                                 header->prefix)) {
        fallback_name(entry, header->name);
    }
    write_octal(header->mode, sizeof(header->mode),
                pax || !entry->directory ? 0644 : 0755);
    write_octal(header->uid, sizeof(header->uid), 0);
    write_octal(header->gid, sizeof(header->gid), 0);
    write_octal(header->size, sizeof(header->size),
                size <= TAR_MAX_OCTAL_SIZE ? size : 0);
    write_octal(header->mtime, sizeof(header->mtime), 0);
    memset(header->checksum, ' ', sizeof(header->checksum));
    header->type = pax ? 'x' : entry->directory ? '5' : '0';
    memcpy(header->magic, "ustar", 5);
    memcpy(header->version, "00", 2);
    memcpy(header->user_name, "kaidump", 7);
    memcpy(header->group_name, "kaidump", 7);

    unsigned checksum = 0;
    const uint8_t *bytes = (const uint8_t *)header;
    for (size_t i = 0; i < sizeof(*header); ++i) checksum += bytes[i];
    snprintf(header->checksum, sizeof(header->checksum), "%06o", checksum);
    header->checksum[6] = 0;
    header->checksum[7] = ' ';
}

static size_t find_entry(const TarArchive *archive, uint64_t offset) {
    size_t low = 0;
    size_t high = archive->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (archive->entries[middle].end <= offset) low = middle + 1;
        else high = middle;
    }
    return low;
}

static size_t copy_part(void *output, size_t wanted, uint64_t offset,
                        uint64_t start, const void *source, size_t source_size) {
    if (offset < start || offset >= start + source_size) return 0;
    size_t within = (size_t)(offset - start);
    size_t amount = source_size - within > wanted
        ? wanted : source_size - within;
    memcpy(output, (const uint8_t *)source + within, amount);
    return amount;
}

Result tarArchiveRead(TarArchive *archive, uint64_t offset, void *buffer,
                      size_t size, size_t *bytes_read) {
    if (!archive || !buffer || !bytes_read)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *bytes_read = 0;
    if (offset >= archive->size || !size) return 0;
    if ((uint64_t)size > archive->size - offset)
        size = (size_t)(archive->size - offset);
    uint8_t *output = buffer;

    while (size) {
        size_t index = find_entry(archive, offset);
        if (index >= archive->entry_count) {
            memset(output, 0, size);
            *bytes_read += size;
            return 0;
        }
        TarEntry *entry = &archive->entries[index];
        size_t amount = 0;
        TarHeader header;
        if (entry->pax && offset < entry->pax_data) {
            make_header(entry, true, &header);
            amount = copy_part(output, size, offset, entry->start,
                               &header, sizeof(header));
        } else if (entry->pax && offset < entry->pax_data + entry->pax_size) {
            amount = copy_part(output, size, offset, entry->pax_data,
                               entry->pax, entry->pax_size);
        } else if (offset < entry->header) {
            amount = entry->header - offset > size
                ? size : (size_t)(entry->header - offset);
            memset(output, 0, amount);
        } else if (offset < entry->data) {
            make_header(entry, false, &header);
            amount = copy_part(output, size, offset, entry->header,
                               &header, sizeof(header));
        } else if (!entry->directory && offset < entry->data + entry->size) {
            size_t wanted = entry->data + entry->size - offset > size
                ? size : (size_t)(entry->data + entry->size - offset);
            Result result = nupTreeRead(archive->tree, entry->path,
                                        offset - entry->data, output,
                                        wanted, &amount);
            if (R_FAILED(result)) return result;
            if (amount != wanted) return io_error();
        } else {
            amount = entry->end - offset > size
                ? size : (size_t)(entry->end - offset);
            memset(output, 0, amount);
        }
        if (!amount) return io_error();
        output += amount;
        offset += amount;
        size -= amount;
        *bytes_read += amount;
    }
    return 0;
}

Result tarWriteDirect(const char *output_path, TarArchive *archive,
                      void *buffer, size_t buffer_size,
                      NupCancelCallback cancelled, void *cancel_user,
                      uint64_t *written_bytes) {
    if (!output_path || !archive || !buffer || !buffer_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (written_bytes) *written_bytes = 0;
    FILE *output = fopen(output_path, "wb+");
    if (!output) return io_error();
    void *output_buffer = malloc(1024 * 1024);
    if (output_buffer &&
        setvbuf(output, output_buffer, _IOFBF, 1024 * 1024) != 0) {
        free(output_buffer);
        output_buffer = NULL;
    }
    Result result = 0;
    if (ftruncate(fileno(output), (off_t)archive->size) != 0) result = io_error();
    uint64_t offset = 0;
    while (R_SUCCEEDED(result) && offset < archive->size) {
        size_t wanted = archive->size - offset > buffer_size
            ? buffer_size : (size_t)(archive->size - offset);
        size_t got = 0;
        result = tarArchiveRead(archive, offset, buffer, wanted, &got);
        if (R_SUCCEEDED(result) &&
            (got != wanted || fwrite(buffer, 1, got, output) != got))
            result = io_error();
        offset += got;
        if (written_bytes) *written_bytes = offset;
        if (R_SUCCEEDED(result) && cancelled && cancelled(cancel_user))
            result = TAR_CANCELED;
    }
    if (R_FAILED(result)) {
        fflush(output);
        ftruncate(fileno(output), (off_t)offset);
    }
    if (fclose(output) != 0 && R_SUCCEEDED(result)) result = io_error();
    free(output_buffer);
    return result;
}

void tarArchiveClose(TarArchive *archive) {
    if (!archive) return;
    for (size_t i = 0; i < archive->entry_count; ++i) {
        free(archive->entries[i].path);
        free(archive->entries[i].archive_path);
        free(archive->entries[i].pax);
    }
    free(archive->entries);
    free(archive);
}
