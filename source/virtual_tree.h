#pragma once

#include <switch.h>

#include "nup.h"

Result virtualTreeMount(const char *device, NupArchive *archive);
void virtualTreeUnmount(void);
