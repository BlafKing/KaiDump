#include "app_internal.h"

/* Title discovery and presentation metadata. */

void copy_display_text(char *out, size_t out_size, const char *input) {
    size_t at = 0;
    if (!out_size) return;
    while (*input && at + 1 < out_size) {
        u32 codepoint = 0;
        ssize_t consumed = decode_utf8(&codepoint, (const u8 *)input);
        if (consumed <= 0) {
            codepoint = '?';
            consumed = 1;
        }
        if (codepoint < 0x20 || codepoint == 0x7f) codepoint = ' ';
        if ((codepoint >= 0x2010 && codepoint <= 0x2015) ||
            codepoint == 0x2212) codepoint = '-';
        else if (codepoint == 0x2018 || codepoint == 0x2019)
            codepoint = '\'';
        else if (codepoint == 0x201c || codepoint == 0x201d)
            codepoint = '"';
        if (codepoint == 0x2026 && at + 3 < out_size) {
            out[at++] = '.';
            out[at++] = '.';
            out[at++] = '.';
        } else if (codepoint <= 0x7f) {
            out[at++] = (char)codepoint;
        } else {
            if (at + (size_t)consumed >= out_size) break;
            memcpy(out + at, input, (size_t)consumed);
            at += (size_t)consumed;
        }
        input += consumed;
    }
    while (at && out[at - 1] == ' ') --at;
    out[at] = 0;
}

void build_output_name(const Application *app, const char *extension,
                              char *output, size_t output_size) {
    const char *input = app->title[0] ? app->title : "Title unavailable";
    char safe_title[221];
    size_t at = 0;
    while (*input && at + 1 < sizeof(safe_title)) {
        u32 codepoint = 0;
        ssize_t consumed = decode_utf8(&codepoint, (const u8 *)input);
        if (consumed <= 0) {
            codepoint = '_';
            consumed = 1;
        }
        if (codepoint <= 0x7f) {
            char value = (char)codepoint;
            if (codepoint < 0x20 || codepoint == 0x7f ||
                strchr("<>:\"/\\|?*", value))
                value = '_';
            safe_title[at++] = value;
        } else {
            if (at + (size_t)consumed >= sizeof(safe_title)) break;
            memcpy(safe_title + at, input, (size_t)consumed);
            at += (size_t)consumed;
        }
        input += consumed;
    }
    while (at && (safe_title[at - 1] == ' ' || safe_title[at - 1] == '.'))
        --at;
    safe_title[at] = 0;
    if (!safe_title[0]) snprintf(safe_title, sizeof(safe_title), "Title");
    snprintf(output, output_size, "%s [%016llX]%s", safe_title,
             (unsigned long long)app->record.application_id,
             extension ? extension : "");
}

static Result load_control_from_readonly_ns(Application *app) {
    Service ns = {0};
    Service reader = {0};
    Result result = smGetService(&ns, "ns:ro");
    if (R_FAILED(result)) result = smGetService(&ns, "ns:am2");
    if (R_FAILED(result)) {
        log_result("control-readonly-service", result);
        return result;
    }
    result = serviceDispatch(&ns, 7989,
        .out_num_objects = 1,
        .out_objects = &reader,
    );
    serviceClose(&ns);
    if (R_FAILED(result)) {
        log_result("control-readonly-open", result);
        return result;
    }

    const size_t buffer_size = 0x24000;
    void *buffer = calloc(1, buffer_size);
    if (!buffer) {
        serviceClose(&reader);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    const struct {
        u8 source;
        u8 padding[7];
        u64 application_id;
    } input = {
        NsApplicationControlSource_Storage,
        {0},
        app->record.application_id,
    };
    result = serviceDispatchIn(&reader, 0, input,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, buffer_size } },
    );
    log_line("control-readonly title=%016llX result=0x%08x",
             (unsigned long long)app->record.application_id, result);
    if (R_SUCCEEDED(result)) title_from_nacp(app, (NacpStruct *)buffer);
    free(buffer);
    serviceClose(&reader);
    return R_SUCCEEDED(result) && app->title[0]
        ? 0 : (R_FAILED(result) ? result
                               : MAKERESULT(Module_Libnx, LibnxError_NotFound));
}

