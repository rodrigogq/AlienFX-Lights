// AlienFX_SDK.cpp — cross-platform port of the AlienFX SDK from
// https://github.com/T-Troll/alienfx-tools (MIT-licensed upstream, by T-Troll
// and contributors, based on Gurjot95's AlienFX_SDK).
// Original macOS port groundwork by kingo132 (AlienFX-For-MacOS).
//
// Port notes (vs. upstream Windows code):
//  - Platform HID access is behind AFXHidTransport.h (IOKit on macOS,
//    Win32 HID on Windows). Report buffers keep the Windows convention:
//    buffer[0] holds the report ID and `length` includes that byte.
//  - Registry-based mapping persistence and ACPI lights removed.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#include "AlienFX_SDK.h"
#include "alienfx-controls.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

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

	void AfxSleepMs(unsigned ms) {
		this_thread::sleep_for(chrono::milliseconds(ms));
	}

	bool Functions::HidSetFeature(byte* buffer, int len) {
		return transport && transport->SetFeature(buffer, len);
	}

	bool Functions::HidSetOutputReport(byte* buffer, int len) {
		return transport && transport->SetOutputReport(buffer, len);
	}

	bool Functions::HidGetFeature(byte* buffer, int len) {
		return transport && transport->GetFeature(buffer, len);
	}

	bool Functions::HidGetInputReport(byte* buffer, int len) {
		return transport && transport->GetInputReport(buffer, len);
	}

	vector<Afx_icommand> *Functions::SetMaskAndColor(vector<Afx_icommand>* mods, Afx_lightblock* act, bool needInverse, DWORD index) {
		Afx_colorcode c = index ? index : needInverse ? ~((1 << act->index)) : 1 << act->index;
		if (version < API_V4) {
			// index mask generation
			*mods = { {1, { v1OpCodes[act->act.front().type], chain, c.r, c.g, c.b } } };
		}
		Afx_action c1 = act->act.front(), c2 = act->act.size() < 2 ? Afx_action({ 0 }) : act->act.back();
		byte tempo = act->act.front().tempo;
		switch (version) {
		case API_V3:
			mods->push_back({6, {c1.r, c1.g,c1.b,
				c2.r,c2.g,c2.b}});
			break;
		case API_V2:
			mods->push_back({6, {(byte)((c1.r & 0xf0) | ((c1.g & 0xf0) >> 4)),
						(byte)((c1.b & 0xf0) | ((c2.r & 0xf0) >> 4)),
						(byte)((c2.g & 0xf0) | ((c2.b & 0xf0) >> 4))}});
			break;
		case API_V6: {
			*mods = { { 9, { (byte)index, c1.r, c1.g, c1.b } } };
			byte mask = (byte)(c1.r ^ c1.g ^ c1.b ^ index);
			switch (c1.type) {
			case AlienFX_A_Color: {
				// Full command block as verified on real monitors by OpenRGB's
				// AlienwareMonitorController: 0a 00 51 87 d0 04 [zonemask] [rgb]
				// [brightness] [checksum], checksum = 0x6e ^ xor(bytes 5..13).
				byte checksum = (byte)(0x6e ^ 0x51 ^ 0x87 ^ 0xd0 ^ 0x04
					^ (byte)index ^ c1.r ^ c1.g ^ c1.b ^ bright);
				*mods = { {3, {0x0a, 0x00, 0x51, 0x87, 0xd0, 0x04,
					(byte)index, c1.r, c1.g, c1.b, bright, checksum}} };
			} break;
			case AlienFX_A_Pulse:
				mask ^= byte(tempo ^ 1);
				mods->insert(mods->end(), {{3, {0xb}}, {6, {0x88}}, {8, {2}}, {13, {bright, tempo}}, {15, {mask}} });
				break;
			case AlienFX_A_Breathing:
				c2 = { 0 };
				[[fallthrough]];
			case AlienFX_A_Morph:
				mask ^= (byte)(c2.r ^ c2.g ^ c2.b ^ tempo ^ 4);
				mods->insert(mods->end(), { {3, {0xf}}, {6, {0x8c}}, {8, {1}},
					{13, {c2.r,c2.g,c2.b}},
					{16, {bright, 2, tempo,mask}} });
				break;
			}
		} break;
		}
		return mods;
	}

	inline bool Functions::PrepareAndSend(const byte* command, vector<Afx_icommand> mods) {
		return PrepareAndSend(command, &mods);
	}

	bool Functions::PrepareAndSend(const byte *command, vector<Afx_icommand> *mods) {

		if (transport) { // Is device initialized?

			byte buffer[MAX_BUFFERSIZE];
			bool needV8Feature = true;

			memset(buffer, version == API_V6 ? 0xff : 0, length);
			memcpy(buffer, command, command[0] + 1);
			buffer[0] = reportIDList[version];

			if (mods) {
				for (auto i = mods->begin(); i < mods->end(); i++)
					memcpy(buffer + i->i, i->vval.data(), i->vval.size());
				needV8Feature = mods->front().vval.size() == 1;
				mods->clear();
			}

			switch (version) {
			case API_V2: case API_V3: case API_V4:
				return HidSetOutputReport(buffer, length);
			case API_V5:
				return HidSetFeature(buffer, length);
			case API_V6: {
				// Monitors want a pause between packets (verified by OpenRGB).
				bool res = HidSetOutputReport(buffer, length);
				AfxSleepMs(50);
				return res;
			}
			case API_V7: {
				// Windows version writes, then reads back a reply. Some devices
				// don't answer control GET_REPORT, so treat the read as advisory.
				bool res = HidSetOutputReport(buffer, length);
				HidGetInputReport(buffer, length);
				return res;
			}
			case API_V8:
				if (needV8Feature) {
					AfxSleepMs(4);
					bool res = HidSetFeature(buffer, length);
					AfxSleepMs(6);
					return res;
				}
				else
				{
					return HidSetOutputReport(buffer, length);
				}
			}
		}
		return false;
	}

	void Functions::SavePowerBlock(byte blID, Afx_lightblock* act, bool needSave, bool needSecondary, bool needInverse) {
		vector<Afx_icommand> mods, group = { {2, {blID}} };
		PrepareAndSend(COMMV1_saveGroup, &group);
		PrepareAndSend(COMMV1_color, SetMaskAndColor(&mods, act));
		if (needSecondary) {
			Afx_lightblock t = *act;
			swap(t.act.front(), t.act.back());
			PrepareAndSend(COMMV1_saveGroup, &group);
			PrepareAndSend(COMMV1_color, SetMaskAndColor(&mods, &t));
		}
		if (needInverse) {
			Afx_lightblock t = { act->index, { {AlienFX_A_Color}, {AlienFX_A_Color}} };
			PrepareAndSend(COMMV1_saveGroup, &group);
			PrepareAndSend(COMMV1_loop);
			chain++;
			PrepareAndSend(COMMV1_saveGroup, &group);
			PrepareAndSend(COMMV1_color, SetMaskAndColor(&mods, &t, true));
		}
		PrepareAndSend(COMMV1_saveGroup, &group);
		PrepareAndSend(COMMV1_loop);
		chain++;

		if (needSave) {
			PrepareAndSend(COMMV1_save);
			Reset();
		}
	}

	bool Functions::AlienFXProbeDevice(const HidDeviceInfo& info, WORD vidd, WORD pidd) {
		version = API_UNKNOWN;
		if ((vidd && info.vid != vidd) || (pidd && info.pid != pidd))
			return false;

		DebugPrint("Probe: vid " + to_string(info.vid) + ", pid " + to_string(info.pid)
			+ ", usage " + to_string(info.usage)
			+ ", maxOut " + to_string(info.maxOutput) + ", maxFeature " + to_string(info.maxFeature) + "\n");

		vid = info.vid;
		pid = info.pid;

		// Windows report byte lengths include the report-ID byte; IOKit's max
		// report sizes may or may not, depending on the descriptor. Accept both
		// and normalize `length` to the canonical (Windows) value per API.
		switch (vid) {
		case 0x0d62: // Darfon - notebook RGB keyboards
			if (info.usage == 0xcc && !info.maxOutput && info.maxFeature) {
				length = 64;
				version = API_V5;
			}
			break;
		case 0x187c: // Alienware
			switch (info.maxOutput) {
			case 8: case 9:
				length = 9;
				version = API_V2;
				break;
			case 11: case 12:
				length = 12;
				version = API_V3;
				break;
			case 33: case 34:
				length = 34;
				version = API_V4;
				break;
			case 64: case 65:
				length = 65;
				version = API_V6;
				break;
			}
			break;
		default:
			if (info.maxOutput == 64 || info.maxOutput == 65)
				switch (vid) {
				case 0x0424: // Microchip - monitors
					if (pid != 0x274c) {
						length = 65;
						version = API_V6;
					}
					break;
				case 0x0461: // Primax - mouses
					length = 65;
					version = API_V7;
					break;
				case 0x04f2: // Chicony - external keyboards
					length = 65;
					version = API_V8;
					break;
				case 0x413c: // Dell - tri-mode wireless peripherals (via USB receiver)
					// AW920K keyboard (pid 0x2126): dedicated protocol decoded
					// from a USB capture of AWCC (docs/aw920k-protocol.md).
					// Report ID 0, first data byte 0x0e; distinct from v8.
					if (pid == 0x2126 && info.usagePage == 0xff00) {
						length = 65;
						version = API_AW920K;
					}
					break;
				}
		}

		if (version != API_UNKNOWN) {
			transport = HidOpen(info);
			if (transport) {
				description = info.manufacturer;
				if (!description.empty() && !info.product.empty())
					description += " ";
				description += info.product;
			}
			else {
				DebugPrint("Device open failed"
					" (a keyboard-class interface may require an input-monitoring permission,"
					" or another program holds the device exclusively)\n");
				version = API_UNKNOWN;
			}
			DebugPrint("Probe done, type " + to_string(version) + "\n");
		}
		return version != API_UNKNOWN;
	}

	//Use this method for general devices, vid = 0 for any vid, pid = 0 for any pid.
	bool Functions::AlienFXInitialize(WORD vidd, WORD pidd) {
		transport.reset();
		version = API_UNKNOWN;

		for (const HidDeviceInfo& info : HidEnumerate()) {
			if (AlienFXProbeDevice(info, vidd, pidd))
				break;
		}
		return version != API_UNKNOWN;
	}

	void Functions::AlienFXClose() {
		transport.reset();
		version = API_UNKNOWN;
	}

	bool Functions::Reset() {
		switch (version) {
		case API_V6:
			// Initial handshake, once per connection. Packet sequence and
			// delays as verified on hardware by OpenRGB's monitor controller.
			if (chain) {
				inSet = PrepareAndSend(COMMV6_systemReset);
				AfxSleepMs(50);
				PrepareAndSend(COMMV6_modeSet);
				AfxSleepMs(50);
				{
					// Status/ready trigger: 93 37 12 over a zero-filled report
					// (unlike other v6 packets, which are 0xff-filled).
					byte buf[MAX_BUFFERSIZE];
					memset(buf, 0, sizeof(buf));
					buf[1] = 0x93; buf[2] = 0x37; buf[3] = 0x12;
					HidSetOutputReport(buf, length);
				}
				AfxSleepMs(50);
				chain = 0;
			} else
				inSet = true;
			break;
		case API_V5:
		{
			inSet = PrepareAndSend(COMMV5_reset);
			GetDeviceStatus();
		} break;
		case API_V4:
		{
			WaitForReady();
			PrepareAndSend(COMMV4_control, { { 4, {4} } });
			inSet = PrepareAndSend(COMMV4_control, { {4, {1}} });
		} break;
		case API_V3: case API_V2:
		{
			chain = 1;
			inSet = PrepareAndSend(COMMV1_reset);
			WaitForReady();
			DebugPrint("Post-Reset status: " + to_string(GetDeviceStatus()) + "\n");
		} break;
		default: inSet = true;
		}
		return inSet;
	}

	bool Functions::UpdateColors() {
		if (inSet) {
			switch (version) {
			case API_V5:
			{
				inSet = !PrepareAndSend(COMMV5_update);
			} break;
			case API_V4:
			{
				inSet = !PrepareAndSend(COMMV4_control);
			} break;
			case API_V3: case API_V2:
			{
				inSet = !PrepareAndSend(COMMV1_update);
				DebugPrint("Post-update status: " + to_string(GetDeviceStatus()) + "\n");
			} break;
			default: inSet = false;
			}
		}
		return !inSet;
	}

	bool Functions::SetColor(byte index, Afx_action c) {
		Afx_lightblock act{ index, {c} };
		return SetAction(&act);
	}

	void Functions::AddV8DataBlock(byte bPos, vector<Afx_icommand>* mods, Afx_lightblock* act) {
		mods->push_back( {bPos, {act->index,v8OpCodes[act->act.front().type], act->act.front().tempo, 0xa5, act->act.front().time, 0xa,
			act->act.front().r, act->act.front().g,act->act.front().b,
			act->act.back().r,act->act.back().g,act->act.back().b,
			2 } } );
	}

	void Functions::AddV5DataBlock(byte bPos, vector<Afx_icommand>* mods, byte index, Afx_action* c) {
		mods->push_back( {bPos, { (byte)(index + 1),c->r,c->g,c->b} });
	}

	bool Functions::SetMultiColor(vector<byte> *lights, Afx_action c) {
		bool val = false;
		Afx_lightblock act{ 0, {c} };
		vector<Afx_icommand> mods;
		if (!inSet) Reset();
		switch (version) {
		case API_V8: {
			PrepareAndSend(COMMV8_readyToColor, { {2,{(byte)lights->size()}} });
			auto nc = lights->begin();
			for (byte cnt = 1; nc != lights->end(); cnt++) {
				for (byte bPos = 5; bPos < length && nc != lights->end(); bPos += 15) {
					act.index = *nc;
					AddV8DataBlock(bPos, &mods, &act);
					nc++;
				}
				if (mods.size()) {
					mods.push_back({ 4, {cnt} });
					val = PrepareAndSend(COMMV8_readyToColor, &mods);
				}
			}
		} break;
		case API_V5:
			for (auto nc = lights->begin(); nc != lights->end();) {
				for (byte bPos = 4; bPos < length && nc != lights->end(); bPos += 4) {
					AddV5DataBlock(bPos, &mods, *nc, &c);
					nc++;
				}
				if (mods.size())
					PrepareAndSend(COMMV5_colorSet, &mods);
			}
			val = PrepareAndSend(COMMV5_loop);
			break;
		case API_V4:
		{
			byte pos = 8;
			for (auto i = lights->begin(); i != lights->end(); i++) {
				if (pos > 33) {
					mods.push_back({ 3, {c.r, c.g, c.b, 0, 26} });
					val = PrepareAndSend(COMMV4_setOneColor, &mods);
					mods.clear();
					pos = 8;
					UpdateColors();
					Reset();
				}
				mods.push_back({ pos++, {*i} });
			}
			if (pos > 8) {
				mods.push_back({ 3, {c.r, c.g, c.b, 0, (byte)(pos - 8)} });
				val = PrepareAndSend(COMMV4_setOneColor, &mods);
			}
		} break;
		case API_V3: case API_V2: case API_V6:
		{
			DWORD fmask = 0;
			for (auto nc = lights->begin(); nc < lights->end(); nc++)
				fmask |= 1 << (*nc);
			SetMaskAndColor(&mods, &act, false, fmask);
			switch (version) {
			case API_V6:
				val = PrepareAndSend(COMMV6_colorSet, &mods);
				break;
			default:
				PrepareAndSend(COMMV1_color, &mods);
				val = PrepareAndSend(COMMV1_loop);
				chain++;
			}
		} break;
		default:
			for (auto nc = lights->begin(); nc < lights->end(); nc++) {
				act.index = *nc;
				val = SetAction(&act);
			}
		}
		return val;
	}

	bool Functions::SetMultiAction(vector<Afx_lightblock> *act, bool save) {
		bool val = true;
		vector<Afx_icommand> mods;

		if (!inSet) Reset();
		switch (version) {
		case API_V8: {
			PrepareAndSend(COMMV8_readyToColor, { {2,{(byte)act->size()}} });
			auto nc = act->begin();
			for (byte cnt = 1; nc != act->end(); cnt++) {
				for (byte bPos = 5; bPos < length && nc != act->end(); bPos+=15) {
					AddV8DataBlock(bPos, &mods, &(*nc));
					nc++;
				}
				mods.push_back({ 4, {cnt} });
				val = PrepareAndSend(COMMV8_readyToColor, &mods);
			}
		} break;
		case API_V5:
		{
			for (auto nc = act->begin(); nc != act->end();) {
				for (byte bPos = 4; bPos < length && nc != act->end(); bPos += 4) {
					AddV5DataBlock(bPos, &mods, nc->index, &nc->act.front());
					nc++;
				}
				PrepareAndSend(COMMV5_colorSet, &mods);
			}
			val = PrepareAndSend(COMMV5_loop);
		} break;
		default:
		{
			// The rest doesn't support bulk light set
			for (auto nc = act->begin(); nc != act->end(); nc++)
				val = SetAction(&(*nc));
		} break;
		}
		if (save) {
			SaveLightsState(act);
		}
		return val;
	}

	bool Functions::SetV4Action(Afx_lightblock* act) {
		bool res = false;
		vector<Afx_icommand> mods;
		PrepareAndSend(COMMV4_colorSel, { {6,{act->index}} });
		for (auto ca = act->act.begin(); ca != act->act.end();) {
			// 3 actions per record..
			for (byte bPos = 3; bPos < length && ca != act->act.end(); bPos += 8) {
				mods.push_back(
					{bPos, {(byte)(ca->type < AlienFX_A_Breathing ? ca->type : AlienFX_A_Morph), ca->time, v4OpCodes[ca->type], 0,
					(byte)(ca->type == AlienFX_A_Color ? 0xfa : ca->tempo), ca->r, ca->g, ca->b} });
				ca++;
			}
			res = PrepareAndSend(COMMV4_colorSet, &mods);
		}
		return res;
	}

	bool Functions::SetAction(Afx_lightblock* act) {
		if (act->act.empty())
			return false;
		if (!inSet) Reset();

		vector<Afx_icommand> mods;
		switch (version) {
		case API_V8:
			AddV8DataBlock(5, &mods, act);
			PrepareAndSend(COMMV8_readyToColor);
			return PrepareAndSend(COMMV8_readyToColor, &mods);
		case API_V7:
		{
			mods = { {5,{v7OpCodes[act->act.front().type],bright,act->index}} };
			for (size_t ca = 0; ca < act->act.size(); ca++) {
				if (ca * 3 + 10 < (size_t)length)
					mods.push_back({ (int)(ca * 3 + 8), {	act->act.at(ca).r, act->act.at(ca).g, act->act.at(ca).b} });
			}
			return PrepareAndSend(COMMV7_control, &mods);
		}
		case API_V6:
			return PrepareAndSend(COMMV6_colorSet, SetMaskAndColor(&mods, act));
		case API_V5:
			AddV5DataBlock(4, &mods, act->index, &act->act.front());
			PrepareAndSend(COMMV5_colorSet, &mods);
			return PrepareAndSend(COMMV5_loop);
		case API_V4: case API_V3: case API_V2: {
			// check types and call
			switch (act->act.front().type) {
			case AlienFX_A_Color: // it's a color, so set as color
				if (version == API_V4)
					return PrepareAndSend(COMMV4_setOneColor, { {3, {act->act.front().r,act->act.front().g,act->act.front().b, 0, 1, (byte)act->index } } });
				break;
			case AlienFX_A_Power: { // Set power button
				return SetPowerAction(act);
			} break;
			default: // Set action
				if (version == API_V4)
					return SetV4Action(act);
				else {
					PrepareAndSend(COMMV1_setTempo,
						{ {2, { (byte)(((WORD)act->act.front().tempo << 3 & 0xff00) >> 8),
							(byte)((WORD)act->act.front().tempo << 3 & 0xff),
							(byte)(((WORD)act->act.front().time << 5 & 0xff00) >> 8),
							(byte)((WORD)act->act.front().time << 5 & 0xff)} } });
					PrepareAndSend(COMMV1_loop);
				}
			}
			for (auto ca = act->act.begin(); ca != act->act.end(); ca++) {
				Afx_lightblock t = { act->index, {*ca} };
				if (act->act.size() > 1)
					t.act.push_back(ca + 1 != act->act.end() ? *(ca + 1) : act->act.front());
				DebugPrint("SDK: Set light " + to_string(act->index) + "\n");
				PrepareAndSend(COMMV1_color, SetMaskAndColor(&mods, &t));
			}
			bool res = PrepareAndSend(COMMV1_loop);
			chain++;
			return res;
		}
		}
		return false;
	}

	void Functions::SaveLightsState(vector<Afx_lightblock>* act) {
		UpdateColors();
		switch (version) {
		case API_V4: {
			PrepareAndSend(COMMV4_control, { { 4, {4,0,0x61} } });
			PrepareAndSend(COMMV4_control, { { 4, {1,0,0x61} } });
			for (auto ca = act->begin(); ca != act->end(); ca++)
				if (ca->act.front().type != AlienFX_A_Power)
					SetV4Action(&(*ca));
			PrepareAndSend(COMMV4_control, { { 4, {2,0,0x61} } });
			PrepareAndSend(COMMV4_control, { { 4, {6,0,0x61} } });
		} break;
		case API_V3: case API_V2:
		{
			Afx_lightblock* pwr = NULL;
			bool wasSave = false;
			for (auto ca = act->begin(); ca != act->end(); ca++)
				if (ca->act.front().type != AlienFX_A_Power) {
					SavePowerBlock(1, &(*ca), false);
					wasSave = true;
				}
				else
					pwr = &(*ca);
			if (wasSave) {
				PrepareAndSend(COMMV1_save);
				Reset();
			}
			if (pwr) {
				SetPowerAction(pwr);
			}
		} break;
		}
	}

	bool Functions::SetPowerAction(Afx_lightblock *act) {
		UpdateColors();
		switch (version) {
		case API_V4:
		{
			// Now set power button....
			for (BYTE cid = 0x5b; cid < 0x61; cid++) {
				// Init query...
				PrepareAndSend(COMMV4_setPower, { {4,{4,0,cid}} });
				PrepareAndSend(COMMV4_setPower, { {4,{1,0,cid}} });
				// Now set color by type...
				Afx_lightblock tact = *act;
				switch (cid) {
				case 0x5b: // AC sleep
					tact.act.push_back(act->act.front());
					tact.act.push_back(Afx_action{ AlienFX_A_Power, 3, 0x64, 0, 0, 0 });
					break;
				case 0x5c: // AC power
					tact.act.push_back(act->act.front());
					tact.act.front().type = AlienFX_A_Color;
					break;
				case 0x5d: // Charge
					tact.act.push_back(act->act.front());
					tact.act.push_back(act->act.back());
					break;
				case 0x5e: // Battery sleep
					tact.act.push_back(act->act.back());
					tact.act.push_back(Afx_action{ AlienFX_A_Power, 3, 0x64, 0, 0, 0 });
					break;
				case 0x5f: // Batt power
					tact.act.push_back(act->act.back());
					tact.act.front().type = AlienFX_A_Color;
					break;
				case 0x60: // Batt critical
					tact.act.push_back(act->act.back());
					tact.act.front().type = AlienFX_A_Pulse;
				}
				SetV4Action(&tact);
				// And finish
				PrepareAndSend(COMMV4_setPower, { {4,{2,0,cid}} });
			}

			PrepareAndSend(COMMV4_control, { { 4, {5} } });
			WaitForBusy();
			WaitForReady();
		} break;
		case API_V3: case API_V2:
		{
			chain = 1;
			Afx_lightblock tact = *act;
			tact.act.front().type = AlienFX_A_Morph;
			tact.act.back() = { AlienFX_A_Morph };
			SavePowerBlock(2, &tact, true, true, true);
			// 08 05 - AC power
			tact.act.front().type = AlienFX_A_Color;
			SavePowerBlock(5, &tact, true);
			// 08 06 - charge
			tact.act.back() = act->act.back();
			tact.act.front().type = tact.act.back().type = AlienFX_A_Morph;
			SavePowerBlock(6, &tact, true, true);
			// 08 07 - Battery standby
			tact.act.front() = act->act.back();
			tact.act.front().type = AlienFX_A_Morph;
			tact.act.back() = { AlienFX_A_Morph };
			SavePowerBlock(7, &tact, true, true, true);
			// 08 08 - battery
			tact.act.front().type = AlienFX_A_Color;
			SavePowerBlock(8, &tact, true);
			// 08 09 - battery critical
			tact.act.front().type = AlienFX_A_Pulse;
			SavePowerBlock(9, &tact, true);
			SetColor(act->index, act->act.front() );
			UpdateColors();
		} break;
		}
		return true;
	}

	bool Functions::SetBrightness(BYTE brightness, BYTE gbr, vector<Afx_light> *mappings, bool power) {
		// return true if update needed
		DebugPrint("State update: PID: " + to_string(pid) + ", brightness: " + to_string(brightness) + ", power: " + to_string(power) + "\n");

		if (inSet) UpdateColors();
		int oldBr = bright;
		bright = (((brightness * gbr) / 255) * brightnessScale[version]) / 0xff;
		switch (version) {
		case API_V8:
			PrepareAndSend(COMMV8_setBrightness, { {2, {bright}} });
			return true;
		case API_V5:
			Reset();
			PrepareAndSend(COMMV5_turnOnSet, { {4, {bright}} });
			return true;
		case API_V4: {
			int pos = 6;
			vector<Afx_icommand> mods;
			for (auto i = mappings->begin(); i < mappings->end(); i++) {
				if (pos > 33) {
					mods.push_back({ 3,{(byte)(0x64 - bright), 0, 28} });
					PrepareAndSend(COMMV4_turnOn, &mods);
					mods.clear();
					pos = 6;
				}
				if (!i->flags || power) {
					mods.push_back({ pos++, {(byte)i->lightid} });
				}
			}
			if (pos > 6) {
				mods.push_back({ 3,{(byte)(0x64 - bright), 0, (byte)(pos - 6)} });
				PrepareAndSend(COMMV4_turnOn, &mods);
			}
			return true;
		}
		case API_V3: case API_V2:
			if (!bright || !oldBr) {
				PrepareAndSend(COMMV1_reset, { {2,{(byte)(brightness ? 4 : power ? 3 : 1)}} });
				WaitForReady();
			}
			PrepareAndSend(COMMV1_dim, { { 2,{bright} } });
			return bright && !oldBr;
		case API_V6: case API_V7:
			return true;
		}
		return false;
	}

	bool Functions::SetGlobalEffects(byte effType, byte mode, byte nc, byte tempo, Afx_colorcode act1, Afx_colorcode act2) {
		switch (version) {
		case API_V8:
			PrepareAndSend(COMMV8_effectReady);
			PrepareAndSend(COMMV8_effectReady, { {3, { effType, act1.r, act1.g, act1.b, act2.r, act2.g, act2.b,
				tempo, bright, 1, mode, nc} }});
			AfxSleepMs(20);
			return true;
		case API_V5:
			if (inSet)
				UpdateColors();
			Reset();
			if (!effType)
				PrepareAndSend(COMMV5_setEffect, { {2, {1, 0xfe}} });
			else
				PrepareAndSend(COMMV5_setEffect, { {2, {effType,tempo}}, { 9, {(byte)(nc - 1),
					act1.r,act1.g,act1.b,act2.r,act2.g,act2.b}}});
			return UpdateColors();
		}
		return false;
	}

	BYTE Functions::GetDeviceStatus() {

		byte buffer[MAX_BUFFERSIZE];
		if (transport) {
			memset(buffer, 0, sizeof(buffer));
			buffer[0] = reportIDList[version];
			switch (version) {
			case API_V5:
			{
				PrepareAndSend(COMMV5_status);
				buffer[0] = reportIDList[version];
				if (HidGetFeature(buffer, length))
					return buffer[2];
			} break;
			case API_V4:
			{
				if (HidGetInputReport(buffer, length)) {
					return buffer[2];
				}
			} break;
			case API_V3: case API_V2:
			{
				PrepareAndSend(COMMV1_status);
				buffer[0] = reportIDList[version];
				if (HidGetInputReport(buffer, length))
					return buffer[0];
			} break;
			}
		}
		return 0;
	}

	BYTE Functions::WaitForReady() {
		int i = 0;
		switch (version) {
		case API_V3: case API_V2:
			for (; i < 100 && GetDeviceStatus() != ALIENFX_V2_READY; i++)
				AfxSleepMs(10);
			return i < 100;
		case API_V4:
			while (!IsDeviceReady()) AfxSleepMs(20);
			return 1;
		default:
			return GetDeviceStatus();
		}
	}

	BYTE Functions::WaitForBusy() {
		int i = 0;
		switch (version) {
		case API_V3: case API_V2:
			if (GetDeviceStatus())
				for (; i < 100 && GetDeviceStatus() != ALIENFX_V2_BUSY; i++)
					AfxSleepMs(10);
			return i < 100;
		case API_V4: {
			if (pid == 0x551) // patch for newer v4
				return true;
			else {
				for (; i < 500 && GetDeviceStatus() != ALIENFX_V4_BUSY; i++) AfxSleepMs(20);
				return i < 500;
			}
		} break;
		default:
			return GetDeviceStatus();
		}
	}

	BYTE Functions::IsDeviceReady() {
		int status = GetDeviceStatus();
		switch (version) {
		case API_V5:
			return status != ALIENFX_V5_WAITUPDATE;
		case API_V4:
			return status ? status != ALIENFX_V4_BUSY : 0xff;
		case API_V3: case API_V2:
			return status == ALIENFX_V2_READY ? 1 : Reset();
		default:
			return !inSet;
		}
	}

	Functions::~Functions() {
		AlienFXClose();
	}

	Mappings::~Mappings() {
		for (auto i = fxdevs.begin(); i < fxdevs.end(); i++)
			if (i->dev) delete i->dev;
	}

	void Mappings::AlienFxUpdateDevice(Functions* dev) {
		auto devInfo = GetDeviceById(dev->pid, dev->vid);
		if (devInfo) {
			devInfo->present = true;
			activeLights += (unsigned)devInfo->lights.size();
			if (devInfo->dev) {
				DebugPrint("Scan: VID: " + to_string(devInfo->vid) + ", PID: " + to_string(devInfo->pid) + ", Version: "
					+ to_string(dev->version) + " - present already\n");
				delete dev;
			}
			else {
				devInfo->dev = dev;
				deviceListChanged = devInfo->arrived = true;
				DebugPrint("Scan: VID: " + to_string(devInfo->vid) + ", PID: " + to_string(devInfo->pid) + ", Version: "
					+ to_string(dev->version) + " - return back\n");
			}
		}
		else {
			fxdevs.push_back({ {{dev->pid, dev->vid}}, dev, dev->description });
			deviceListChanged = fxdevs.back().arrived = fxdevs.back().present = true;
			DebugPrint("Scan: VID: " + to_string(dev->vid) + ", PID: " + to_string(dev->pid) + ", Version: "
				+ to_string(dev->version) + " - new device added\n");
		}
		activeDevices++;
	}

	bool Mappings::AlienFXEnumDevices() {
		deviceListChanged = false;

		// Reset active status
		for (auto i = fxdevs.begin(); i != fxdevs.end(); i++)
			i->present = i->arrived = false;
		activeDevices = activeLights = 0;

		for (const HidDeviceInfo& info : HidEnumerate()) {
			Functions* dev = new Functions();
			if (dev->AlienFXProbeDevice(info))
				AlienFxUpdateDevice(dev);
			else
				delete dev;
		}

		// Check removed devices. Unlike upstream, drop the stale control object
		// so a re-plugged device gets probed (and opened) again.
		for (auto i = fxdevs.begin(); i != fxdevs.end(); i++)
			if (!i->present && i->dev) {
				deviceListChanged = true;
				DebugPrint("Scan: VID: " + to_string(i->vid) + ", PID: " + to_string(i->pid) + " - removed\n");
				delete i->dev;
				i->dev = NULL;
			}
		return deviceListChanged;
	}

	Afx_device* Mappings::GetDeviceById(WORD pid, WORD vid) {
		for (auto pos = fxdevs.begin(); pos < fxdevs.end(); pos++)
			if (pos->pid == pid && (!vid || pos->vid == vid)) {
				return &(*pos);
			}
		return nullptr;
	}

	Afx_device* Mappings::GetDeviceById(DWORD devID) {
		for (auto pos = fxdevs.begin(); pos < fxdevs.end(); pos++)
			if (pos->devID == devID) {
				return &(*pos);
			}
		return nullptr;
	}

	Afx_device* Mappings::AddDeviceById(DWORD devID)
	{
		Afx_device* dev = GetDeviceById(devID);
		if (!dev) {
			fxdevs.push_back({ {{(WORD)(devID & 0xffff), (WORD)(devID >> 16)}}, NULL });
			dev = &fxdevs.back();
		}
		return dev;
	}

	Afx_light *Mappings::GetMappingByDev(Afx_device* dev, WORD LightID) {
		if (dev) {
			for (auto pos = dev->lights.begin(); pos != dev->lights.end(); pos++)
				if (pos->lightid == LightID)
					return &(*pos);
		}
		return nullptr;
	}

	vector<Afx_group> *Mappings::GetGroups() {
		return &groups;
	}

	bool Mappings::SetDeviceBrightness(Afx_device* dev, BYTE br, bool power)
	{
		if (dev->dev) {
			return dev->dev->SetBrightness(br, dev->brightness, &dev->lights, power);
		}
		return false;
	}

	void Mappings::RemoveMapping(Afx_device* dev, WORD lightID)
	{
		if (dev) {
			for (auto del_map = dev->lights.begin(); del_map != dev->lights.end(); del_map++)
				if (del_map->lightid == lightID) {
					dev->lights.erase(del_map);
					return;
				}
		}
	}

	Afx_group *Mappings::GetGroupById(DWORD gID) {
		for (auto pos = groups.begin(); pos != groups.end(); pos++)
			if (pos->gid == gID)
				return &(*pos);
		return nullptr;
	}

	int Mappings::GetFlags(Afx_device* dev, WORD lightid) {
		Afx_light* lgh = GetMappingByDev(dev, lightid);
		return lgh ? lgh->flags : 0;
	}

	bool Functions::IsHaveGlobal()
	{
		return version == API_V5 || version == API_V8;
	}

	// --- Alienware tri-mode wireless (AW920K) -------------------------------
	// All reports are unnumbered (report ID 0); the literal 0x0e is the first
	// data byte. On Windows the report length includes the phantom ID byte, so
	// buffer[0] = 0 and the on-wire data starts at buffer[1].
	// See docs/aw920k-protocol.md.

	// Feature "commit" report (0e 05 01 51) that AWCC sends before every
	// output report; appears to latch/begin a lighting transaction.
	void Functions::AWCommit() {
		byte commit[MAX_BUFFERSIZE] = { 0 };
		commit[1] = 0x0e; commit[2] = 0x05; commit[3] = 0x01; commit[4] = 0x51;
		transport->SetFeature(commit, length);
	}

	// The "base layer" output report AWCC always sends before a color: mode
	// 0x09 defines the on-key-press highlight and puts the keyboard into
	// software-controlled mode (the static color is ignored without it).
	// AWCC hardcoded cyan here; we use the chosen color so pressed keys
	// match the rest of the board.
	void Functions::AWBaseLayer(byte r, byte g, byte b) {
		byte base[MAX_BUFFERSIZE] = { 0 };
		byte cmd[] = { 0x0e,0x05,0x01,0x09, r, g, b, 0x00,0x00,0x00,0x06,0x0a,0x01,0x02,0x01 };
		memcpy(base + 1, cmd, sizeof(cmd));
		transport->SetOutputReport(base, length);
	}

	// The keyboard layers two lighting planes: the permanent color (command
	// 0x01) and the pressed-key highlight (command 0x09). Always On uses the
	// same color on both; On While Typing paints the permanent plane black so
	// only pressed keys light up.
	bool Functions::AWApply(byte pr, byte pg, byte pb, byte kr, byte kg, byte kb, byte bright) {
		if (!transport) return false;
		byte color[MAX_BUFFERSIZE] = { 0 };
		color[1] = 0x0e; color[2] = 0x05; color[3] = 0x01; color[4] = 0x01; // permanent plane
		color[5] = pr; color[6] = pg; color[7] = pb;
		color[12] = bright > 0x0a ? 0x0a : bright;
		color[13] = color[14] = color[15] = 0x01;
		// Replicate AWCC's sequence AND pacing: the keyboard is wireless (the
		// receiver forwards over 2.4 GHz) and drops back-to-back commands.
		// AWCC leaves ~16 ms between commit and payload and ~640 ms between
		// payloads; we use conservative values in the same shape.
		AWCommit();
		AfxSleepMs(20);
		AWBaseLayer(kr, kg, kb);
		AfxSleepMs(300);
		AWCommit();
		AfxSleepMs(20);
		bool ok = transport->SetOutputReport(color, length);
		AfxSleepMs(300);
		return ok;
	}

	bool Functions::AWStaticColor(byte r, byte g, byte b, byte bright) {
		return AWApply(r, g, b, r, g, b, bright);
	}

	bool Functions::AWKeyPressColor(byte r, byte g, byte b, byte bright) {
		return AWApply(0, 0, 0, r, g, b, bright);
	}

	bool Functions::AWEffect(byte effectId, byte r, byte g, byte b, byte bright) {
		if (!transport) return false;
		if (effectId <= 0x01)
			return AWStaticColor(r, g, b, bright);
		// Permanent-plane packet layout (bytes after the 0x0e prefix):
		// 05 01 [id] R G B R2 G2 B2 [tempo] [bright] p1 p2 p3
		// Captured spectrum: id 08, tempo 0x19, params 01 02 03. Static uses
		// params 01 01 01 and tempo 0. Other IDs are extrapolated.
		byte eff[MAX_BUFFERSIZE] = { 0 };
		eff[1] = 0x0e; eff[2] = 0x05; eff[3] = 0x01; eff[4] = effectId;
		eff[5] = r; eff[6] = g; eff[7] = b;
		// eff[8..10] = second color (black)
		eff[11] = 0x19; // tempo, as captured for spectrum
		eff[12] = bright > 0x0a ? 0x0a : bright;
		eff[13] = 0x01; eff[14] = 0x01; eff[15] = 0x01;
		if (effectId == 0x08) { // spectrum: exact captured parameters
			eff[5] = 0xff; eff[6] = 0; eff[7] = 0;
			eff[14] = 0x02; eff[15] = 0x03;
		}
		AWCommit();
		AfxSleepMs(20);
		AWBaseLayer(r, g, b);
		AfxSleepMs(300);
		AWCommit();
		AfxSleepMs(20);
		bool ok = transport->SetOutputReport(eff, length);
		AfxSleepMs(300);
		return ok;
	}

	bool Functions::AWGlobalBrightness(byte level) {
		if (!transport) return false;
		byte buf[MAX_BUFFERSIZE] = { 0 };
		buf[1] = 0x0e; buf[2] = 0x17; buf[3] = level > 0x0a ? 0x0a : level;
		bool ok = transport->SetFeature(buf, length);
		AfxSleepMs(100); // wireless pacing (see AWStaticColor)
		return ok;
	}

	bool Functions::AWSpectrum(byte bright) {
		if (!transport) return false;
		byte eff[MAX_BUFFERSIZE] = { 0 };
		eff[1] = 0x0e; eff[2] = 0x05; eff[3] = 0x01; eff[4] = 0x08; // spectrum mode
		eff[5] = 0xff; eff[11] = 0x19; // speed / tempo, as captured
		eff[12] = bright > 0x0a ? 0x0a : bright;
		eff[13] = 0x01; eff[14] = 0x02; eff[15] = 0x03;
		AWCommit();
		AfxSleepMs(20);
		bool ok = transport->SetOutputReport(eff, length);
		AfxSleepMs(300);
		return ok;
	}
}
