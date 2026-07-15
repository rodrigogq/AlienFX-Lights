// AFXHidTransport.h — platform-neutral HID transport used by the AlienFX SDK
// core. Each platform provides one implementation (IOKit on macOS, Win32 HID
// on Windows) in its HidTransport_*.cpp backend.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AlienFX_SDK {

	// One HID interface as seen during enumeration. Nothing is opened yet.
	// Report lengths follow the Windows convention: they include the
	// report-ID byte (Win32 reports them that way natively; the IOKit
	// backend passes through whatever the descriptor says, which is why the
	// probe table accepts off-by-one values).
	struct HidDeviceInfo {
		uint16_t vid = 0, pid = 0;
		uint16_t usagePage = 0, usage = 0;
		int maxInput = 0, maxOutput = 0, maxFeature = 0;
		std::string manufacturer, product, transport;
		std::string path;             // Win32: device interface path (used by HidOpen)
		uint64_t registryEntryId = 0; // macOS: IORegistry entry ID (used by HidOpen)
	};

	// An open HID device. Buffers use the Windows/hidapi convention:
	// buffer[0] holds the report ID (0 for unnumbered reports) and `len`
	// includes that byte. Platform quirks (stripping the ID byte for
	// unnumbered reports, length self-calibration) live inside the backend.
	class IHidTransport {
	public:
		virtual ~IHidTransport() = default;
		virtual bool SetOutputReport(uint8_t* buffer, int len) = 0;
		virtual bool SetFeature(uint8_t* buffer, int len) = 0;
		virtual bool GetFeature(uint8_t* buffer, int len) = 0;
		// Advisory read from the control pipe (v7 read-back, v1-v4 status).
		virtual bool GetInputReport(uint8_t* buffer, int len) = 0;
	};

	// Enumerate every HID interface on the system (implemented per platform).
	std::vector<HidDeviceInfo> HidEnumerate();

	// Open one interface for report I/O. Returns nullptr on failure
	// (e.g. missing Input Monitoring permission on macOS, exclusive
	// access on Windows).
	std::unique_ptr<IHidTransport> HidOpen(const HidDeviceInfo& info);

	// Millisecond sleep for protocol pacing (implemented in shared code).
	void AfxSleepMs(unsigned ms);
}
