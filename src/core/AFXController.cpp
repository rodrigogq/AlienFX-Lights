// AFXController.cpp — platform-independent application facade.
//
// Global-effect values for APIv5/v8 keyboards follow alienfx-gui
// (ProfilesDialog.cpp): v8 effect 1 = static color, 9 = "one key" keypress,
// mode 1 = permanent slot, mode 2 = key slot; v5 effect 1 = static color.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#include "AFXController.h"

#include <cstdio>

using namespace std;

namespace AlienFX_SDK {

	vector<byte> DefaultLights(int version) {
		vector<byte> lights;
		int count;
		switch (version) {
			case API_V6:
				// Monitor zone mask bits: 0=logo, 1=number, 3=power button
				// (0x01|0x02|0x08 = the "all zones" mask OpenRGB uses).
				return { 0, 1, 3 };
			case API_V2:
			case API_V3: count = 14;  break; // zone mask bits
			case API_V4: count = 64;  break; // zone/light indexes (desktop
				// controllers like the AW-ELC use IDs above 24; unknown IDs
				// are ignored by the firmware, so over-shoot generously)
			case API_V7: count = 8;   break; // mouse zones
			case API_V5:
			case API_V8: count = 136; break; // per-key keyboards
			default:     count = 8;   break;
		}
		for (int i = 0; i < count; i++)
			lights.push_back((byte)i);
		return lights;
	}

	const char* AFXKindLabel(int version) {
		switch (version) {
			case API_V5:
			case API_V8:
			case API_AW920K: return "Keyboard";
			case API_V6: return "Monitor";
			case API_V7: return "Mouse";
			default:     return "Lights";
		}
	}

	bool AFXIsInputDevice(int version) {
		switch (version) {
			case API_V5:
			case API_V7:
			case API_V8:
			case API_AW920K: return true;
			default:         return false;
		}
	}

	static const uint16_t kAFXVendors[] = { 0x187c, 0x0d62, 0x0424, 0x0461, 0x04f2, 0x413c };

	static bool AFXIsKnownVendor(uint16_t vid) {
		for (uint16_t v : kAFXVendors)
			if (v == vid) return true;
		return false;
	}

	vector<AFXDeviceEntry> AFXController::Rescan() {
		fx.AlienFXEnumDevices();
		// Give v4 controllers a default light list so brightness commands have IDs to act on.
		for (auto &d : fx.fxdevs)
			if (d.dev && d.lights.empty() && d.dev->version == API_V4)
				for (byte i : DefaultLights(API_V4))
					d.lights.push_back({ i, {{0, 0}}, "" });
		return Devices();
	}

	vector<AFXDeviceEntry> AFXController::Devices() {
		vector<AFXDeviceEntry> result;
		for (auto &d : fx.fxdevs) {
			if (!d.dev) continue; // not currently connected
			string product = d.dev->description;
			if (product.empty()) {
				char idbuf[16];
				snprintf(idbuf, sizeof(idbuf), "%04x:%04x", d.vid, d.pid);
				product = idbuf;
			}
			AFXDeviceEntry entry;
			entry.vid = d.vid;
			entry.pid = d.pid;
			entry.apiVersion = d.dev->version;
			entry.name = string(AFXKindLabel(d.dev->version)) + " (" + product + ")";
			entry.supportsKeyPress = d.dev->version == API_V8 || d.dev->version == API_AW920K;
			entry.deviceID = ((uint32_t)d.vid << 16) | d.pid;
			result.push_back(std::move(entry));
		}
		return result;
	}

	Afx_device* AFXController::FindDevice(uint32_t deviceID) {
		Afx_device* dev = fx.GetDeviceById((WORD)(deviceID & 0xffff), (WORD)(deviceID >> 16));
		return (dev && dev->dev) ? dev : nullptr;
	}

	// AW920K permanent-plane effect IDs = (v8 opcode - 0x80); see AWEffect.
	static byte AWEffectId(AFXEffect effect) {
		switch (effect) {
		case AFXEffect::Pulse:     return 0x02;
		case AFXEffect::Morph:     return 0x03;
		case AFXEffect::Breathing: return 0x07;
		case AFXEffect::Spectrum:  return 0x08;
		case AFXEffect::Static:
		default:                   return 0x01;
		}
	}

	// Action type for the per-light protocols (v2-v7).
	static byte ActionType(AFXEffect effect) {
		switch (effect) {
		case AFXEffect::Pulse:     return AlienFX_A_Pulse;
		case AFXEffect::Morph:     return AlienFX_A_Morph;
		case AFXEffect::Breathing: return AlienFX_A_Breathing;
		case AFXEffect::Spectrum:  return AlienFX_A_Spectrum;
		case AFXEffect::Static:
		default:                   return AlienFX_A_Color;
		}
	}

