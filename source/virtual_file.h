#pragma once

#include <stddef.h>
#include <stdint.h>

#include <switch.h>

typedef Result (*VirtualFileReadCallback)(void *source, uint64_t offset,
                                          void *buffer, size_t size,
                                          size_t *bytes_read);

Result virtualFileMount(const char *device, const char *name, uint64_t size,
                        void *source, VirtualFileReadCallback read_source);
void virtualFileUnmount(void);

