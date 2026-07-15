// AlienFX_SDK.h — cross-platform port of the AlienFX SDK from
// https://github.com/T-Troll/alienfx-tools (MIT-licensed upstream, by T-Troll
// and contributors, based on Gurjot95's AlienFX_SDK).
// Original macOS port groundwork by kingo132 (AlienFX-For-MacOS).
// Platform HID access (IOKit on macOS, Win32 HID on Windows) is isolated
// behind AFXHidTransport.h; registry-based mapping persistence removed
// (the app persists its own state).
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "AFXHidTransport.h"

namespace AlienFX_SDK {

	typedef uint8_t  byte;
	typedef uint8_t  BYTE;
	typedef uint8_t  UCHAR;
	typedef uint16_t WORD;
	typedef uint32_t DWORD;

	// Statuses for v1-v3
	#define ALIENFX_V2_READY 0x10
	#define ALIENFX_V2_BUSY 0x11
	#define ALIENFX_V2_UNKNOWN 0x12
	// Statuses for apiv4
	#define ALIENFX_V4_READY 33
	#define ALIENFX_V4_BUSY 34
	#define ALIENFX_V4_WAITCOLOR 35
	#define ALIENFX_V4_WAITUPDATE 36
	#define ALIENFX_V4_WASON 38
	// Statuses for apiv5
	#define ALIENFX_V5_STARTCOMMAND 0x8c
	#define ALIENFX_V5_WAITUPDATE 0x80
	#define ALIENFX_V5_INCOMMAND 0xcc

	// Mapping flags:
	#define ALIENFX_FLAG_POWER		1 // This is power button
	#define ALIENFX_FLAG_INDICATOR	2 // This is indicator light (keep at lights off)

	// Maximal buffer size across all device types
	#define MAX_BUFFERSIZE 193

	union Afx_colorcode // Atomic color structure
	{
		struct {
			byte b, g, r;
			byte br; // Brightness
		};
		DWORD cf;
		Afx_colorcode(DWORD ci) : cf(ci) {}
		Afx_colorcode(byte b, byte g, byte r) : b(b), g(g), r(r), br(0) {}
		Afx_colorcode(byte b, byte g, byte r, byte br) : b(b), g(g), r(r), br(br) {}
	};

	struct Afx_icommand {
		int i;
		std::vector<byte> vval;
	};

	struct Afx_light { // Light information block
		byte lightid;
		union {
			struct {
				WORD flags;
				WORD scancode;
			};
			DWORD data;
		};
		std::string name;
	};

	union Afx_groupLight {
		struct {
			WORD did, lid;
		};
		DWORD lgh;
	};

	struct Afx_group { // Light group information block
		DWORD gid;
		std::string name;
		std::vector<Afx_groupLight> lights;
	};

	struct Afx_action { // atomic light action phase
		BYTE type; // one of Action values - action type
		BYTE time; // How long this phase stay
		BYTE tempo; // How fast it should transform
		BYTE r, g, b; // phase color
	};

	struct Afx_lightblock { // light action block
		byte index;
		std::vector<Afx_action> act;
	};

	enum Afx_Version {
		// Dell/Alienware tri-mode wireless (AW920K, 413c:2126). Distinct from
		// the generic Chicony v8 protocol; decoded from a USB capture. See
		// docs/aw920k-protocol.md.
		API_AW920K = 10, //65, report ID 0, first data byte 0x0e
		API_V8 = 8, //65
		API_V7 = 7, //65
		API_V6 = 6, //65
		API_V5 = 5, //64
		API_V4 = 4, //34
		API_V3 = 3, //12
		API_V2 = 2, //9
		API_UNKNOWN = -1
	};

	enum Action	{
		AlienFX_A_Color = 0,
		AlienFX_A_Pulse = 1,
		AlienFX_A_Morph = 2,
		AlienFX_A_Breathing= 3,
		AlienFX_A_Spectrum = 4,
		AlienFX_A_Rainbow = 5,
		AlienFX_A_Power = 6
	};

	class Functions
	{
	private:

		std::unique_ptr<IHidTransport> transport; // open HID device, null if not initialized

		bool inSet = false;

		int length = -1; // HID report length (Windows convention: includes report-ID byte)
		byte chain = 1; // seq. number for APIv1-v3

