//
// ram_mirror.h
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

// The RAM-to-SysEx mirror bridge that feeds MT32Emu::Synth: watches the firmware's own RAM for
// patch/timbre/tone/system writes and turns changes into DT1 "Data set 1" SysEx messages, the
// same way the real D-110 hardware's internal engine mirrors its memory to its sound board.
//
// No ring buffer or atomics: CD110Synth steps the CPU and drains this mirror on the same
// thread (unlike the originating VST project, where a MAME thread produced and the audio
// thread consumed), so a plain deque is correct and simpler.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

class RamMirror {
public:
	struct Region {
		uint16_t ramOffset;
		uint32_t sysexAddress;
		uint16_t length;
		const char *name;
		bool reassertAfterTimbreTemp = false;
	};
	static const Region kRegions[];
	static const int kNumRegions;
	static constexpr int kMaxSysexBytes = 256;

	RamMirror();

	// Call periodically (any fixed cadence is fine, matching D110Core's own "once per
	// emulated video frame" - see D110CoreNative::runForSeconds()). elapsedSeconds is
	// cumulative emulated time since start(), used only for the boot-settle resync delay.
	void update(const uint8_t *ram, double elapsedSeconds);

	// Forces every region to resend on the next update() - matches D110Core::resyncMirror(),
	// needed after a factory reset or any external RAM rewrite the diff wouldn't otherwise see.
	void forceResync() { resyncPending_ = true; }

	// Drains one queued DT1 SysEx message (F0...F7) into out (must hold kMaxSysexBytes).
	// Returns its length, or 0 if the queue is empty.
	int popSysex(uint8_t *out);

	// Total messages built since construction - D110CoreNative derives sysexEmitted() (the
	// Monitor tab's diagnostic, D110Core::sysexEmitted()'s counterpart) from this.
	uint64_t messagesEmitted() const { return messagesEmitted_; }

private:
	std::vector<std::vector<uint8_t>> prev_;
	bool primed_ = false;
	bool bootResyncPending_ = true;
	bool resyncPending_ = false;
	static constexpr double kBootSettleSeconds = 4.0; // D110Core::kBootSettleMs / 1000
	std::deque<std::vector<uint8_t>> queue_;
	uint64_t messagesEmitted_ = 0;

	void emitRegionSysex(const Region &region, const uint8_t *ramImage);
};