static void load_metadata(Application *app) {
    if (app->metadata_loaded || !g.ns_active) return;

    app->metadata_loaded = true;
    u64 actual_size = 0;
    memset(&g.control_data, 0, sizeof(g.control_data));
    app->metadata_result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage,
        app->record.application_id,
        &g.control_data,
        sizeof(g.control_data),
        &actual_size);
    if (R_FAILED(app->metadata_result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        memset(&g.control_data, 0, sizeof(g.control_data));
        app->metadata_result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage,
            app->record.application_id,
            &g.control_data,
            sizeof(g.control_data),
            0,
            1,
            &actual_size,
            &unknown);
        log_line("control2 title=%016llX acd=1 result=0x%08x size=%llu unknown=%u",
                 (unsigned long long)app->record.application_id,
                 app->metadata_result, (unsigned long long)actual_size, unknown);
    }
    log_line("control title=%016llX result=0x%08x size=%llu format=%u",
             (unsigned long long)app->record.application_id,
             app->metadata_result, (unsigned long long)actual_size,
             R_SUCCEEDED(app->metadata_result) ? g.control_data.nacp.titles_data_format : 0);
    if (R_FAILED(app->metadata_result) || actual_size < sizeof(NacpStruct)) {
        Result readonly_result = load_control_from_readonly_ns(app);
        if (R_SUCCEEDED(readonly_result)) {
            log_line("control-readonly-name title=%016llX name=%s",
                     (unsigned long long)app->record.application_id, app->title);
            return;
        }
        Result content_result = load_control_from_content(app);
        log_line("control-content title=%016llX result=0x%08x name=%s",
                 (unsigned long long)app->record.application_id,
                 content_result, app->title[0] ? app->title : "(unavailable)");
        return;
    }

    NacpLanguageEntry *language = NULL;
    Result language_result = nsGetApplicationDesiredLanguage(
        &g.control_data.nacp, &language);
    if (R_FAILED(language_result)) {
        language_result = nacpGetLanguageEntry(&g.control_data.nacp, &language);
    }
    if (!app->title[0] && R_SUCCEEDED(language_result) && language) {
        copy_display_text(app->title, sizeof(app->title), language->name);
    }
    copy_display_text(app->version, sizeof(app->version),
                      g.control_data.nacp.display_version);
    log_line("control-language title=%016llX result=0x%08x name=%s",
             (unsigned long long)app->record.application_id, language_result,
             app->title[0] ? app->title : "(unavailable)");
}

