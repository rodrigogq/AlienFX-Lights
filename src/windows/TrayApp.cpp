// TrayApp.cpp — Windows system-tray UI for AlienFX Lights.
//
// Menu model (user spec):
//   1. Go Light / Go Dim / Go Dark — system brightness, color preserved;
//   2. Color — one color for all devices;
//   3. Input Devices — Always On / On While Typing / Always Off for all
//      keyboards and mice at once;
//   4. per-device submenu to override the mode (Default follows the rules).
// Picking a color while in Go Dark switches to Go Light.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#include "TrayApp.h"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <tlhelp32.h>

#include <cmath>
#include <cwchar>
#include <string>

using AlienFX_SDK::AFXDeviceEntry;
using AlienFX_SDK::AFXIsInputDevice;
using AlienFX_SDK::AFXMode;

namespace {

const UINT WMAPP_TRAY = WM_APP + 1;
const UINT_PTR TIMER_RESCAN = 1;
const UINT_PTR TIMER_RESUME = 2;

const BYTE kDimLevel = 102; // Go Dim brightness (~40%; AW920K maps to 4/10)

enum : UINT {
    IDM_SYS_FIRST = 100,     // + SystemMode value (Light/Dim/Dark)
    IDM_INPUT_FIRST = 150,   // + DeviceMode value (AlwaysOn/OnKeyPress/Off)
    IDM_EFFECT_FIRST = 200,  // + AFXEffect value
    IDM_COLOR_FIRST = 300,   // + preset index
    IDM_COLOR_CUSTOM = 350,
    IDM_DEVICE_BASE = 400,   // + device index * 8 + device-mode slot
    IDM_OFF_WITH_DISPLAY = 900,
    IDM_REFRESH = 901,
    IDM_START_LOGIN = 902,
    IDM_QUIT = 903,
};

// Per-device submenu slots (IDM_DEVICE_BASE + index*8 + slot).
enum : UINT {
    DEV_SLOT_DEFAULT = 0,
    DEV_SLOT_ALWAYS_ON = 1,
    DEV_SLOT_ON_KEY_PRESS = 2,
    DEV_SLOT_OFF = 3,
    DEV_SLOT_STRIDE = 8,
};

struct SystemModeItem { SystemMode mode; const wchar_t* title; };
const SystemModeItem kSystemModes[] = {
    { SystemMode::Light, L"Go Light" },
    { SystemMode::Dim,   L"Go Dim" },
    { SystemMode::Dark,  L"Go Dark" },
};

struct InputModeItem { DeviceMode mode; const wchar_t* title; };
const InputModeItem kInputModes[] = {
    { DeviceMode::AlwaysOn,   L"Always On" },
    { DeviceMode::OnKeyPress, L"On While Typing" },
    { DeviceMode::Off,        L"Always Off" },
};

struct EffectItem { const wchar_t* title; };
// Order matches AFXEffect. Animated effects are best-effort per protocol
// and experimental on the AW920K.
const EffectItem kEffects[] = {
    { L"Static Color" },
    { L"Pulse" },
    { L"Morph" },
    { L"Breathing" },
    { L"Spectrum" },
};

struct ColorPreset { const wchar_t* name; BYTE r, g, b; };
// Exact palette of the macOS app; Alienware Cyan is the factory default.
const ColorPreset kPresets[] = {
    { L"Alienware Cyan", 0, 255, 255 },
    { L"Alien Green",    0, 255, 50 },
    { L"White",          255, 255, 255 },
    { L"Red",            255, 0, 0 },
    { L"Orange",         255, 100, 0 },
    { L"Yellow",         255, 220, 0 },
    { L"Blue",           0, 60, 255 },
    { L"Purple",         150, 0, 255 },
    { L"Pink",           255, 0, 130 },
};

// HID class interface GUID (from hidclass.h), so this file doesn't need the
// DDK headers just to filter device notifications.
const GUID kHidInterfaceGuid =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

} // namespace

