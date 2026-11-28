#include "app_internal.h"

/* Process-wide services, diagnostics, and compatibility checks. */

void refresh_ftp_availability(void) {
    u32 address = 0;
    g.ftp_network_result = ftpGetLocalAddress(&address);
    g.ftp_available = R_SUCCEEDED(g.ftp_network_result);
    log_line("ftp-network-check result=0x%08x address=%u.%u.%u.%u",
             g.ftp_network_result,
             ((const u8 *)&address)[0], ((const u8 *)&address)[1],
             ((const u8 *)&address)[2], ((const u8 *)&address)[3]);
}

bool g_capture_control_icons;
ControlIcon g_control_icons[MAX_CONTROL_ICONS];
int g_control_icon_count;

void free_control_icons(void) {
    for (int i = 0; i < g_control_icon_count; ++i) free(g_control_icons[i].data);
    memset(g_control_icons, 0, sizeof(g_control_icons));
    g_control_icon_count = 0;
}

static void capture_control_icons(void) {
    free_control_icons();
    DIR *directory = opendir("kdctl:/");
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           g_control_icon_count < MAX_CONTROL_ICONS) {
        size_t length = strlen(entry->d_name);
        if (length < 10 || strncmp(entry->d_name, "icon_", 5) ||
            strcmp(entry->d_name + length - 4, ".dat")) continue;
        ControlIcon *icon = &g_control_icons[g_control_icon_count];
        const size_t language_length = length - 9;
        if (language_length >= sizeof(icon->language)) continue;
        char path[FS_MAX_PATH];
        int written = snprintf(path, sizeof(path), "kdctl:/%s", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        struct stat file_info;
        if (stat(path, &file_info) != 0 || file_info.st_size <= 0 ||
            file_info.st_size > 8 * 1024 * 1024) continue;
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        icon->data = malloc((size_t)file_info.st_size);
        if (icon->data)
            icon->size = fread(icon->data, 1, (size_t)file_info.st_size, file);
        fclose(file);
        if (!icon->data || icon->size != (size_t)file_info.st_size) {
            free(icon->data);
            memset(icon, 0, sizeof(*icon));
            continue;
        }
        memcpy(icon->language, entry->d_name + 5, language_length);
        icon->language[language_length] = 0;
        ++g_control_icon_count;
    }
    closedir(directory);
    log_line("control-icons captured=%d", g_control_icon_count);
}

size_t load_control_icon(void) {
    if (g_capture_control_icons) capture_control_icons();
    DIR *directory = opendir("kdctl:/");
    if (!directory) return 0;

    size_t icon_size = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 10 || strncmp(entry->d_name, "icon_", 5) ||
            strcmp(entry->d_name + length - 4, ".dat")) continue;

        char path[FS_MAX_PATH];
        int written = snprintf(path, sizeof(path), "kdctl:/%s", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        struct stat file_info;
        if (stat(path, &file_info) != 0 || file_info.st_size <= 0 ||
            file_info.st_size > 8 * 1024 * 1024) continue;
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        u8 *icon = malloc((size_t)file_info.st_size);
        if (!icon) {
            fclose(file);
            break;
        }
        icon_size = fread(icon, 1, (size_t)file_info.st_size, file);
        bool jpeg = icon_size == (size_t)file_info.st_size &&
                    icon_size >= 2 && icon[0] == 0xff && icon[1] == 0xd8;
        fclose(file);
        log_line("control-icon file=%s size=%zu jpeg=%u",
                 entry->d_name, icon_size, jpeg);
        if (jpeg) {
            g.control_icon = icon;
            break;
        }
        free(icon);
        icon_size = 0;
    }
    closedir(directory);
    return icon_size;
}

static Mutex g_log_mutex;

void log_line(const char *format, ...) {
    if (!g_log) return;
    mutexLock(&g_log_mutex);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    mutexUnlock(&g_log_mutex);
}

const char *result_name(Result result) {
    switch (R_VALUE(result)) {
        case 0: return "Success";
        case R_VALUE(MAKERESULT(Module_Libnx, LibnxError_NotInitialized)):
            return "LibnxNotInitialized";
        case 0xe401: return "InvalidHandle";
        case 0x408: return "ProgramLocationNotFound";
        case 0xc08: return "RegisteredProgramLocationNotFound";
        case 0x1408: return "DebugProgramLocationNotFound";
        case 0x7d202: return "PartitionNotFound";
        case R_VALUE(MAKERESULT(Module_Libnx, KAIDUMP_ERROR_DATA_IN_UPDATE)):
            return "DataOnlyInUpdate";
        case 0x7d802: return "NcaExternalKeyNotFound";
        case 0x1fb402: return "RomNcaHeaderSignature1VerificationFailed";
        case 0x235e02: return "InvalidNcaId";
        case 0x244802: return "PartitionSignatureVerificationFailed";
        case 0x2ee202: return "InvalidArgument";
        case 0x320002: return "PermissionDenied";
        case 0x339a02: return "ProgramInfoNotFound";
        default: return "UnknownResult";
    }
}