	bool AFXController::ApplyMode(uint32_t deviceID, AFXMode mode, uint8_t r, uint8_t g, uint8_t b,
	                              uint8_t level, AFXEffect effect) {
		Afx_device* dev = FindDevice(deviceID);
		if (!dev) return false;
		Functions* fn = dev->dev;
		Afx_colorcode color((byte)b, (byte)g, (byte)r); // ctor order is b,g,r
		Afx_colorcode black((byte)0, (byte)0, (byte)0);

		switch (fn->version) {
		case API_AW920K: {
			byte aw = (byte)((level * 0x0a) / 255); // hardware scale is 0..10
			if (aw == 0 && level) aw = 1;
			switch (mode) {
			case AFXMode::Off:
				return fn->AWGlobalBrightness(0);
			case AFXMode::OnKeyPress:
				fn->AWGlobalBrightness(aw);
				return fn->AWKeyPressColor(r, g, b, aw);
			case AFXMode::AlwaysOn:
			default:
				fn->AWGlobalBrightness(aw);
				return fn->AWEffect(AWEffectId(effect), r, g, b, aw);
			}
		}

		case API_V8:
			switch (mode) {
			case AFXMode::Off:
				fn->SetGlobalEffects(0, 1, 1, 0, black, black); // clear permanent effect
				fn->SetGlobalEffects(0, 2, 1, 0, black, black); // clear key effect
				fn->SetBrightness(0, dev->brightness, &dev->lights, false);
				return true;
			case AFXMode::OnKeyPress:
				fn->SetBrightness(level, dev->brightness, &dev->lights, true);
				fn->SetGlobalEffects(0, 1, 1, 0, black, black);  // keys dark until pressed
				return fn->SetGlobalEffects(9, 2, 1, 5, color, black); // "one key" on press
			case AFXMode::AlwaysOn:
			default:
				fn->SetBrightness(level, dev->brightness, &dev->lights, true);
				return fn->SetGlobalEffects(1, 1, 1, 5, color, color); // static color
			}

		case API_V5:
			if (mode == AFXMode::Off) {
				fn->SetGlobalEffects(0, 1, 1, 0, black, black);
				fn->SetBrightness(0, dev->brightness, &dev->lights, false);
				return true;
			}
			fn->SetBrightness(level, dev->brightness, &dev->lights, true);
			return fn->SetGlobalEffects(1, 1, 1, 0, color, color); // static color

		default:
			if (mode == AFXMode::Off) {
				fn->SetBrightness(0, dev->brightness, &dev->lights, false);
				if (fn->version == API_V6 || fn->version == API_V7) {
					// Monitors and mice have no hardware brightness switch - paint them black.
					ApplyColorTo(dev, 0, 0, 0);
				}
				fn->UpdateColors();
				return true;
			}
			// v6/v7 carry the brightness byte inside their color packets
			// (set via SetBrightness -> fn->bright), so Go Dim works there too.
			fn->SetBrightness(level, dev->brightness, &dev->lights, true);
			return ApplyColorTo(dev, r, g, b, effect);
		}
	}

	bool AFXController::ApplyColorTo(Afx_device* dev, uint8_t r, uint8_t g, uint8_t b,
	                                 AFXEffect effect) {
		Functions* fn = dev->dev;
		vector<byte> lights = DefaultLights(fn->version);
		if (effect == AFXEffect::Static) {
			Afx_action color = { AlienFX_A_Color, 0, 0, r, g, b };
			bool ok = fn->SetMultiColor(&lights, color);
			fn->UpdateColors();
			return ok;
		}
		// Animated effects need an action block per light (SetMultiColor only
		// does steady colors on the mask-based protocols).
		Afx_action action = { ActionType(effect), 3, 5, r, g, b };
		bool ok = true;
		for (byte id : lights) {
			Afx_lightblock act{ id, { action, { AlienFX_A_Color, 3, 5, 0, 0, 0 } } };
			ok = fn->SetAction(&act) && ok;
		}
		fn->UpdateColors();
		return ok;
	}

	string AFXController::DiagnosticsReport() {
		string out = "All HID interfaces (* = AlienFX-related vendor: "
			"187c Alienware, 0d62 Darfon, 0424 Microchip, 0461 Primax, 04f2 Chicony, 413c Dell):\n";

		int total = 0;
		for (const HidDeviceInfo& info : HidEnumerate()) {
			total++;
			bool known = AFXIsKnownVendor(info.vid);
			string verdict;
			if (known) {
				Functions probe;
				bool matched = probe.AlienFXProbeDevice(info);
				if (matched) {
					verdict = "  ->  MATCHED as APIv" + to_string(probe.version);
					probe.AlienFXClose();
				}
				else
					verdict = "  ->  not matched";
			}
			char line[512];
			snprintf(line, sizeof(line),
				"%s %04x:%04x  \"%s %s\"  %s  usagePage 0x%x usage 0x%x  "
				"maxIn %d maxOut %d maxFeature %d%s\n",
				known ? "*" : " ",
				info.vid, info.pid,
				info.manufacturer.c_str(), info.product.c_str(),
				info.transport.c_str(),
				info.usagePage, info.usage,
				info.maxInput, info.maxOutput, info.maxFeature,
				verdict.c_str());
			out += line;
		}

		out += to_string(total) + " HID interfaces total on this system.\n";
		return out;
	}
}