static void load_title_list(void) {
    g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    if (g.count <= 0 || hosversionBefore(20, 0, 0)) return;

    const size_t entry_bytes = (size_t)g.count * sizeof(NacpLanguageEntry);
    size_t buffer_size = entry_bytes + (size_t)g.count * sizeof(u64) +
                         sizeof(NsApplicationControlData);
    buffer_size = (buffer_size + 0xfff) & ~(size_t)0xfff;
    void *buffer = aligned_alloc(0x1000, buffer_size);
    u64 *ids = malloc((size_t)g.count * sizeof(*ids));
    if (!buffer || !ids) {
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        free(buffer);
        free(ids);
        log_result("title-list allocation", g.title_list_result);
        return;
    }
    memset(buffer, 0, buffer_size);
    for (s32 i = 0; i < g.count; ++i) ids[i] = g.applications[i].record.application_id;

    AsyncValue value = {0};
    g.title_list_result = nsListApplicationTitle2(
        &value, NsApplicationControlSource_Storage, ids, g.count, buffer, buffer_size);
    s32 offset = -1;
    u64 value_size = 0;
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueWait(&value, UINT64_MAX);
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueGetSize(&value, &value_size);
    if (R_SUCCEEDED(g.title_list_result) && value_size < entry_bytes)
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueGet(&value, &offset, sizeof(offset));

    if (R_SUCCEEDED(g.title_list_result) && offset >= 0 &&
        (size_t)offset <= buffer_size && entry_bytes <= buffer_size - (size_t)offset) {
        NacpLanguageEntry *entries = (NacpLanguageEntry *)((u8 *)buffer + offset);
        s32 named = 0;
        for (s32 i = 0; i < g.count; ++i)
            copy_display_text(g.applications[i].title,
                              sizeof(g.applications[i].title), entries[i].name);
        for (s32 i = 0; i < g.count; ++i)
            if (g.applications[i].title[0]) ++named;
        log_line("title-list supplied names=%d/%d", named, g.count);
    } else if (R_SUCCEEDED(g.title_list_result)) {
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    log_line("title-list result=0x%08x count=%d buffer=%zu value=%llu offset=%d",
             g.title_list_result, g.count, buffer_size,
             (unsigned long long)value_size, offset);
    if (serviceIsActive(&value.s)) asyncValueClose(&value);
    free(ids);
    free(buffer);
}

static int compare_applications(const void *left, const void *right) {
    const Application *a = left;
    const Application *b = right;
    if (!a->title[0] || !b->title[0]) {
        if (a->title[0] != b->title[0]) return a->title[0] ? -1 : 1;
    } else {
        const unsigned char *a_text = (const unsigned char *)a->title;
        const unsigned char *b_text = (const unsigned char *)b->title;
        while (*a_text && *b_text) {
            unsigned char a_value = *a_text;
            unsigned char b_value = *b_text;
            if (a_value < 0x80) a_value = (unsigned char)tolower(a_value);
            if (b_value < 0x80) b_value = (unsigned char)tolower(b_value);
            if (a_value != b_value) return a_value < b_value ? -1 : 1;
            ++a_text;
            ++b_text;
        }
        if (*a_text != *b_text) return *a_text ? 1 : -1;
        int exact = strcmp(a->title, b->title);
        if (exact) return exact;
    }
    if (a->record.application_id == b->record.application_id) return 0;
    return a->record.application_id < b->record.application_id ? -1 : 1;
}

static void sort_applications(void) {
    if (g.count > 1)
        qsort(g.applications, (size_t)g.count,
              sizeof(g.applications[0]), compare_applications);
    log_line("application-sort count=%d", g.count);
}

void load_visible_metadata(void) {
    if (g.count <= 0) return;
    s32 first = (g.selected / ROWS_PER_PAGE) * ROWS_PER_PAGE;
    s32 last = first + ROWS_PER_PAGE;
    if (last > g.count) last = g.count;
    for (s32 i = first; i < last; ++i) load_metadata(&g.applications[i]);
}

static Result discover_accessible_applications(void) {
    Result result = ncmInitialize();
    log_result("inventory-ncm-initialize", result);
    if (R_FAILED(result)) return result;

    const u8 storages[] = {
        NcmStorageId_BuiltInUser,
        NcmStorageId_SdCard,
        NcmStorageId_GameCard,
    };
    bool opened_any = false;
    Result last = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    for (size_t storage_index = 0;
         storage_index < sizeof(storages); ++storage_index) {
        NcmContentMetaDatabase database = {0};
        last = ncmOpenContentMetaDatabase(&database, storages[storage_index]);
        log_line("inventory-open storage=%u result=0x%08x",
                 storages[storage_index], last);
        if (R_FAILED(last)) continue;
        opened_any = true;

        NcmContentMetaKey keys[MAX_APPLICATIONS] = {0};
        s32 total = 0;
        s32 written = 0;
        last = ncmContentMetaDatabaseList(
            &database, &total, &written, keys, MAX_APPLICATIONS,
            NcmContentMetaType_Application, 0, 0, UINT64_MAX,
            NcmContentInstallType_Full);
        log_line("inventory-list storage=%u result=0x%08x total=%d written=%d",
                 storages[storage_index], last, total, written);
        if (R_SUCCEEDED(last)) {
            if (written > MAX_APPLICATIONS) written = MAX_APPLICATIONS;
            for (s32 i = 0; i < written && g.count < MAX_APPLICATIONS; ++i) {
                bool duplicate = false;
                for (s32 existing = 0; existing < g.count; ++existing) {
                    if (g.applications[existing].record.application_id == keys[i].id) {
                        if (storages[storage_index] == NcmStorageId_GameCard) {
                            g.applications[existing].base_storage =
                                NcmStorageId_GameCard;
                            g.applications[existing].base_version =
                                keys[i].version;
                        }
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                Application *app = &g.applications[g.count++];
                app->record.application_id = keys[i].id;
                app->base_storage = storages[storage_index];
                app->base_version = keys[i].version;
                log_line("inventory-add title=%016llX storage=%u version=%u",
                         (unsigned long long)keys[i].id,
                         storages[storage_index], keys[i].version);
            }
        }
        ncmContentMetaDatabaseClose(&database);
    }
    ncmExit();
    if (g.count > 0) return 0;
    return opened_any ? last : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

void refresh_applications(void) {
    close_ns();
    uiClearThumbnails();
    memset(g.applications, 0, sizeof(g.applications));
    memset(g.grid_icon_loaded, 0, sizeof(g.grid_icon_loaded));
    g.grid_icon_first = -1;
    g.count = 0;
    g.selected = 0;
    g.meta_count = 0;
    g.list_result = 0;
    g.screen = SCREEN_TITLES;

    g.init_result = nsInitialize();
    log_result("nsInitialize", g.init_result);
    if (R_FAILED(g.init_result)) return;
    g.ns_active = true;

    g.list_result = discover_accessible_applications();
    if (R_FAILED(g.list_result)) {
        g.count = 0;
        return;
    }
    log_line("accessible-application-list result=0x%08x count=%d",
             g.list_result, g.count);
    load_title_list();
    for (s32 i = 0; i < g.count; ++i) {
        if (!g.applications[i].title[0])
            load_metadata(&g.applications[i]);
    }
    sort_applications();
    load_visible_metadata();
}

const char *storage_name(u8 id) {
    switch (id) {
        case NcmStorageId_GameCard: return "Game card";
        case NcmStorageId_BuiltInSystem: return "System";
        case NcmStorageId_BuiltInUser: return "Internal";
        case NcmStorageId_SdCard: return "SD card";
        default: return "Unknown";
    }
}

static void add_content_choice(ContentKind kind, s32 meta_index,
                               s32 update_index, bool selected) {
    if (g.choice_count >= MAX_META_STATUS) return;
    g.choices[g.choice_count++] = (ContentChoice){
        .kind = kind,
        .meta_index = meta_index,
        .update_index = update_index,
        .selected = selected,
    };
}

static void build_content_choices(void) {
    g.choice_count = 0;
    s32 base = -1;
    s32 update = -1;
    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type == NcmContentMetaType_Application)
            base = i;
        else if (g.meta[i].meta_type == NcmContentMetaType_Patch)
            update = i;
    }

    bool game_card = base >= 0 &&
                     g.applications[g.selected].base_storage ==
                         NcmStorageId_GameCard;
    if (game_card) {
        /* An update's RomFS is a delta on top of the base RomFS, so it can
           only be read merged with the base. Offer the merged game, plus the
           untouched game card as an option. A card can also ship with a
           revision at or above the installed update; the console ignores
           such an update, so only the card is offered then. */
        const bool update_newer = update >= 0 &&
            g.meta[update].version > g.meta[base].version;
        if (update >= 0 && !update_newer)
            log_line("content-update-superseded update=%u base=%u",
                     g.meta[update].version, g.meta[base].version);
        if (update_newer) add_content_choice(CONTENT_GAME, base, update, true);
        add_content_choice(CONTENT_BASE, base, -1, !update_newer);
    } else if (base >= 0) {
        add_content_choice(CONTENT_GAME, base, update, true);
    } else if (update >= 0) {
        add_content_choice(CONTENT_GAME, update, update, true);
    }

    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type == NcmContentMetaType_AddOnContent ||
            g.meta[i].meta_type == NcmContentMetaType_DataPatch)
            add_content_choice(CONTENT_DLC, i, -1, false);
    }
    log_line("content-choices count=%d base=%d update=%d game-card=%u",
             g.choice_count, base, update, game_card);
}

