// Settings.cpp — registry persistence for the Windows tray app.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#include "Settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

static const wchar_t* kAppKey = L"Software\\AlienFXLights";
static const wchar_t* kDeviceModesKey = L"Software\\AlienFXLights\\DeviceModes";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"AlienFX Lights";

static std::wstring ReadString(const wchar_t* key, const wchar_t* value) {
    // Sized dynamically: the Run-key value holds the full quoted exe path,
    // which easily exceeds a small fixed buffer.
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return L"";
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, &buf[0], &size) != ERROR_SUCCESS)
        return L"";
    buf.resize(wcslen(buf.c_str())); // trim the terminator(s)
    return buf;
}

static bool ReadDword(const wchar_t* key, const wchar_t* value, DWORD* out) {
    DWORD size = sizeof(DWORD);
    return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

static void WriteString(const wchar_t* key, const wchar_t* value, const std::wstring& data) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(h, value, 0, REG_SZ, (const BYTE*)data.c_str(),
            (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
}

static void WriteDword(const wchar_t* key, const wchar_t* value, DWORD data) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(h, value, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
        RegCloseKey(h);
    }
}

static void DeleteValue(const wchar_t* key, const wchar_t* value) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, value);
        RegCloseKey(h);
    }
}

static std::wstring DeviceKey(uint32_t deviceID) {
    wchar_t buf[16];
    swprintf_s(buf, L"%04x:%04x", (deviceID >> 16) & 0xffff, deviceID & 0xffff);
    return buf;
}

static DeviceMode ParseDeviceMode(const std::wstring& raw, DeviceMode fallback) {
    if (raw == L"alwaysOn") return DeviceMode::AlwaysOn;
    if (raw == L"onKeyPress") return DeviceMode::OnKeyPress;
    if (raw == L"off") return DeviceMode::Off;
    return fallback;
}

static const wchar_t* DeviceModeString(DeviceMode mode) {
    switch (mode) {
    case DeviceMode::OnKeyPress: return L"onKeyPress";
    case DeviceMode::Off: return L"off";
    case DeviceMode::AlwaysOn:
    default: return L"alwaysOn";
    }
}

SystemMode Settings::GetSystemMode() const {
    std::wstring raw = ReadString(kAppKey, L"SystemMode");
    if (raw == L"goDim") return SystemMode::Dim;
    if (raw == L"goDark") return SystemMode::Dark;
    return SystemMode::Light;
}

void Settings::SetSystemMode(SystemMode mode) {
    const wchar_t* raw = L"goLight";
    if (mode == SystemMode::Dim) raw = L"goDim";
    else if (mode == SystemMode::Dark) raw = L"goDark";
    WriteString(kAppKey, L"SystemMode", raw);
}

uint32_t Settings::GetColorHex() const {
    DWORD hex = 0;
    if (ReadDword(kAppKey, L"ColorHex", &hex))
        return hex & 0xffffff;
    return kDefaultColorHex;
}

void Settings::SetColorHex(uint32_t hex) {
    WriteDword(kAppKey, L"ColorHex", hex & 0xffffff);
}

int Settings::GetEffect() const {
    DWORD v = 0;
    if (ReadDword(kAppKey, L"Effect", &v) && v <= 4)
        return (int)v;
    return 0;
}

void Settings::SetEffect(int effect) {
    WriteDword(kAppKey, L"Effect", (DWORD)effect);
}

DeviceMode Settings::GetInputDevicesMode() const {
    return ParseDeviceMode(ReadString(kAppKey, L"InputDevicesMode"), DeviceMode::AlwaysOn);
}

void Settings::SetInputDevicesMode(DeviceMode mode) {
    WriteString(kAppKey, L"InputDevicesMode",
        DeviceModeString(mode == DeviceMode::Default ? DeviceMode::AlwaysOn : mode));
}

DeviceMode Settings::GetDeviceMode(uint32_t deviceID) const {
    return ParseDeviceMode(ReadString(kDeviceModesKey, DeviceKey(deviceID).c_str()),
        DeviceMode::Default);
}

void Settings::SetDeviceMode(uint32_t deviceID, DeviceMode mode) {
    if (mode == DeviceMode::Default)
        DeleteValue(kDeviceModesKey, DeviceKey(deviceID).c_str());
    else
        WriteString(kDeviceModesKey, DeviceKey(deviceID).c_str(), DeviceModeString(mode));
}

bool Settings::GetTurnOffWithDisplay() const {
    DWORD v = 1;
    if (ReadDword(kAppKey, L"TurnOffWithDisplay", &v))
        return v != 0;
    return true;
}

void Settings::SetTurnOffWithDisplay(bool value) {
    WriteDword(kAppKey, L"TurnOffWithDisplay", value ? 1 : 0);
}

static std::wstring QuotedExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\"";
}

bool Settings::IsStartAtLogin() const {
    return ReadString(kRunKey, kRunValue) == QuotedExePath();
}

void Settings::SetStartAtLogin(bool enabled) {
    if (enabled) {
        WriteString(kRunKey, kRunValue, QuotedExePath());
    } else {
        DeleteValue(kRunKey, kRunValue);
    }
}
