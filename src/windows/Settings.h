// Settings.h — persisted state for the Windows tray app.
//
// Operating model (user spec, July 2026):
//   1. System mode on top: Go Light / Go Dim / Go Dark (brightness only,
//      color is preserved — like AWCC's system lighting mode).
//   2. One color for all devices.
//   3. An "Input Devices" mode applied to all keyboards/mice:
//      Always On / On While Typing / Always Off.
//   4. Optional per-device mode override (Default = follow the rules above).
//
// Stored under HKCU\Software\AlienFXLights; Start at Login uses the HKCU
// Run key.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#include <cstdint>

// Whole-system brightness mode (menu top radio group, mirrors AWCC).
enum class SystemMode {
    Light = 0, // full brightness
    Dim = 1,   // reduced brightness, colors preserved
    Dark = 2,  // everything off
};

// Lighting mode of a device (or of the whole Input Devices section).
enum class DeviceMode {
    Default = -1, // per-device only: follow the Input Devices mode / Always On
    AlwaysOn = 0,
    OnKeyPress = 1,
    Off = 2,
};

class Settings {
public:
    // Factory default is Alienware's signature cyan.
    static const uint32_t kDefaultColorHex = 0x00FFFF;

    SystemMode GetSystemMode() const;          // default Light
    void SetSystemMode(SystemMode mode);

    uint32_t GetColorHex() const; // 0xRRGGBB
    void SetColorHex(uint32_t hex);

    // Lighting effect index (AFXEffect values: 0 static .. 4 spectrum).
    int GetEffect() const;        // default 0 (static)
    void SetEffect(int effect);

    // Mode for all input devices (keyboards, touchpad, mice). Never Default.
    DeviceMode GetInputDevicesMode() const;    // default AlwaysOn
    void SetInputDevicesMode(DeviceMode mode);

    // Per-device override; Default (no value stored) = follow the rules.
    DeviceMode GetDeviceMode(uint32_t deviceID) const;
    void SetDeviceMode(uint32_t deviceID, DeviceMode mode);

    bool GetTurnOffWithDisplay() const; // default true
    void SetTurnOffWithDisplay(bool value);

    // Start at Login via HKCU\...\CurrentVersion\Run.
    bool IsStartAtLogin() const;
    void SetStartAtLogin(bool enabled);
};
