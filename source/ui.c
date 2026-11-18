#include "ui.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <SDL.h>
#include <SDL_ttf.h>

#define UI_FONT_COUNT PlSharedFontType_Total
#define UI_FONT_CACHE_SIZE 20

typedef struct {
    int size;
    TTF_Font *fonts[UI_FONT_COUNT];
} FontSet;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *image;
    int image_width;
    int image_height;
    SDL_Texture *thumbnails[UI_THUMBNAIL_COUNT];
    int thumbnail_widths[UI_THUMBNAIL_COUNT];
    int thumbnail_heights[UI_THUMBNAIL_COUNT];
    PlFontData font_data[UI_FONT_COUNT];
    bool font_available[UI_FONT_COUNT];
    FontSet font_cache[UI_FONT_CACHE_SIZE];
    bool sdl_ready;
    bool ttf_ready;
    bool pl_ready;
} UiState;

static UiState s_ui;

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegError;

static void jpeg_error_exit(j_common_ptr info) {
    JpegError *error = (JpegError *)info->err;
    longjmp(error->jump, 1);
}

static SDL_Color sdl_color(uint32_t color) {
    return (SDL_Color){
        color & 0xff,
        (color >> 8) & 0xff,
        (color >> 16) & 0xff,
        (color >> 24) & 0xff,
    };
}

static void set_draw_color(uint32_t color) {
    SDL_Color value = sdl_color(color);
    SDL_SetRenderDrawColor(s_ui.renderer, value.r, value.g, value.b, value.a);
}

static FontSet *font_set_for_size(int size) {
    FontSet *empty = NULL;
    for (int i = 0; i < UI_FONT_CACHE_SIZE; ++i) {
        if (s_ui.font_cache[i].size == size) return &s_ui.font_cache[i];
        if (!s_ui.font_cache[i].size && !empty) empty = &s_ui.font_cache[i];
    }
    if (!empty) empty = &s_ui.font_cache[UI_FONT_CACHE_SIZE - 1];

    for (int i = 0; i < UI_FONT_COUNT; ++i) {
        if (empty->fonts[i]) TTF_CloseFont(empty->fonts[i]);
        empty->fonts[i] = NULL;
        if (!s_ui.font_available[i]) continue;
        SDL_RWops *memory = SDL_RWFromConstMem(s_ui.font_data[i].address,
                                               s_ui.font_data[i].size);
        if (memory) empty->fonts[i] = TTF_OpenFontRW(memory, 1, size);
    }
    empty->size = size;
    return empty;
}

static TTF_Font *font_for_codepoint(FontSet *set, uint32_t codepoint) {
    for (int i = 0; i < UI_FONT_COUNT; ++i) {
        if (set->fonts[i] && TTF_GlyphIsProvided32(set->fonts[i], codepoint))
            return set->fonts[i];
    }
    return set->fonts[PlSharedFontType_Standard];
}

static int next_codepoint(const char **text, uint32_t *codepoint,
                          const char **start) {
    if (!**text) return 0;
    if (start) *start = *text;
    ssize_t consumed = decode_utf8(codepoint, (const uint8_t *)*text);
    if (consumed <= 0) {
        *codepoint = '?';
        consumed = 1;
    }
    *text += consumed;
    return (int)consumed;
}

static int glyph_width(TTF_Font *font, uint32_t codepoint) {
    int advance = 0;
    if (!font || TTF_GlyphMetrics32(font, codepoint, NULL, NULL, NULL, NULL,
                                    &advance) < 0) {
        return 0;
    }
    return advance;
}

static int text_width_internal(FontSet *set, const char *text) {
    int width = 0;
    while (*text) {
        uint32_t codepoint;
        next_codepoint(&text, &codepoint, NULL);
        if (codepoint == '\n') break;
        width += glyph_width(font_for_codepoint(set, codepoint), codepoint);
    }
    return width;
}

static void draw_run(TTF_Font *font, const char *text, size_t length,
                     int x, int y, uint32_t color, int *width) {
    if (!font || !length) return;
    char run[512];
    if (length >= sizeof(run)) length = sizeof(run) - 1;
    memcpy(run, text, length);
    run[length] = 0;

    SDL_Color value = sdl_color(color);
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, run, value);
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(s_ui.renderer, surface);
    if (texture) {
        SDL_Rect destination = {x, y, surface->w, surface->h};
        SDL_RenderCopy(s_ui.renderer, texture, NULL, &destination);
        SDL_DestroyTexture(texture);
    }
    if (width) *width = surface->w;
    SDL_FreeSurface(surface);
}