void log_result(const char *operation, Result result) {
    log_line("%s: raw=0x%08x canonical=0x%06x module=%u description=%u name=%s",
             operation, result, R_VALUE(result), R_MODULE(result),
             R_DESCRIPTION(result), result_name(result));
}

void format_result_code(char *out, size_t out_size, Result result) {
    snprintf(out, out_size, "%04u-%04u", 2000 + R_MODULE(result),
             R_DESCRIPTION(result));
}

void log_process_identity(void) {
    u64 program_id = 0;
    u64 process_id = 0;
    Result program_result = svcGetInfo(&program_id, InfoType_ProgramId,
                                       CUR_PROCESS_HANDLE, 0);
    Result process_result = svcGetProcessId(&process_id, CUR_PROCESS_HANDLE);
    log_line("process-identity program=%016llX program-result=0x%08x pid=%llu pid-result=0x%08x",
             (unsigned long long)program_id, program_result,
             (unsigned long long)process_id, process_result);
}

const char *kaihen_reason_name(KaihenCompatibilityReason reason) {
    switch (reason) {
        case KAIHEN_COMPATIBLE: return "compatible";
        case KAIHEN_NOT_DETECTED: return "not-detected";
        case KAIHEN_STATUS_UNAVAILABLE: return "status-unavailable";
        case KAIHEN_INITIALIZATION_FAILED: return "initialization-failed";
        case KAIHEN_PROTOCOL_TOO_OLD: return "protocol-too-old";
        case KAIHEN_LOADER_OVERRIDE_MISSING: return "loader-override-missing";
        default: return "unknown";
    }
}

KaihenCompatibility check_kaihen_compatibility(void) {
    const Result not_attempted =
        MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    KaihenCompatibility check = {
        .reason = KAIHEN_STATUS_UNAVAILABLE,
        .ro_result = not_attempted,
        .session_result = not_attempted,
        .status_result = not_attempted,
        .initialization_result = not_attempted,
        .marker_present = hosversionIsKaiHEN(),
        .loader_override_present =
            smGetServiceOverride(smEncodeName("fsp-ldr")) != INVALID_HANDLE,
    };

    if (!check.marker_present) {
        check.reason = KAIHEN_NOT_DETECTED;
        goto finished;
    }

    Service ro = {0};
    Service hen = {0};
    check.ro_result = kaihenGetRoSession(&ro);
    if (R_FAILED(check.ro_result)) goto finished;

    check.session_result = kaihenOpenHomebrewSession(&ro, &hen);
    if (R_FAILED(check.session_result)) goto finished;

    HenStatus status = {0};
    check.status_result = henGetStatus(&hen, &status);
    serviceClose(&hen);
    if (R_FAILED(check.status_result)) goto finished;
    log_line("hen-status pid=%llu protocol=%u",
             (unsigned long long)status.process_id, status.protocol_version);

    check.protocol_version = status.protocol_version;
    check.initialization_result = status.initialization_result;
    if (R_FAILED(status.initialization_result)) {
        check.reason = KAIHEN_INITIALIZATION_FAILED;
    } else if (status.protocol_version < KAIDUMP_MIN_HEN_PROTOCOL) {
        check.reason = KAIHEN_PROTOCOL_TOO_OLD;
    } else if (!check.loader_override_present) {
        check.reason = KAIHEN_LOADER_OVERRIDE_MISSING;
    } else {
        check.reason = KAIHEN_COMPATIBLE;
    }

finished:
    log_line("kaihen-compatibility reason=%s marker=%u protocol=%u minimum=%u loader-override=%u ro=0x%08x session=0x%08x status=0x%08x initialization=0x%08x",
             kaihen_reason_name(check.reason), check.marker_present,
             check.protocol_version, KAIDUMP_MIN_HEN_PROTOCOL,
             check.loader_override_present, check.ro_result,
             check.session_result, check.status_result,
             check.initialization_result);
    return check;
}

void close_ns(void) {
    if (g.ns_active) {
        nsExit();
        g.ns_active = false;
    }
}