bool TrayApp::Init(HINSTANCE inst) {
    instance = inst;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = L"AlienFXLightsWnd";
    if (!RegisterClassW(&wc))
        return false;

    // Hidden top-level window (never shown). Deliberately NOT message-only:
    // HWND_MESSAGE windows don't receive WM_DEVICECHANGE / WM_POWERBROADCAST
    // broadcasts, which drive hotplug and turn-off-with-display.
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"AlienFX Lights", WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd)
        return false;

    taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APPICON), LIM_SMALL, &appIcon);
    AddTrayIcon();

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kHidInterfaceGuid;
    devNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    powerNotify = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE,
        DEVICE_NOTIFY_WINDOW_HANDLE);

    RefreshDevices(false); // enumerate only - stay passive until the user acts
    ShowAwccWarningIfRunning();
    return true;
}

int TrayApp::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK TrayApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayApp* self;
    if (msg == WM_NCCREATE) {
        self = (TrayApp*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (TrayApp*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    return self ? self->WndProc(hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WMAPP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU: {
            // NOTIFYICON_VERSION_4 packs the anchor point into wParam.
            POINT anchor = { GET_X_LPARAM(wp), GET_Y_LPARAM(wp) };
            ShowMenu(anchor);
            return 0;
        }
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_RESCAN || wp == TIMER_RESUME) {
            KillTimer(wnd, wp);
            RefreshDevices(active);
        }
        return 0;

    case WM_DEVICECHANGE:
        switch (wp) {
        case DBT_DEVICEARRIVAL:
        case DBT_DEVICEREMOVECOMPLETE:
        case DBT_DEVNODES_CHANGED:
            // Debounce: one physical device often exposes several HID interfaces.
            SetTimer(wnd, TIMER_RESCAN, 1000, nullptr);
            break;
        }
        return TRUE;

    case WM_POWERBROADCAST:
        switch (wp) {
        case PBT_POWERSETTINGCHANGE: {
            auto* setting = (POWERBROADCAST_SETTING*)lp;
            if (setting && IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)
                && setting->DataLength >= sizeof(DWORD)) {
                DWORD state = *(DWORD*)setting->Data; // 0 off, 1 on, 2 dimmed
                if (state == 0) {
                    if (active && settings.GetTurnOffWithDisplay())
                        ApplyOffToAll();
                } else if (state == 1) {
                    if (active)
                        ApplyStateToAll();
                }
            }
            break;
        }
        case PBT_APMRESUMEAUTOMATIC:
            // Give USB devices a moment to re-enumerate after wake.
            SetTimer(wnd, TIMER_RESUME, 2000, nullptr);
            break;
        }
        return TRUE;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (devNotify) { UnregisterDeviceNotification(devNotify); devNotify = nullptr; }
        if (powerNotify) { UnregisterPowerSettingNotification(powerNotify); powerNotify = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == taskbarCreatedMsg && taskbarCreatedMsg) {
            AddTrayIcon(); // Explorer restarted
            return 0;
        }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

#pragma region Tray icon

void TrayApp::AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WMAPP_TRAY;
    nid.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"AlienFX Lights");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayApp::RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayApp::ShowAwccWarningIfRunning() {
    bool awccRunning = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                if (_wcsnicmp(entry.szExeFile, L"AWCC", 4) == 0) {
                    awccRunning = true;
                    break;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }
    if (!awccRunning)
        return;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, L"AlienFX Lights");
    wcscpy_s(nid.szInfo, L"Alienware Command Center is running and may override lighting.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

#pragma endregion

#pragma region Menu

HBITMAP TrayApp::MakeSwatch(BYTE r, BYTE g, BYTE b, int size) {
    // Antialiased round swatch on a premultiplied-ARGB DIB, so it renders
    // correctly with the themed menu painter (no owner-draw needed).
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp)
        return nullptr;

    auto* px = (DWORD*)bits;
    const double center = (size - 1) / 2.0;
    const double radius = size / 2.0 - 1.0;   // 1px inset, like the macOS swatch
    const double border = 1.0;                // subtle gray ring so White stays visible
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double d = std::sqrt((x - center) * (x - center) + (y - center) * (y - center));
            double coverage = radius + 0.5 - d;         // antialiased outer edge
            if (coverage <= 0) { px[y * size + x] = 0; continue; }
            if (coverage > 1) coverage = 1;
            double ring = d - (radius - border);        // >0 inside the ring band
            BYTE cr = r, cg = g, cb = b;
            if (ring > 0) {
                double t = ring < 1 ? ring : 1;         // blend toward gray at the rim
                cr = (BYTE)(r + (128 - r) * t * 0.6);
                cg = (BYTE)(g + (128 - g) * t * 0.6);
                cb = (BYTE)(b + (128 - b) * t * 0.6);
            }
            BYTE a = (BYTE)(coverage * 255);
            px[y * size + x] = ((DWORD)a << 24)
                | ((DWORD)(cr * a / 255) << 16)
                | ((DWORD)(cg * a / 255) << 8)
                | (DWORD)(cb * a / 255);
        }
    }
    return bmp;
}