static void draw_text_internal(int x, int y, int size, uint32_t color,
                               const char *text, int max_width,
                               bool ellipsize) {
    if (!text || !s_ui.renderer) return;
    FontSet *set = font_set_for_size(size);
    int origin_x = x;
    int cursor_x = x;
    int line_height = set->fonts[PlSharedFontType_Standard]
        ? TTF_FontLineSkip(set->fonts[PlSharedFontType_Standard]) : size + 4;
    int ellipsis_width = ellipsize ? text_width_internal(set, "...") : 0;

    while (*text) {
        uint32_t codepoint;
        const char *codepoint_start;
        const char *after = text;
        next_codepoint(&after, &codepoint, &codepoint_start);
        if (codepoint == '\n') {
            text = after;
            cursor_x = origin_x;
            y += line_height;
            continue;
        }

        TTF_Font *font = font_for_codepoint(set, codepoint);
        const char *run_start = text;
        const char *run_end = after;
        int run_width = glyph_width(font, codepoint);
        const char *scan = after;
        while (*scan) {
            const char *next = scan;
            uint32_t next_value;
            next_codepoint(&next, &next_value, NULL);
            if (next_value == '\n' || font_for_codepoint(set, next_value) != font)
                break;
            int advance = glyph_width(font, next_value);
            if (max_width > 0 && cursor_x - origin_x + run_width + advance >
                max_width - (ellipsize ? ellipsis_width : 0)) break;
            run_width += advance;
            run_end = next;
            scan = next;
        }

        bool has_more = *run_end != 0;
        if (max_width > 0 && cursor_x - origin_x + run_width >
            max_width - ((ellipsize && has_more) ? ellipsis_width : 0)) {
            if (ellipsize)
                draw_run(set->fonts[PlSharedFontType_Standard], "...", 3,
                         cursor_x, y, color, NULL);
            break;
        }

        int rendered_width = 0;
        draw_run(font, run_start, (size_t)(run_end - run_start),
                 cursor_x, y, color, &rendered_width);
        cursor_x += rendered_width ? rendered_width : run_width;
        text = run_end;

        if (max_width > 0 && ellipsize && *text && *text != '\n' &&
            cursor_x - origin_x + ellipsis_width >= max_width) {
            draw_run(set->fonts[PlSharedFontType_Standard], "...", 3,
                     cursor_x, y, color, NULL);
            break;
        }
    }
}

Result uiInitialize(void) {
    memset(&s_ui, 0, sizeof(s_ui));

    Result result = plInitialize(PlServiceType_User);
    if (R_FAILED(result)) return result;
    s_ui.pl_ready = true;

    for (int i = 0; i < UI_FONT_COUNT; ++i) {
        result = plGetSharedFontByType(&s_ui.font_data[i], (PlSharedFontType)i);
        s_ui.font_available[i] = R_SUCCEEDED(result);
    }
    if (!s_ui.font_available[PlSharedFontType_Standard]) {
        uiExit();
        return result;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        uiExit();
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    }
    s_ui.sdl_ready = true;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");
    if (TTF_Init() < 0) {
        uiExit();
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    }
    s_ui.ttf_ready = true;

    s_ui.window = SDL_CreateWindow("KaiDump", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, UI_WIDTH, UI_HEIGHT,
                                  SDL_WINDOW_SHOWN);
    if (!s_ui.window) {
        uiExit();
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    }
    s_ui.renderer = SDL_CreateRenderer(s_ui.window, -1,
                                      SDL_RENDERER_ACCELERATED |
                                      SDL_RENDERER_PRESENTVSYNC);
    if (!s_ui.renderer)
        s_ui.renderer = SDL_CreateRenderer(s_ui.window, -1,
                                           SDL_RENDERER_SOFTWARE);
    if (!s_ui.renderer) {
        uiExit();
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    }
    SDL_SetRenderDrawBlendMode(s_ui.renderer, SDL_BLENDMODE_BLEND);
    return 0;
}

