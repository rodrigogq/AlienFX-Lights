// HidTransport_IOKit.cpp — macOS implementation of AFXHidTransport.h over
// IOKit HID. Holds the platform quirks that used to live in AlienFX_SDK.cpp:
//  - For unnumbered reports (ID 0) the ID byte is stripped before hitting
//    the wire, like hidapi does.
//  - lenAdjust: some HID descriptors report sizes off-by-one compared to the
//    Windows report byte length the protocol assumes; the first failing
//    SetReport self-calibrates and the correction sticks for the device.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#if defined(__APPLE__)

#include "AFXHidTransport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>

using namespace std;

// Runtime debug logging: set ALIENFX_DEBUG=1 in the environment.
static bool AFXDebugEnabled() {
	static int enabled = -1;
	if (enabled < 0) {
		const char* e = getenv("ALIENFX_DEBUG");
		enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	return enabled;
}
#define DebugPrint(_x_) do { if (AFXDebugEnabled()) fprintf(stderr, "[AlienFX] %s", string(_x_).c_str()); } while (0)

namespace AlienFX_SDK {

	static int32_t GetHIDIntProperty(IOHIDDeviceRef device, CFStringRef key) {
		int32_t value = 0;
		CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
		if (ref && CFGetTypeID(ref) == CFNumberGetTypeID())
			CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
		return value;
	}

	static string GetHIDStringProperty(IOHIDDeviceRef device, CFStringRef key) {
		CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
		if (ref && CFGetTypeID(ref) == CFStringGetTypeID()) {
			char buf[256] = {0};
			if (CFStringGetCString((CFStringRef)ref, buf, sizeof(buf), kCFStringEncodingUTF8))
				return string(buf);
		}
		return string();
	}

	class HidTransportIOKit : public IHidTransport {
	public:
		HidTransportIOKit(IOHIDDeviceRef device) : devHandle(device) {} // takes ownership (retained+open)

		~HidTransportIOKit() override {
			if (devHandle) {
				IOHIDDeviceClose(devHandle, kIOHIDOptionsTypeNone);
				CFRelease(devHandle);
			}
		}

		bool SetOutputReport(uint8_t* buffer, int len) override {
			return HidSetReport(kIOHIDReportTypeOutput, buffer, len);
		}

		bool SetFeature(uint8_t* buffer, int len) override {
			return HidSetReport(kIOHIDReportTypeFeature, buffer, len);
		}

		bool GetFeature(uint8_t* buffer, int len) override {
			return HidGetReport(kIOHIDReportTypeFeature, buffer, len);
		}

		bool GetInputReport(uint8_t* buffer, int len) override {
			return HidGetReport(kIOHIDReportTypeInput, buffer, len);
		}

	private:
		IOHIDDeviceRef devHandle;
		int lenAdjust = 0; // learned off-by-one correction for this device's reports

		bool HidSetReport(IOHIDReportType type, uint8_t* buffer, int len) {
			if (!devHandle) return false;
			const uint8_t* data = buffer;
			CFIndex l = len;
			if (buffer[0] == 0) { data = buffer + 1; l = len - 1; }
			l += lenAdjust;
			IOReturn ret = IOHIDDeviceSetReport(devHandle, type, buffer[0], data, l);
			if (ret != kIOReturnSuccess) {
				DebugPrint("SetReport(type " + to_string(type) + ", id " + to_string(buffer[0])
					+ ", len " + to_string(l) + ") failed: 0x" + to_string(ret) + "\n");
				// Self-calibrate: some HID descriptors report sizes off-by-one
				// compared to the Windows report byte length the protocol assumes.
				if (!lenAdjust && l > 1) {
					ret = IOHIDDeviceSetReport(devHandle, type, buffer[0], data, l - 1);
					if (ret == kIOReturnSuccess) {
						lenAdjust = -1;
						DebugPrint("SetReport retry with len " + to_string(l - 1) + " OK - using it from now on\n");
						return true;
					}
				}
				return false;
			}
			return true;
		}

		bool HidGetReport(IOHIDReportType type, uint8_t* buffer, int len) {
			if (!devHandle) return false;
			uint8_t* data = buffer;
			CFIndex l = len;
			if (buffer[0] == 0) { data = buffer + 1; l = len - 1; }
			return IOHIDDeviceGetReport(devHandle, type, buffer[0], data, &l) == kIOReturnSuccess;
		}
	};

	vector<HidDeviceInfo> HidEnumerate() {
		vector<HidDeviceInfo> result;
		io_iterator_t iter = IO_OBJECT_NULL;
		CFMutableDictionaryRef matching = IOServiceMatching(kIOHIDDeviceKey);
		if (matching && IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) == KERN_SUCCESS) {
			io_object_t service;
			while ((service = IOIteratorNext(iter))) {
				IOHIDDeviceRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
				if (device) {
					HidDeviceInfo info;
					info.vid = (uint16_t)GetHIDIntProperty(device, CFSTR(kIOHIDVendorIDKey));
					info.pid = (uint16_t)GetHIDIntProperty(device, CFSTR(kIOHIDProductIDKey));
					info.usagePage = (uint16_t)GetHIDIntProperty(device, CFSTR(kIOHIDPrimaryUsagePageKey));
					info.usage = (uint16_t)GetHIDIntProperty(device, CFSTR(kIOHIDPrimaryUsageKey));
					info.maxInput = GetHIDIntProperty(device, CFSTR(kIOHIDMaxInputReportSizeKey));
					info.maxOutput = GetHIDIntProperty(device, CFSTR(kIOHIDMaxOutputReportSizeKey));
					info.maxFeature = GetHIDIntProperty(device, CFSTR(kIOHIDMaxFeatureReportSizeKey));
					info.manufacturer = GetHIDStringProperty(device, CFSTR(kIOHIDManufacturerKey));
					info.product = GetHIDStringProperty(device, CFSTR(kIOHIDProductKey));
					info.transport = GetHIDStringProperty(device, CFSTR(kIOHIDTransportKey));
					uint64_t entryID = 0;
					if (IORegistryEntryGetRegistryEntryID(service, &entryID) == KERN_SUCCESS)
						info.registryEntryId = entryID;
					result.push_back(std::move(info));
					CFRelease(device);
				}
				IOObjectRelease(service);
			}
			IOObjectRelease(iter);
		}
		return result;
	}

	unique_ptr<IHidTransport> HidOpen(const HidDeviceInfo& info) {
		if (!info.registryEntryId)
			return nullptr;
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
			IORegistryEntryIDMatching(info.registryEntryId));
		if (!service)
			return nullptr;
		IOHIDDeviceRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
		IOObjectRelease(service);
		if (!device)
			return nullptr;
		IOReturn openRet = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
		if (openRet != kIOReturnSuccess) {
			DebugPrint("Device open failed: 0x" + to_string(openRet)
				+ " (a keyboard-class interface may require the Input Monitoring permission)\n");
			CFRelease(device);
			return nullptr;
		}
		return unique_ptr<IHidTransport>(new HidTransportIOKit(device));
	}
}

#endif // __APPLE__