		// support function for mask-based devices (v1-v3, v6)
		std::vector<Afx_icommand>* SetMaskAndColor(std::vector<Afx_icommand>* mods, Afx_lightblock* act, bool needInverse = false, DWORD index = 0);

		// Support function to send data to USB device
		bool PrepareAndSend(const byte* command, std::vector<Afx_icommand> mods);

		// Low-level report I/O, forwarded to the platform transport. Buffers
		// use the Windows/hidapi convention: buffer[0] is the report ID
		// (0 for unnumbered reports).
		bool HidSetFeature(byte* buffer, int len);
		bool HidSetOutputReport(byte* buffer, int len);
		bool HidGetFeature(byte* buffer, int len);
		bool HidGetInputReport(byte* buffer, int len);

		// Add new light effect block for v8
		inline void AddV8DataBlock(byte bPos, std::vector<Afx_icommand>* mods, Afx_lightblock* act);

		// Add new color block for v5
		inline void AddV5DataBlock(byte bPos, std::vector<Afx_icommand>* mods, byte index, Afx_action* act);

		// Support function to send whole power block for v1-v3
		void SavePowerBlock(byte blID, Afx_lightblock* act, bool needSave, bool needSecondary = false, bool needInverse = false);

		// Support function for APIv4 action set
		bool SetV4Action(Afx_lightblock* act);

		// return current device state
		BYTE GetDeviceStatus();

		// AW920K helpers (see docs/aw920k-protocol.md)
		void AWCommit();
		void AWBaseLayer(byte r, byte g, byte b);

		// Next command delay for APIv1-v3
		BYTE WaitForReady();

		// After-reset delay for APIv1-v3
		BYTE WaitForBusy();

	public:
		union {
			struct {
				WORD pid, vid;			// Device IDs
			};
			DWORD devID;
		};
		int version = API_UNKNOWN; // interface version, will stay at API_UNKNOWN if not initialized
		byte bright = 64; // Last brightness set for device
		std::string description; // device description

		~Functions();

		// Support function to send data to USB device
		bool PrepareAndSend(const byte* command, std::vector<Afx_icommand>* mods = NULL);

		// Check a single enumerated HID interface and initialize data.
		// vid/pid of 0 match any vendor/product.
		// Returns true if the device is a supported AlienFX light controller
		// (the device is opened in that case).
		bool AlienFXProbeDevice(const HidDeviceInfo& info, WORD vid = 0, WORD pid = 0);

		// Find and initialize the first matching device in the system.
		// If vid is 0 or absent, first device found into the system will be used, otherwise device with this VID only.
		// If pid is 0 or absent, first device with any pid and given vid will be used, otherwise vid/pid pair.
		// Returns true if device found and initialized.
		bool AlienFXInitialize(WORD vid = 0, WORD pid = 0);

		// Release the underlying HID device.
		void AlienFXClose();

		// Prepare to set lights
		bool Reset();

		// false - not ready, true - ready, 0xff - stalled
		BYTE IsDeviceReady();

		// Basic color set for current device.
		// index - light ID
		// c - color action structure
		// It's a synonym of SetAction, but with one color action
		bool SetColor(byte index, Afx_action c);

		// Set multiply lights to the same color. This only works for some API devices, and emulated for other ones.
		// lights - pointer to vector of light IDs need to be set.
		// c - color to set
		bool SetMultiColor(std::vector<byte> *lights, Afx_action c);

		// Set multiply lights to different color.
		// act - pointer to vector of light control blocks (each define one light)
		// store - need to save settings into device memory (v1-v4)
		bool SetMultiAction(std::vector<Afx_lightblock> *act, bool store = false);

		// Set color to action
		// act - pointer to light actions vector
		bool SetAction(Afx_lightblock* act);

		// Save lights state as power-on defaults
		// act - vector of lights to set, including power button (if any)
		void SaveLightsState(std::vector<Afx_lightblock>* act);

		// Set action for Power button
		// act - pointer to light control block
		bool SetPowerAction(Afx_lightblock *act);