void uiExit(void) {
    for (int i = 0; i < UI_FONT_CACHE_SIZE; ++i) {
        for (int font = 0; font < UI_FONT_COUNT; ++font) {
            if (s_ui.font_cache[i].fonts[font])
                TTF_CloseFont(s_ui.font_cache[i].fonts[font]);
        }
    }
    uiClearImage();
    uiClearThumbnails();
    if (s_ui.renderer) SDL_DestroyRenderer(s_ui.renderer);
    if (s_ui.window) SDL_DestroyWindow(s_ui.window);
    if (s_ui.ttf_ready) TTF_Quit();
    if (s_ui.sdl_ready) SDL_Quit();
    if (s_ui.pl_ready) plExit();
    memset(&s_ui, 0, sizeof(s_ui));
}

static SDL_Texture *create_jpeg_texture(const void *data, size_t size,
                                        int *width, int *height) {
    if (!s_ui.renderer || !data || !size) return false;

    struct jpeg_decompress_struct decoder;
    JpegError error;
    memset(&decoder, 0, sizeof(decoder));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, data, size);
    if (jpeg_read_header(&decoder, true) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, (int)decoder.output_width, (int)decoder.output_height,
        24, SDL_PIXELFORMAT_RGB24);
    if (!surface) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row = (u8 *)surface->pixels +
                       decoder.output_scanline * surface->pitch;
        jpeg_read_scanlines(&decoder, &row, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);

    SDL_Surface *rounded = SDL_ConvertSurfaceFormat(
        surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rounded) return false;

    int radius = (rounded->w < rounded->h ? rounded->w : rounded->h) / 18;
    if (radius < 4) radius = 4;
    for (int y = 0; y < rounded->h; ++y) {
        for (int x = 0; x < rounded->w; ++x) {
            int corner_x = x < radius ? radius - x - 1
                : (x >= rounded->w - radius ? x - (rounded->w - radius) : 0);
            int corner_y = y < radius ? radius - y - 1
                : (y >= rounded->h - radius ? y - (rounded->h - radius) : 0);
            if (corner_x && corner_y &&
                corner_x * corner_x + corner_y * corner_y > radius * radius) {
                u8 *pixel = (u8 *)rounded->pixels + y * rounded->pitch + x * 4;
                pixel[3] = 0;
            }
        }
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(s_ui.renderer, rounded);
    if (width) *width = rounded->w;
    if (height) *height = rounded->h;
    SDL_FreeSurface(rounded);
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_ScaleModeBest);
    return texture;
}

bool uiSetJpegImage(const void *data, size_t size) {
    uiClearImage();
    s_ui.image = create_jpeg_texture(
        data, size, &s_ui.image_width, &s_ui.image_height);
    return s_ui.image != NULL;
}

void uiClearImage(void) {
    if (s_ui.image) SDL_DestroyTexture(s_ui.image);
    s_ui.image = NULL;
    s_ui.image_width = 0;
    s_ui.image_height = 0;
}

static void draw_texture(SDL_Texture *texture, int image_width,
                         int image_height, int x, int y,
                         int width, int height) {
    if (!s_ui.renderer || !texture || width <= 0 || height <= 0) return;
    int draw_width = width;
    int draw_height = height;
    if (image_width > 0 && image_height > 0) {
        if ((long long)width * image_height <=
            (long long)height * image_width) {
            draw_height = width * image_height / image_width;
        } else {
            draw_width = height * image_width / image_height;
        }
    }
    SDL_Rect destination = {
        x + (width - draw_width) / 2,
        y + (height - draw_height) / 2,
        draw_width,
        draw_height,
    };
    SDL_RenderCopy(s_ui.renderer, texture, NULL, &destination);
}

void uiDrawImage(int x, int y, int width, int height) {
    draw_texture(s_ui.image, s_ui.image_width, s_ui.image_height,
                 x, y, width, height);
}

bool uiSetJpegThumbnail(int index, const void *data, size_t size) {
    if (index < 0 || index >= UI_THUMBNAIL_COUNT) return false;
    if (s_ui.thumbnails[index]) SDL_DestroyTexture(s_ui.thumbnails[index]);
    s_ui.thumbnails[index] = create_jpeg_texture(
        data, size, &s_ui.thumbnail_widths[index],
        &s_ui.thumbnail_heights[index]);
    return s_ui.thumbnails[index] != NULL;
}