void TrayApp::ShowMenu(POINT anchor) {
    // Refresh the device list on open (AWCC may have been used in parallel,
    // devices may have come or gone). Enumeration only - no state is applied.
    RefreshDevices(false);

    // Menu is rebuilt fresh on every open.
    HMENU menu = CreatePopupMenu();
    HMENU colorMenu = CreatePopupMenu();
    HMENU inputMenu = CreatePopupMenu();
    SystemMode sysMode = settings.GetSystemMode();
    DeviceMode inputMode = settings.GetInputDevicesMode();
    uint32_t currentHex = settings.GetColorHex();

    // 1. System mode (Go Light / Go Dim / Go Dark)
    for (const SystemModeItem& m : kSystemModes)
        AppendMenuW(menu, MF_STRING, IDM_SYS_FIRST + (UINT)m.mode, m.title);
    CheckMenuRadioItem(menu, IDM_SYS_FIRST, IDM_SYS_FIRST + 2,
        IDM_SYS_FIRST + (UINT)sysMode, MF_BYCOMMAND);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 2. Color for all devices
    UINT dpi = GetDpiForWindow(hwnd);
    int swatchSize = MulDiv(14, (int)dpi, 96);
    for (size_t i = 0; i < _countof(kPresets); i++) {
        const ColorPreset& p = kPresets[i];
        uint32_t presetHex = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
        HBITMAP swatch = MakeSwatch(p.r, p.g, p.b, swatchSize);
        swatches.push_back(swatch);

        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP | MIIM_STATE;
        mii.wID = IDM_COLOR_FIRST + (UINT)i;
        mii.dwTypeData = const_cast<wchar_t*>(p.name);
        mii.hbmpItem = swatch;
        mii.fState = presetHex == currentHex ? MFS_CHECKED : MFS_ENABLED;
        InsertMenuItemW(colorMenu, (UINT)i, TRUE, &mii);
    }
    AppendMenuW(colorMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(colorMenu, MF_STRING, IDM_COLOR_CUSTOM, L"Custom…");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorMenu, L"Color");

    // 2b. Effect (applies to the Always On lighting)
    HMENU effectMenu = CreatePopupMenu();
    int currentEffect = settings.GetEffect();
    for (size_t i = 0; i < _countof(kEffects); i++)
        AppendMenuW(effectMenu, MF_STRING, IDM_EFFECT_FIRST + (UINT)i, kEffects[i].title);
    CheckMenuRadioItem(effectMenu, IDM_EFFECT_FIRST, IDM_EFFECT_FIRST + _countof(kEffects) - 1,
        IDM_EFFECT_FIRST + (UINT)currentEffect, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)effectMenu, L"Effect");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 3. Input Devices mode (all keyboards/mice at once)
    for (const InputModeItem& m : kInputModes)
        AppendMenuW(inputMenu, MF_STRING, IDM_INPUT_FIRST + (UINT)m.mode, m.title);
    CheckMenuRadioItem(inputMenu, IDM_INPUT_FIRST, IDM_INPUT_FIRST + 2,
        IDM_INPUT_FIRST + (UINT)inputMode, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)inputMenu, L"Input Devices");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 4. Per-device mode overrides
    if (devices.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No Alienware devices found");
    } else {
        for (size_t i = 0; i < devices.size(); i++) {
            const AFXDeviceEntry& dev = devices[i];
            DeviceMode devMode = settings.GetDeviceMode(dev.deviceID);
            UINT base = IDM_DEVICE_BASE + (UINT)i * DEV_SLOT_STRIDE;

            HMENU devMenu = CreatePopupMenu();
            AppendMenuW(devMenu, MF_STRING, base + DEV_SLOT_DEFAULT, L"Default");
            AppendMenuW(devMenu, MF_STRING, base + DEV_SLOT_ALWAYS_ON, L"Always On");
            if (dev.supportsKeyPress)
                AppendMenuW(devMenu, MF_STRING, base + DEV_SLOT_ON_KEY_PRESS, L"On While Typing");
            AppendMenuW(devMenu, MF_STRING, base + DEV_SLOT_OFF, L"Off");

            UINT checked = base + DEV_SLOT_DEFAULT;
            if (devMode == DeviceMode::AlwaysOn) checked = base + DEV_SLOT_ALWAYS_ON;
            else if (devMode == DeviceMode::OnKeyPress) checked = base + DEV_SLOT_ON_KEY_PRESS;
            else if (devMode == DeviceMode::Off) checked = base + DEV_SLOT_OFF;
            CheckMenuRadioItem(devMenu, base, base + DEV_SLOT_STRIDE - 1, checked, MF_BYCOMMAND);

            AppendMenuW(menu, MF_POPUP, (UINT_PTR)devMenu, Utf8ToWide(dev.name).c_str());
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (settings.GetTurnOffWithDisplay() ? MF_CHECKED : 0),
        IDM_OFF_WITH_DISPLAY, L"Turn Off With Display");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh Devices");
    AppendMenuW(menu, MF_STRING | (settings.IsStartAtLogin() ? MF_CHECKED : 0),
        IDM_START_LOGIN, L"Start at Login");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit AlienFX Lights");

    // Canonical tray-menu dance so the menu dismisses when clicking away.
    SetForegroundWindow(hwnd);
    UINT cmd = (UINT)TrackPopupMenuEx(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        anchor.x, anchor.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu); // also destroys submenus
    for (HBITMAP bmp : swatches)
        if (bmp) DeleteObject(bmp);
    swatches.clear();

    if (cmd)
        OnCommand(cmd);
}