static void load_selected_icon(Application *app) {
    uiClearImage();
    g.selected_icon_loaded = false;
    if (!g.ns_active) return;

    if (g.control_icon_size) {
        g.selected_icon_loaded = uiSetJpegImage(
            g.control_icon, g.control_icon_size);
        log_line("details-icon title=%016llX source=content size=%zu loaded=%u",
                 (unsigned long long)app->record.application_id,
                 g.control_icon_size, g.selected_icon_loaded);
        if (g.selected_icon_loaded) return;
    }

    u64 actual_size = 0;
    Result result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, app->record.application_id,
        &g.control_data, sizeof(g.control_data), &actual_size);
    if (R_FAILED(result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage, app->record.application_id,
            &g.control_data, sizeof(g.control_data), 0, 1,
            &actual_size, &unknown);
    }
    size_t icon_size = actual_size > sizeof(NacpStruct)
        ? (size_t)(actual_size - sizeof(NacpStruct)) : 0;
    if (icon_size > sizeof(g.control_data.icon))
        icon_size = sizeof(g.control_data.icon);
    bool loaded = R_SUCCEEDED(result) &&
                  uiSetJpegImage(g.control_data.icon, icon_size);
    g.selected_icon_loaded = loaded;
    log_line("details-icon title=%016llX source=ns result=0x%08x size=%zu loaded=%u",
             (unsigned long long)app->record.application_id,
             result, icon_size, loaded);
}

