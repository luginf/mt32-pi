//
// d110_bus.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// D-110 emulation core added 2026, ported from the D-110 VST Emulator project
// (https://github.com/luginf/d110-vst-emulator) by Alan <luginfo10@gmail.com>.
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include "synth/d110/d110_bus.h"

#include <algorithm>
#include <cstring>

void D110Bus::setFirmwareRom(const uint8_t *data, size_t size) {
	std::fill(firmware_.begin(), firmware_.end(), u8(0));
	std::memcpy(firmware_.data(), data, std::min(size, firmware_.size()));
}

void D110Bus::setPresetsRom(const uint8_t *data, size_t size) {
	std::fill(presets_.begin(), presets_.end(), u8(0));
	std::memcpy(presets_.data(), data, std::min(size, presets_.size()));
}

D110Bus::u8 D110Bus::bankedRead(u32 physical) {
	if (physical < 0x10000) return firmware_[physical];
	if (physical >= 0x40000 && physical < 0x48000) return rams[physical - 0x40000];
	if (physical >= 0x80000 && physical < 0xa0000) return presets_[physical - 0x80000];
	if (physical >= 0xc0000 && physical < 0xc8000) {
		if (!cardInserted) return kCardAbsentByte;
		const u32 offset = physical - 0xc0000;
		if (offset == u32(kCardStatusOffset)) return cardStatusByte();
		return memcs[offset];
	}
	return 0; // open bus
}

void D110Bus::bankedWrite(u32 physical, u8 data) {
	if (physical >= 0x40000 && physical < 0x48000) { rams[physical - 0x40000] = data; return; }
	if (physical >= 0xc0000 && physical < 0xc8000) { memcs[physical - 0xc0000] = data; return; }
	// firmware/presets ROM: writes dropped, matching real ROM regions.
}

D110Bus::u8 D110Bus::read(u16 addr) {
	if (addr >= 0x1000 && addr < 0x8000)
		return firmware_[(addr - 0x1000) + 0x1000]; // == firmware_[addr], kept explicit to mirror the driver's own offset arithmetic
	if (addr >= 0x8000 && addr < 0xc000)
		return bankedRead(u32(bankReg_) * 0x4000 + (addr - 0x8000));
	if (addr >= 0xc000)
		return rams[addr - 0xc000]; // fixed window, low 16KB of rams only
	if (addr == 0x021a || addr == 0x021b) return sc0;
	if (addr == 0x021c || addr == 0x021d) return sc1;
	if (addr == 0x0380) return lcd.control_r() >> 7; // real hardware; this model's busy bit is always 0
	if (addr == 0x0c00) {
		if (!la32Pending) return 0;
		la32Pending = false;
		return la32Status;
	}
	if (addr == 0x0c01) return 0;
	// SO latch (0x0200/0x0280) is write-only on the real hardware too. Everything else in
	// this low range is unmapped (open bus, reads 0).
	return 0;
}

void D110Bus::write(u16 addr, u8 data) {
	if (addr == 0x0100) { bankReg_ = data; return; }
	if (addr >= 0x8000 && addr < 0xc000) { bankedWrite(u32(bankReg_) * 0x4000 + (addr - 0x8000), data); return; }
	if (addr >= 0xc000) {
		rams[addr - 0xc000] = data;
		if (addr >= kVoiceCtxTapBase && addr < kVoiceCtxTapBase + kVoiceCtxTapSpan && onVoiceCtxWrite)
			onVoiceCtxWrite(u16(addr - 0xc000), data);
		return;
	}
	if (addr == 0x021a || addr == 0x021b) { swScan_ = data; return; }
	if (addr >= kLa32RegBase && addr <= kLa32RegEnd) { if (onLa32RegWrite) onLa32RegWrite(addr, data); return; }
	if (addr == 0x0300) {
		if (lcdDataBufferPos_ != int(sizeof(lcdDataBuffer_))) lcdDataBuffer_[lcdDataBufferPos_++] = data;
		return;
	}
	if (addr == 0x0380) {
		// roland_d10_state::lcd_ctrl_w(): control byte lands FIRST, then every data byte
		// buffered since the last control write flushes through afterwards - not how the
		// real MSM6222B's separate control/data pins work, but how this driver wires them.
		lcd.control_w(data);
		for (int i = 0; i < lcdDataBufferPos_; ++i) lcd.data_w(lcdDataBuffer_[i]);
		lcdDataBufferPos_ = 0;
		return;
	}
	// SO latch (0x0200/0x0280): stubbed - LED/reverb-program bits, no read-back exists on
	// real hardware either, so there is nothing this port needs to model yet.
}
