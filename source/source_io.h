#ifndef KAIDUMP_SOURCE_IO_H
#define KAIDUMP_SOURCE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <switch.h>

void sourceIoResetError(void);
bool sourceIoHadReadError(void);
Result sourceReadExact(FILE *file, const char *path, uint64_t offset,
                       void *buffer, size_t size);

#endif
