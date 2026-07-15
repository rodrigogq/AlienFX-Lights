// TrayApp.h — Windows system-tray UI for AlienFX Lights.
//
// Menu model (user spec): Go Light/Dim/Dark on top, then Color for all
// devices, then an Input Devices mode (keyboards/mice), then per-device
// mode overrides. See Settings.h.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <vector>

#include "AFXController.h"
#include "Settings.h"

class TrayApp {
public:
    // Creates the hidden window, tray icon and device/power notifications,
    // then does the initial device scan + state apply.
    bool Init(HINSTANCE instance);
    int Run(); // message loop; returns the WM_QUIT exit code

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowMenu(POINT anchor);
    void OnCommand(UINT cmd);
    HBITMAP MakeSwatch(BYTE r, BYTE g, BYTE b, int size);

    void RefreshDevices(bool applyState);
    void ApplyStateToAll();
    void ApplyStateTo(const AlienFX_SDK::AFXDeviceEntry& device);
    void ApplyOffToAll();
    void SetColor(BYTE r, BYTE g, BYTE b);
    void PickCustomColor();
    void ShowAwccWarningIfRunning();

    // Resolved mode for a device: per-device override, else the Input
    // Devices mode for keyboards/mice, else Always On.
    DeviceMode ResolvedMode(const AlienFX_SDK::AFXDeviceEntry& device) const;

    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HICON appIcon = nullptr;
    UINT taskbarCreatedMsg = 0;
    HDEVNOTIFY devNotify = nullptr;
    HPOWERNOTIFY powerNotify = nullptr;

    AlienFX_SDK::AFXController controller;
    Settings settings;
    std::vector<AlienFX_SDK::AFXDeviceEntry> devices;
    std::vector<HBITMAP> swatches; // alive while the menu is open

    // The app starts passive: it never touches the hardware until the user
    // picks something in the menu, so lighting set by AWCC or stored in the
    // devices is preserved. After the first user action it owns the state
    // (reapplies on wake, hotplug and display power events).
    bool active = false;
};
