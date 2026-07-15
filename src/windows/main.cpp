// main.cpp — Windows entry point for AlienFX Lights.
//
// Same executable serves the tray app and the CLI diagnostic modes:
//   AlienFXLights.exe          tray app (single instance)
//   AlienFXLights.exe --list   HID inventory + probe verdicts
//   AlienFXLights.exe --test   cycle colors on every device (bug reports)
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#include "TrayApp.h"

#include <commctrl.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

using AlienFX_SDK::AFXController;
using AlienFX_SDK::AFXDeviceEntry;
using AlienFX_SDK::AFXMode;

// The exe is /SUBSYSTEM:WINDOWS, so for CLI modes we attach to the console
// that launched us (or create one when launched detached). Std handles that
// are already redirected (pipes, files) are left untouched.
static void OpenConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    bool haveOut = out && out != INVALID_HANDLE_VALUE;
    bool haveErr = err && err != INVALID_HANDLE_VALUE;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !(haveOut && haveErr))
        AllocConsole();
    FILE* stream = nullptr;
    if (!haveOut) freopen_s(&stream, "CONOUT$", "w", stdout);
    if (!haveErr) freopen_s(&stream, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
}

// Self-test mode: cycles colors on every detected device with low-level
// logging enabled, then prints a summary. This is the one command to run
// when reporting "lights don't change" issues. Mirrors main.swift.
static int RunTest() {
    _putenv("ALIENFX_DEBUG=1");
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
    for (const AFXDeviceEntry& device : devices) {
        printf("=== %s [%04x:%04x APIv%d]\n", device.name.c_str(),
            device.vid, device.pid, device.apiVersion);
        for (const Step& s : steps) {
            bool ok = controller.ApplyMode(device.deviceID, AFXMode::AlwaysOn, s.r, s.g, s.b);
            printf(">>> %s: %s\n", s.label, ok ? "commands accepted" : "COMMAND FAILED");
            fflush(stdout);
            Sleep(1500);
        }
    }
    printf("Done. If commands were accepted but no light changed color, the device speaks a different protocol.\n");
    return 0;
}

// Diagnostic mode: prints detected devices plus the full HID inventory.
static int RunList() {
    AFXController controller;
    auto devices = controller.Rescan();
    if (devices.empty())
        printf("No AlienFX devices found.\n");
    for (const AFXDeviceEntry& device : devices)
        printf("%04x:%04x  APIv%d  %s\n", device.vid, device.pid,
            device.apiVersion, device.name.c_str());
    printf("\n%s", controller.DiagnosticsReport().c_str());
    printf("Tip: run with ALIENFX_DEBUG=1 for low-level command logging.\n");
    return 0;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool testMode = false, listMode = false;
    for (int i = 1; argv && i < argc; i++) {
        if (wcscmp(argv[i], L"--test") == 0) testMode = true;
        else if (wcscmp(argv[i], L"--list") == 0) listMode = true;
    }
    if (argv) LocalFree(argv);

    if (testMode || listMode) {
        OpenConsole();
        return testMode ? RunTest() : RunList();
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AlienFXLights.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0; // already running

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    TrayApp app;
    if (!app.Init(instance)) {
        ReleaseMutex(mutex);
        return 1;
    }
    int rc = app.Run();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return rc;
}
