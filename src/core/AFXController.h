// AFXController.h — platform-independent application facade over the AlienFX
// SDK: device list management, lighting-mode-to-packet sequences and the HID
// diagnostics report. The macOS menu bar app wraps it in Objective-C
// (AlienFXBridge); the Windows tray app uses it directly.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AlienFX_SDK.h"

namespace AlienFX_SDK {

	// Desired lighting state for a device.
	enum class AFXMode {
		Off = 0,
		// Steady color.
		AlwaysOn = 1,
		// Light keys as they are pressed, fading out after a moment.
		// Hardware feature of external (APIv8) keyboards; other devices
		// fall back to AlwaysOn.
		OnKeyPress = 2,
	};

	// Lighting effect for the "on" modes. Static is the only one guaranteed
	// everywhere; the animated ones are applied best-effort per protocol
	// (and are EXPERIMENTAL on the AW920K — IDs extrapolated from the
	// captured static/spectrum/keypress commands).
	enum class AFXEffect {
		Static = 0,
		Pulse = 1,
		Morph = 2,
		Breathing = 3,
		Spectrum = 4,
	};

	// One detected AlienFX-capable device (keyboard, mouse, monitor,
	// laptop controller...).
	struct AFXDeviceEntry {
		uint16_t vid = 0, pid = 0;
		int apiVersion = API_UNKNOWN;
		std::string name;            // "Keyboard (Alienware AW920K ...)"
		bool supportsKeyPress = false; // APIv8 keyboards
		uint32_t deviceID = 0;       // (vid << 16) | pid
	};

	// Default light IDs used when no per-model mapping is known. Extra IDs
	// that a device doesn't have are simply ignored by the firmware, so we
	// over-shoot.
	std::vector<byte> DefaultLights(int version);

	// What kind of device each protocol family drives, so menu entries are
	// recognizable even when the HID product string isn't ("Hub Controller"
	// is actually the lighting controller inside Alienware monitors).
	const char* AFXKindLabel(int version);

	// Input devices (keyboards, mice) get the "Input Devices" section mode
	// by default; monitors and case/laptop controllers default to Always On.
	bool AFXIsInputDevice(int version);

	// Owns device enumeration and light control. Not thread-safe; call from
	// the UI thread.
	class AFXController {
	public:
		// Re-enumerate devices. Returns the currently connected AlienFX devices.
		std::vector<AFXDeviceEntry> Rescan();

		// Currently known connected devices (as of the last rescan).
		std::vector<AFXDeviceEntry> Devices();

		// Apply a lighting mode (and color, for the "on" modes) to a device.
		// level is the hardware brightness for the "on" modes (0..255, 255 =
		// full); the Go Dim system mode passes a reduced level. Devices
		// without hardware brightness (monitors, mice) get their RGB scaled.
		bool ApplyMode(uint32_t deviceID, AFXMode mode, uint8_t r, uint8_t g, uint8_t b,
		               uint8_t level = 255, AFXEffect effect = AFXEffect::Static);

		// Human-readable report of every HID interface, including the ones
		// that were NOT matched — for bug reports (`--list`). Platform
		// wrappers may append platform-specific notes.
		std::string DiagnosticsReport();

	private:
		Mappings fx;

		Afx_device* FindDevice(uint32_t deviceID);
		bool ApplyColorTo(Afx_device* dev, uint8_t r, uint8_t g, uint8_t b,
		                  AFXEffect effect = AFXEffect::Static);
	};
}