void TrayApp::OnCommand(UINT cmd) {
    // Any lighting choice makes the app take ownership of the hardware state.
    if ((cmd >= IDM_SYS_FIRST && cmd < IDM_SYS_FIRST + _countof(kSystemModes))
        || (cmd >= IDM_INPUT_FIRST && cmd < IDM_INPUT_FIRST + _countof(kInputModes))
        || (cmd >= IDM_EFFECT_FIRST && cmd < IDM_EFFECT_FIRST + _countof(kEffects))
        || (cmd >= IDM_COLOR_FIRST && cmd <= IDM_COLOR_CUSTOM)
        || (cmd >= IDM_DEVICE_BASE && cmd < IDM_DEVICE_BASE + devices.size() * DEV_SLOT_STRIDE))
        active = true;

    if (cmd >= IDM_SYS_FIRST && cmd < IDM_SYS_FIRST + _countof(kSystemModes)) {
        settings.SetSystemMode((SystemMode)(cmd - IDM_SYS_FIRST));
        ApplyStateToAll();
        return;
    }
    if (cmd >= IDM_INPUT_FIRST && cmd < IDM_INPUT_FIRST + _countof(kInputModes)) {
        settings.SetInputDevicesMode((DeviceMode)(cmd - IDM_INPUT_FIRST));
        ApplyStateToAll();
        return;
    }
    if (cmd >= IDM_COLOR_FIRST && cmd < IDM_COLOR_FIRST + _countof(kPresets)) {
        const ColorPreset& p = kPresets[cmd - IDM_COLOR_FIRST];
        SetColor(p.r, p.g, p.b);
        return;
    }
    if (cmd >= IDM_EFFECT_FIRST && cmd < IDM_EFFECT_FIRST + _countof(kEffects)) {
        settings.SetEffect((int)(cmd - IDM_EFFECT_FIRST));
        if (settings.GetSystemMode() == SystemMode::Dark)
            settings.SetSystemMode(SystemMode::Light); // picking an effect turns lights on
        ApplyStateToAll();
        return;
    }
    if (cmd >= IDM_DEVICE_BASE && cmd < IDM_DEVICE_BASE + devices.size() * DEV_SLOT_STRIDE) {
        size_t index = (cmd - IDM_DEVICE_BASE) / DEV_SLOT_STRIDE;
        UINT slot = (cmd - IDM_DEVICE_BASE) % DEV_SLOT_STRIDE;
        DeviceMode mode = DeviceMode::Default;
        if (slot == DEV_SLOT_ALWAYS_ON) mode = DeviceMode::AlwaysOn;
        else if (slot == DEV_SLOT_ON_KEY_PRESS) mode = DeviceMode::OnKeyPress;
        else if (slot == DEV_SLOT_OFF) mode = DeviceMode::Off;
        settings.SetDeviceMode(devices[index].deviceID, mode);
        ApplyStateTo(devices[index]);
        return;
    }
    switch (cmd) {
    case IDM_COLOR_CUSTOM:
        PickCustomColor();
        break;
    case IDM_OFF_WITH_DISPLAY:
        settings.SetTurnOffWithDisplay(!settings.GetTurnOffWithDisplay());
        break;
    case IDM_REFRESH:
        RefreshDevices(true);
        break;
    case IDM_START_LOGIN:
        settings.SetStartAtLogin(!settings.IsStartAtLogin());
        break;
    case IDM_QUIT:
        DestroyWindow(hwnd);
        break;
    }
}

