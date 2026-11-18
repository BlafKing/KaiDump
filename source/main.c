#include "app_internal.h"

AppState g;
unsigned char *g_copy_buffer;
FILE *g_log;
Service g_license_context;
u64 g_exefs_attempt;
volatile int g_usb_stop;
Thread g_usb_thread;

int main(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeAny(&pad);

    if (ensure_directory("sdmc:/switch/KaiDump") == 0)
        g_log = fopen("sdmc:/switch/KaiDump/KaiDump.log", "w");
    g.log_active = g_log != NULL;
    const u32 hos = hosversionGet();
    log_line("KaiDump %s start kaiHEN=%u HOS=%u.%u.%u", KAIDUMP_VERSION,
             hosversionIsKaiHEN(), HOSVER_MAJOR(hos), HOSVER_MINOR(hos),
             HOSVER_MICRO(hos));
    Handle fsp_override = smGetServiceOverride(smEncodeName("fsp-srv"));
    log_line("HEN fsp-srv override=%s handle=0x%x",
             fsp_override != INVALID_HANDLE ? "present" : "absent", fsp_override);
    Handle fsldr_override = smGetServiceOverride(smEncodeName("fsp-ldr"));
    log_line("HEN fsp-ldr override=%s handle=0x%x",
             fsldr_override != INVALID_HANDLE ? "present" : "absent",
             fsldr_override);

    Result ui_result = uiInitialize();
    log_result("ui-initialize", ui_result);
    if (R_FAILED(ui_result)) {
        consoleInit(NULL);
        printf("KaiDump could not initialize its graphical interface.\n\n");
        printf("Result: 0x%08x (module %u, description %u)\n\n",
               ui_result, R_MODULE(ui_result), R_DESCRIPTION(ui_result));
        printf("The result was also written to KaiDump.log.\n");
        printf("Press + to exit.\n");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (buttons_pressed(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        log_line("KaiDump exit after UI initialization failure");
        if (g_log) fclose(g_log);
        consoleExit(NULL);
        return 1;
    }

    KaihenCompatibility compatibility = check_kaihen_compatibility();
    g.kaihen_checked = true;
    g.kaihen_ready = compatibility.reason == KAIHEN_COMPATIBLE;

    int exit_code = 0;
    if (!g.kaihen_ready) {
        draw_kaihen_error(&compatibility);
        while (appletMainLoop()) {
            uiPumpEvents();
            padUpdate(&pad);
            if (buttons_pressed(&pad) & HidNpadButton_Plus) break;
            svcSleepThread(16000000ULL);
        }
        exit_code = 1;
    } else {
        g.title_grid = true;
        g.grid_icon_first = -1;
        draw_loading();
        refresh_applications();
        draw();
        bool running = true;
        while (running && appletMainLoop()) {
            uiPumpEvents();
            padUpdate(&pad);
            running = handle_input(buttons_pressed(&pad), &pad);
        }
    }

    if (g_copy_buffer) free(g_copy_buffer);
    free(g.control_icon);
    if (serviceIsActive(&g_license_context)) serviceClose(&g_license_context);
    close_ns();
    log_line("KaiDump exit");
    if (g_log) fclose(g_log);
    uiExit();
    return exit_code;
}
