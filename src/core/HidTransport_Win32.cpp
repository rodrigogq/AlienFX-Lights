// HidTransport_Win32.cpp — Windows implementation of AFXHidTransport.h over
// the Win32 HID stack (SetupAPI enumeration + hid.dll report I/O).
//
// Conventions (matching upstream alienfx-tools and hidapi):
//  - buffer[0] always holds the report ID (0 for unnumbered reports) and is
//    sent as-is; Windows report byte lengths natively include that byte, so
//    no length adjustment is ever needed here.
//  - Output reports go through overlapped WriteFile with a bounded wait, so
//    a NAKing device cannot wedge the UI thread. Feature reports use the
//    synchronous HidD_SetFeature/HidD_GetFeature IOCTLs, which are safe on
//    an overlapped handle.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#if defined(_WIN32)

#include "AFXHidTransport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

// Runtime debug logging: set ALIENFX_DEBUG=1 in the environment.
static bool AFXDebugEnabled() {
	static int enabled = -1;
	if (enabled < 0) {
		const char* e = getenv("ALIENFX_DEBUG");
		enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	return enabled;
}
#define DebugPrint(_x_) do { if (AFXDebugEnabled()) fprintf(stderr, "[AlienFX] %s", std::string(_x_).c_str()); } while (0)

namespace AlienFX_SDK {

	static std::string WideToUtf8(const wchar_t* w) {
		if (!w || !*w) return std::string();
		int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
		if (len <= 1) return std::string();
		std::string out(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
		return out;
	}

	static std::wstring Utf8ToWide(const std::string& s) {
		if (s.empty()) return std::wstring();
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
		if (len <= 1) return std::wstring();
		std::wstring out(len - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
		return out;
	}

	class HidTransportWin32 : public IHidTransport {
	public:
		HidTransportWin32(HANDLE handle, const HIDP_CAPS& caps)
			: dev(handle), outputLen(caps.OutputReportByteLength),
			  featureLen(caps.FeatureReportByteLength), inputLen(caps.InputReportByteLength) {
			ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		}

		~HidTransportWin32() override {
			if (dev != INVALID_HANDLE_VALUE) {
				CancelIo(dev);
				CloseHandle(dev);
			}
			if (ioEvent) CloseHandle(ioEvent);
		}

		bool SetOutputReport(uint8_t* buffer, int len) override {
			if (dev == INVALID_HANDLE_VALUE || !outputLen || !ioEvent) return false;
			// The driver expects exactly OutputReportByteLength bytes.
			uint8_t report[MAX_REPORT] = {};
			memcpy(report, buffer, (size_t)(len < outputLen ? len : outputLen));

			OVERLAPPED ov = {};
			ov.hEvent = ioEvent;
			ResetEvent(ioEvent);
			DWORD written = 0;
			if (!WriteFile(dev, report, outputLen, nullptr, &ov)
				&& GetLastError() != ERROR_IO_PENDING) {
				DebugPrint("WriteFile(output report, id " + std::to_string(buffer[0])
					+ ") failed: " + std::to_string(GetLastError()) + "\n");
				return false;
			}
			if (WaitForSingleObject(ioEvent, kWriteTimeoutMs) != WAIT_OBJECT_0) {
				// Bounded wait: a device that never completes the interrupt
				// transfer must not hang the caller.
				CancelIoEx(dev, &ov);
				GetOverlappedResult(dev, &ov, &written, TRUE); // reap the cancel
				DebugPrint("WriteFile(output report) timed out\n");
				return false;
			}
			return GetOverlappedResult(dev, &ov, &written, FALSE) && written == outputLen;
		}

		bool SetFeature(uint8_t* buffer, int len) override {
			if (dev == INVALID_HANDLE_VALUE || !featureLen) return false;
			uint8_t report[MAX_REPORT] = {};
			memcpy(report, buffer, (size_t)(len < featureLen ? len : featureLen));
			if (!HidD_SetFeature(dev, report, featureLen)) {
				DebugPrint("HidD_SetFeature(id " + std::to_string(buffer[0])
					+ ") failed: " + std::to_string(GetLastError()) + "\n");
				return false;
			}
			return true;
		}

		bool GetFeature(uint8_t* buffer, int len) override {
			if (dev == INVALID_HANDLE_VALUE || !featureLen) return false;
			uint8_t report[MAX_REPORT] = {};
			report[0] = buffer[0]; // report ID selects what to read
			if (!HidD_GetFeature(dev, report, featureLen))
				return false;
			memcpy(buffer, report, (size_t)(len < featureLen ? len : featureLen));
			return true;
		}

		bool GetInputReport(uint8_t* buffer, int len) override {
			if (dev == INVALID_HANDLE_VALUE || !inputLen) return false;
			uint8_t report[MAX_REPORT] = {};
			report[0] = buffer[0];
			if (!HidD_GetInputReport(dev, report, inputLen))
				return false;
			memcpy(buffer, report, (size_t)(len < inputLen ? len : inputLen));
			return true;
		}

	private:
		static const int MAX_REPORT = 1024; // > any HID report we drive (<= 65)
		static const DWORD kWriteTimeoutMs = 500;

		HANDLE dev;
		USHORT outputLen, featureLen, inputLen;
		HANDLE ioEvent;
	};

	std::vector<HidDeviceInfo> HidEnumerate() {
		std::vector<HidDeviceInfo> result;

		GUID hidGuid;
		HidD_GetHidGuid(&hidGuid);
		HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
			DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (devs == INVALID_HANDLE_VALUE)
			return result;

		SP_DEVICE_INTERFACE_DATA ifData;
		ifData.cbSize = sizeof(ifData);
		for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, idx, &ifData); idx++) {
			DWORD needed = 0;
			SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &needed, nullptr);
			if (!needed) continue;
			std::vector<uint8_t> detailBuf(needed);
			auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
			detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, needed, nullptr, nullptr))
				continue;

			// Open with access 0: lets us read attributes/caps even from
			// interfaces the OS holds exclusively (keyboards, mice).
			HANDLE h = CreateFileW(detail->DevicePath, 0,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				continue;

			HIDD_ATTRIBUTES attrs;
			attrs.Size = sizeof(attrs);
			PHIDP_PREPARSED_DATA ppd = nullptr;
			if (HidD_GetAttributes(h, &attrs) && HidD_GetPreparsedData(h, &ppd)) {
				HIDP_CAPS caps = {};
				if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
					HidDeviceInfo info;
					info.vid = attrs.VendorID;
					info.pid = attrs.ProductID;
					info.usagePage = caps.UsagePage;
					info.usage = caps.Usage;
					// Windows report byte lengths include the report-ID byte
					// - exactly the convention the probe table assumes.
					info.maxInput = caps.InputReportByteLength;
					info.maxOutput = caps.OutputReportByteLength;
					info.maxFeature = caps.FeatureReportByteLength;
					wchar_t wstr[256];
					if (HidD_GetManufacturerString(h, wstr, sizeof(wstr)))
						info.manufacturer = WideToUtf8(wstr);
					if (HidD_GetProductString(h, wstr, sizeof(wstr)))
						info.product = WideToUtf8(wstr);
					info.transport = "USB";
					info.path = WideToUtf8(detail->DevicePath);
					result.push_back(std::move(info));
				}
				HidD_FreePreparsedData(ppd);
			}
			CloseHandle(h);
		}
		SetupDiDestroyDeviceInfoList(devs);
		return result;
	}

	std::unique_ptr<IHidTransport> HidOpen(const HidDeviceInfo& info) {
		if (info.path.empty())
			return nullptr;
		std::wstring path = Utf8ToWide(info.path);

		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
			// Reads are advisory (v7 read-back, status polls) and degrade
			// gracefully - try write-only before giving up.
			h = CreateFileW(path.c_str(), GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED, nullptr);
		}
		if (h == INVALID_HANDLE_VALUE) {
			DebugPrint("CreateFile(" + info.path + ") failed: "
				+ std::to_string(GetLastError()) + "\n");
			return nullptr;
		}

		PHIDP_PREPARSED_DATA ppd = nullptr;
		HIDP_CAPS caps = {};
		bool haveCaps = HidD_GetPreparsedData(h, &ppd)
			&& HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS;
		if (ppd) HidD_FreePreparsedData(ppd);
		if (!haveCaps) {
			CloseHandle(h);
			return nullptr;
		}
		return std::unique_ptr<IHidTransport>(new HidTransportWin32(h, caps));
	}
}

#endif // _WIN32