		// Hardware brightness for device, if supported
		// brightness - desired brightness (0 - off, 255 - full)
		// gbr - global device brightness from mappings
		// mappings - mappings list for v4 brightness set (it require light IDs list)
		// power - if true, power and indicator lights will be set too
		bool SetBrightness(BYTE brightness, BYTE gbr, std::vector <Afx_light>* mappings, bool power);

		// Global (whole device) effect control for APIv5, v8
		// effType - effect type
		// mode - effect mode (off, steady, keypress, etc)
		// nc - number of colors (3 - spectrum)
		// tempo - effect tempo
		// act1 - first effect color
		// act2 - second effect color (not for all effects)
		bool SetGlobalEffects(byte effType, byte mode, byte nc, byte tempo, Afx_colorcode act1, Afx_colorcode act2);

		// Apply changes and update colors
		bool UpdateColors();

		// check global effects availability
		bool IsHaveGlobal();

		// --- Alienware tri-mode wireless (AW920K) ---------------------------
		// Dedicated packet path decoded from a USB capture of AWCC; see
		// docs/aw920k-protocol.md. Report ID 0, first data byte 0x0e.

		// The keyboard has two lighting planes: permanent color + pressed-key
		// highlight. bright is 0..0x0a.
		bool AWApply(byte pr, byte pg, byte pb, byte kr, byte kg, byte kb, byte bright);

		// Static color across the whole keyboard (both planes).
		bool AWStaticColor(byte r, byte g, byte b, byte bright);

		// Keys dark until pressed; pressed keys light in the given color.
		bool AWKeyPressColor(byte r, byte g, byte b, byte bright);

		// EXPERIMENTAL permanent-plane effect. Effect IDs follow the pattern
		// (v8 opcode - 0x80), confirmed for 0x01 static / 0x08 spectrum /
		// 0x09 keypress: 0x02 pulse, 0x03 morph, 0x07 breathing.
		bool AWEffect(byte effectId, byte r, byte g, byte b, byte bright);

		// Whole-device brightness ("Go Light/Dim/Dark"): 0 = off .. 0x0a = full.
		// Preserves the last color, like AWCC's system lighting mode.
		bool AWGlobalBrightness(byte level);

		// Spectrum (rainbow cycle) effect at the given brightness.
		bool AWSpectrum(byte bright);
	};

	struct Afx_device { // Single device data
		union {
			struct {
				WORD pid, vid;			// IDs
			};
			DWORD devID;
		};
		Functions* dev = NULL;		// device control object pointer
		std::string name;			// device name
		Afx_colorcode white = 0xffffffff; // white balance
		byte brightness = 255;		// global device brightness
		std::vector <Afx_light> lights;	// vector of lights defined
		bool arrived = false, present = false; // for newly arrived and present devices
	};

	class Mappings {
	private:
		std::vector<Afx_group> groups; // Defined light groups

	public:

		std::vector<Afx_device> fxdevs; // main devices/mappings array
		unsigned activeLights = 0,  // total number of active lights into the system
				 activeDevices = 0; // total number of active devices
		bool deviceListChanged = false; // Is list changed after last device scan?

		~Mappings();

		// Update device info after it found into the system
		void AlienFxUpdateDevice(Functions* dev);

		// Enumerate all alienware devices into the system
		// returns true if light device list was changed
		bool AlienFXEnumDevices();

		// Set device brightness
		// dev - point to AFX device info
		// br - brightness level
		// power - set power/indicator lights too
		bool SetDeviceBrightness(Afx_device* dev, BYTE br, bool power);

		// get defined groups vector
		std::vector <Afx_group>* GetGroups();

		// get device structure by PID/VID.
		// VID can be zero for any VID
		Afx_device* GetDeviceById(WORD pid, WORD vid = 0);

		// get device by VID/PID DWORD.
		Afx_device* GetDeviceById(DWORD devID);

		// get or add device structure by PID/VID
		Afx_device* AddDeviceById(DWORD devID);

		// find light mapping into device structure by light ID
		Afx_light* GetMappingByDev(Afx_device* dev, WORD LightID);

		// find light group by it's ID
		Afx_group* GetGroupById(DWORD gid);

		// remove light mapping from device by id
		void RemoveMapping(Afx_device* dev, WORD lightID);

		// get light flags (Power, indicator, etc) from light structure
		int GetFlags(Afx_device* dev, WORD lightid);
	};

}