void TrayApp::PickCustomColor() {
    static COLORREF customColors[16] = {};
    uint32_t hex = settings.GetColorHex();
    // Stored ColorHex is 0xRRGGBB (like macOS); COLORREF is 0x00BBGGRR.
    COLORREF initial = RGB((hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);

    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hwnd;
    cc.rgbResult = initial;
    cc.lpCustColors = customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc))
        SetColor(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
}

#pragma endregion

#pragma region Device state

DeviceMode TrayApp::ResolvedMode(const AFXDeviceEntry& device) const {
    DeviceMode mode = settings.GetDeviceMode(device.deviceID);
    if (mode != DeviceMode::Default)
        return mode;
    if (AFXIsInputDevice(device.apiVersion))
        return settings.GetInputDevicesMode();
    return DeviceMode::AlwaysOn;
}

void TrayApp::RefreshDevices(bool applyState) {
    devices = controller.Rescan();
    if (applyState)
        ApplyStateToAll();
}

void TrayApp::ApplyStateTo(const AFXDeviceEntry& device) {
    SystemMode sysMode = settings.GetSystemMode();
    DeviceMode devMode = ResolvedMode(device);

    AFXMode mode;
    BYTE level = 255;
    if (sysMode == SystemMode::Dark || devMode == DeviceMode::Off) {
        mode = AFXMode::Off;
    } else {
        mode = devMode == DeviceMode::OnKeyPress ? AFXMode::OnKeyPress : AFXMode::AlwaysOn;
        if (sysMode == SystemMode::Dim)
            level = kDimLevel;
    }

    uint32_t hex = settings.GetColorHex();
    controller.ApplyMode(device.deviceID, mode,
        (uint8_t)((hex >> 16) & 0xff), (uint8_t)((hex >> 8) & 0xff), (uint8_t)(hex & 0xff),
        level, (AlienFX_SDK::AFXEffect)settings.GetEffect());
}

void TrayApp::ApplyStateToAll() {
    for (const AFXDeviceEntry& device : devices)
        ApplyStateTo(device);
}

void TrayApp::ApplyOffToAll() {
    for (const AFXDeviceEntry& device : devices)
        controller.ApplyMode(device.deviceID, AFXMode::Off, 0, 0, 0);
}

void TrayApp::SetColor(BYTE r, BYTE g, BYTE b) {
    settings.SetColorHex(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    if (settings.GetSystemMode() == SystemMode::Dark)
        settings.SetSystemMode(SystemMode::Light); // picking a color turns lights on
    ApplyStateToAll();
}

#pragma endregion
