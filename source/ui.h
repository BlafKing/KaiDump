#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720
#define UI_THUMBNAIL_COUNT 10
#define UI_RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(a) << 24))

Result uiInitialize(void);
void uiExit(void);
void uiPumpEvents(void);

bool uiBeginFrame(uint32_t color);
void uiEndFrame(void);

bool uiSetJpegImage(const void *data, size_t size);
void uiClearImage(void);
void uiDrawImage(int x, int y, int width, int height);
bool uiSetJpegThumbnail(int index, const void *data, size_t size);
void uiClearThumbnails(void);
void uiDrawThumbnail(int index, int x, int y, int width, int height);

void uiFillRect(int x, int y, int width, int height, uint32_t color);
void uiFillRoundedRect(int x, int y, int width, int height, int radius,
                       uint32_t color);
void uiStrokeRoundedRect(int x, int y, int width, int height, int radius,
                         int thickness, uint32_t color);
void uiDrawLine(int x1, int y1, int x2, int y2, int thickness,
                uint32_t color);

void uiDrawText(int x, int y, int size, uint32_t color, const char *text);
void uiDrawTextf(int x, int y, int size, uint32_t color,
                 const char *format, ...);
void uiDrawTextEllipsized(int x, int y, int size, uint32_t color,
                          const char *text, int max_width);
int uiTextWidth(int size, const char *text);
