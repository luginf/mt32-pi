//
// msm6222b.h
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

// Hand-ported from the vendored MAME tree's devices/video/msm6222b.cpp (the D-110's real LCD
// controller, MSM6222B-01 variant - HD44780-compatible with a fixed CGROM). Near-verbatim:
// the original has exactly two MAME touchpoints, both replaced here -
//   1. optional_region_ptr<uint8_t> m_cgrom -> a plain pointer set via setCgrom().
//   2. machine().time().as_ticks(250000) in blink_on() -> an explicit tick count the caller
//      advances (see setClockTicks()), since this class no longer has its own notion of
//      wall-clock time.
// Everything else - control_w/data_w/cursor_step/shift_step/render() - is pure state-machine
// logic over ddram[80]/cgram[64]/render_buf[80*16], unchanged.
#pragma once

#include <cstdint>
#include <cstring>

class Msm6222b {
public:
	using u8 = uint8_t;
	using u64 = uint64_t;

	Msm6222b() { reset(); }
	void reset();

	// cgrom must point at >= 16*128 bytes (0x1000 for the -01 variant) and outlive this
	// object - matches msm6222b-01.bin (SHA1 e108b520e6d20459a7bbd5958bbfa1d551a690bd),
	// already present beside the other ROMs in D-110 Data.
	void setCgrom(const u8 *cgrom) { cgrom_ = cgrom; }

	void control_w(u8 data);
	u8 control_r();
	void data_w(u8 data);

	// 250kHz tick count, used only for the cursor-blink phase - purely cosmetic, no effect
	// on displayed text. Advance it however is convenient (e.g. from a sample counter).
	void setClockTicks(u64 ticks) { clockTicks_ = ticks; }

	// Character n's 8 (or 11, double-height) rows live at bytes n*16..n*16+7(+10). Only the
	// low 5 bits of each row byte are used. One-line mode: n = 0..79. Two-line: first line
	// 0..39, second 40..79.
	const u8 *render();

	// The raw DDRAM byte at shift-adjusted render position i (same "(i + shift) % 80"
	// indexing render() uses internally) - the character CODE, not its rendered pixels.
	// Codes >=16 are a direct index into the loaded CGROM, which is ASCII-compatible for the
	// printable range on any HD44780-style controller (including this one - see msm6222b.h's
	// own provenance note), so a caller can treat those as plain text with no pixel round
	// trip. Codes <16 select one of the 8 CGRAM custom characters and have no ASCII
	// equivalent - the caller must substitute something printable for those itself.
	u8 charAt(int i) const { return ddram[(i + shift) % 80]; }

private:
	void cursor_step(bool direction);
	void shift_step(bool direction);
	bool blink_on() const;

	const u8 *cgrom_ = nullptr;
	u64 clockTicks_ = 0;

	u8 cgram[8 * 8] = {};
	u8 ddram[80] = {};
	u8 render_buf[80 * 16] = {};
	bool cursor_direction = false, cursor_blinking = false, two_line = false,
	     shift_on_write = false, double_height = false, cursor_on = false, display_on = false;
	u8 adc = 0, shift = 0;
};
