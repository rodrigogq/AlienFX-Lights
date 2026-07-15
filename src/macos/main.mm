// main.mm — entry point for the macOS menu bar app (Objective-C++).
//
// Same binary serves the menu bar app and the CLI diagnostic modes:
//   AlienFXLights            menu bar app
//   AlienFXLights --list     HID inventory + probe verdicts
//   AlienFXLights --test     cycle colors on every device (bug reports)
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#import <AppKit/AppKit.h>
#import <IOKit/hid/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDLib.h> // IOHIDCheckAccess

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "AFXController.h"
#include "AppDelegate.h"

using AlienFX_SDK::AFXController;
using AlienFX_SDK::AFXDeviceEntry;
using AlienFX_SDK::AFXMode;

// Self-test mode: cycles colors on every detected device with low-level
// logging, then prints a summary. The one command to run for bug reports.
static int RunTest() {
    setenv("ALIENFX_DEBUG", "1", 1);
    AFXController controller;
    auto devices = controller.Rescan();
    if (devices.empty()) {
        printf("No AlienFX devices found. Run --list for the full HID inventory.\n");
        return 0;
    }
    struct Step { const char* label; uint8_t r, g, b; };
    const Step steps[] = {
        { "RED", 255, 0, 0 }, { "GREEN", 0, 255, 0 },
        { "BLUE", 0, 0, 255 }, { "WHITE", 255, 255, 255 },
    };
    for (const AFXDeviceEntry& d : devices) {
        printf("=== %s [%04x:%04x APIv%d]\n", d.name.c_str(), d.vid, d.pid, d.apiVersion);
        for (const Step& s : steps) {
            bool ok = controller.ApplyMode(d.deviceID, AFXMode::AlwaysOn, s.r, s.g, s.b);
            printf(">>> %s: %s\n", s.label, ok ? "commands accepted" : "COMMAND FAILED");
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
    printf("Done. If commands were accepted but no light changed color, the device speaks a different protocol.\n");
    return 0;
}

static int RunList() {
    AFXController controller;
    auto devices = controller.Rescan();
    if (devices.empty())
        printf("No AlienFX devices found.\n");
    for (const AFXDeviceEntry& d : devices)
        printf("%04x:%04x  APIv%d  %s\n", d.vid, d.pid, d.apiVersion, d.name.c_str());
    printf("\n%s", controller.DiagnosticsReport().c_str());

    if (@available(macOS 10.15, *)) {
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted)
            printf("Note: Input Monitoring permission not granted. If a keyboard interface "
                   "fails to open, allow this app under System Settings > Privacy & Security "
                   "> Input Monitoring.\n");
    }
    printf("Tip: run with ALIENFX_DEBUG=1 for low-level command logging.\n");
    return 0;
}

int main(int argc, const char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return RunTest();
        if (strcmp(argv[i], "--list") == 0) return RunList();
    }

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; // menu bar only
        [app run];
    }
    return 0;
}
