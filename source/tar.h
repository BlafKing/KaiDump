#pragma once

#include <stddef.h>
#include <stdint.h>

#include <switch.h>

#include "nup.h"

typedef struct TarArchive TarArchive;

Result tarPrepare(TarArchive **out_archive, NupArchive *tree);
uint64_t tarArchiveSize(const TarArchive *archive);
Result tarArchiveRead(TarArchive *archive, uint64_t offset, void *buffer,
                      size_t size, size_t *bytes_read);
Result tarWriteDirect(const char *output_path, TarArchive *archive,
                      void *buffer, size_t buffer_size,
                      NupCancelCallback cancelled, void *cancel_user,
                      uint64_t *written_bytes);
void tarArchiveClose(TarArchive *archive);