static bool load_grid_icon(Application *app, int slot) {
    u64 actual_size = 0;
    Result result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, app->record.application_id,
        &g.control_data, sizeof(g.control_data), &actual_size);
    if (R_FAILED(result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage, app->record.application_id,
            &g.control_data, sizeof(g.control_data), 0, 1,
            &actual_size, &unknown);
    }
    size_t icon_size = actual_size > sizeof(NacpStruct)
        ? (size_t)(actual_size - sizeof(NacpStruct)) : 0;
    if (icon_size > sizeof(g.control_data.icon))
        icon_size = sizeof(g.control_data.icon);
    bool loaded = R_SUCCEEDED(result) && icon_size &&
                  uiSetJpegThumbnail(slot, g.control_data.icon, icon_size);
    const char *source = "ns";

    if (!loaded) {
        result = load_control_from_content(app);
        source = "content";
        loaded = R_SUCCEEDED(result) && g.control_icon_size &&
                 uiSetJpegThumbnail(slot, g.control_icon,
                                    g.control_icon_size);
    }
    log_line("grid-icon slot=%d title=%016llX source=%s result=0x%08x size=%zu loaded=%u",
             slot, (unsigned long long)app->record.application_id,
             source, result,
             source[0] == 'n' ? icon_size : g.control_icon_size, loaded);
    return loaded;
}

void load_grid_icons(void) {
    if (!g.title_grid || g.count <= 0) return;
    s32 first = (g.selected / ROWS_PER_PAGE) * ROWS_PER_PAGE;
    if (g.grid_icon_first == first) return;

    uiClearThumbnails();
    memset(g.grid_icon_loaded, 0, sizeof(g.grid_icon_loaded));
    g.grid_icon_first = first;
    s32 last = first + ROWS_PER_PAGE;
    if (last > g.count) last = g.count;
    for (s32 i = first; i < last; ++i) {
        int slot = i - first;
        g.grid_icon_loaded[slot] = load_grid_icon(&g.applications[i], slot);
    }
}

void load_details(void) {
    if (g.count <= 0) return;
    Application *app = &g.applications[g.selected];
    load_metadata(app);
    g.meta_count = 0;
    g.details_result = nsListApplicationContentMetaStatus(
        app->record.application_id, 0, g.meta, MAX_META_STATUS, &g.meta_count);
    if (R_FAILED(g.details_result)) g.meta_count = 0;
    if (g.meta_count > MAX_META_STATUS) g.meta_count = MAX_META_STATUS;
    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type == NcmContentMetaType_Application &&
            g.meta[i].storageID == NcmStorageId_None && app->base_storage) {
            log_line("details-base-storage title=%016llX ns=%u inventory=%u ns-version=%u inventory-version=%u",
                     (unsigned long long)app->record.application_id,
                     g.meta[i].storageID, app->base_storage,
                     g.meta[i].version, app->base_version);
            g.meta[i].storageID = app->base_storage;
            /* NS does not know where this base lives, so its version may be
               stale too; NCM lookups need the version actually on storage. */
            g.meta[i].version = app->base_version;
        }
    }
    Result current_control = load_control_from_content(app);
    log_line("details-current-control title=%016llX result=0x%08x version=%s icon=%zu",
             (unsigned long long)app->record.application_id,
             current_control, app->version[0] ? app->version : "(unavailable)",
             g.control_icon_size);
    g.content_cursor = 0;
    build_content_choices();
    load_selected_icon(app);
    g.screen = SCREEN_DETAILS;
    log_line("details title=%016llX result=0x%08x count=%d",
             (unsigned long long)app->record.application_id,
             g.details_result, g.meta_count);
}
