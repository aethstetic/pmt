#include "app.h"
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    pmt::App app;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--disable-color") == 0) {
            app.color_disabled = true;
        } else if (strcmp(argv[i], "--accent") == 0 && i + 1 < argc) {
            app.accent_hex = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: pmt [OPTIONS]\n\n");
            printf("Options:\n");
            printf("  --disable-color       Disable all colors (monochrome mode)\n");
            printf("  --accent <#RRGGBB>    Set custom accent color\n");
            printf("  -h, --help            Show this help\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Try 'pmt --help' for usage.\n");
            return 1;
        }
    }

    if (!app.init()) {
        fprintf(stderr, "Failed to initialize pmt\n");
        return 1;
    }

    app.run();
    return 0;
}