void uiClearThumbnails(void) {
    for (int i = 0; i < UI_THUMBNAIL_COUNT; ++i) {
        if (s_ui.thumbnails[i]) SDL_DestroyTexture(s_ui.thumbnails[i]);
        s_ui.thumbnails[i] = NULL;
        s_ui.thumbnail_widths[i] = 0;
        s_ui.thumbnail_heights[i] = 0;
    }
}

void uiDrawThumbnail(int index, int x, int y, int width, int height) {
    if (index < 0 || index >= UI_THUMBNAIL_COUNT) return;
    draw_texture(s_ui.thumbnails[index], s_ui.thumbnail_widths[index],
                 s_ui.thumbnail_heights[index], x, y, width, height);
}

void uiPumpEvents(void) {
    SDL_PumpEvents();
}

bool uiBeginFrame(uint32_t color) {
    if (!s_ui.renderer) return false;
    set_draw_color(color);
    return SDL_RenderClear(s_ui.renderer) == 0;
}

void uiEndFrame(void) {
    if (s_ui.renderer) SDL_RenderPresent(s_ui.renderer);
}

void uiFillRect(int x, int y, int width, int height, uint32_t color) {
    if (!s_ui.renderer || width <= 0 || height <= 0) return;
    SDL_Rect rectangle = {x, y, width, height};
    set_draw_color(color);
    SDL_RenderFillRect(s_ui.renderer, &rectangle);
}

void uiFillRoundedRect(int x, int y, int width, int height, int radius,
                       uint32_t color) {
    if (!s_ui.renderer || width <= 0 || height <= 0) return;
    if (radius < 0) radius = 0;
    int maximum = (width < height ? width : height) / 2;
    if (radius > maximum) radius = maximum;
    set_draw_color(color);
    if (!radius) {
        SDL_Rect rectangle = {x, y, width, height};
        SDL_RenderFillRect(s_ui.renderer, &rectangle);
        return;
    }

    for (int row = 0; row < height; ++row) {
        int inset = 0;
        if (row < radius || row >= height - radius) {
            int dy = row < radius ? radius - row - 1
                                  : row - (height - radius);
            int inside = radius * radius - dy * dy;
            inset = radius - (int)sqrtf((float)(inside > 0 ? inside : 0));
        }
        SDL_RenderDrawLine(s_ui.renderer, x + inset, y + row,
                           x + width - inset - 1, y + row);
    }
}

void uiStrokeRoundedRect(int x, int y, int width, int height, int radius,
                         int thickness, uint32_t color) {
    if (!s_ui.renderer || thickness <= 0) return;
    set_draw_color(color);
    for (int offset = 0; offset < thickness; ++offset) {
        SDL_Rect rectangle = {x + offset, y + offset,
                              width - offset * 2, height - offset * 2};
        SDL_RenderDrawRect(s_ui.renderer, &rectangle);
    }
    (void)radius;
}

void uiDrawLine(int x1, int y1, int x2, int y2, int thickness,
                uint32_t color) {
    if (!s_ui.renderer || thickness <= 0) return;
    set_draw_color(color);
    int first = -(thickness / 2);
    int last = first + thickness;
    for (int offset = first; offset < last; ++offset) {
        SDL_RenderDrawLine(s_ui.renderer, x1 + offset, y1,
                           x2 + offset, y2);
        SDL_RenderDrawLine(s_ui.renderer, x1, y1 + offset,
                           x2, y2 + offset);
    }
}

void uiDrawText(int x, int y, int size, uint32_t color, const char *text) {
    draw_text_internal(x, y, size, color, text, 0, false);
}

void uiDrawTextf(int x, int y, int size, uint32_t color,
                 const char *format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    uiDrawText(x, y, size, color, text);
}

void uiDrawTextEllipsized(int x, int y, int size, uint32_t color,
                          const char *text, int max_width) {
    draw_text_internal(x, y, size, color, text, max_width, true);
}

int uiTextWidth(int size, const char *text) {
    return text_width_internal(font_set_for_size(size), text);
}
