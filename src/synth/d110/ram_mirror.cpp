//
// ram_mirror.cpp
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

// See ram_mirror.h - table and logic ported verbatim from the D-110 VST Emulator project's
// own D110Core.cpp (kMirrorRegions/osdSnapshotRam/emitRegionSysex).
#include "synth/d110/ram_mirror.h"

#include <algorithm>
#include <cstring>

// Tone Temporary Area, one region per part - see D110Core.cpp's own comment for how the base
// 0x21E4 was measured (tone_probe.cpp, exact 246-byte match against the engine's own tones).
#define D110_TONE(part) \
	{ uint16_t(0x21E4 + (part) * 246), \
	  0x040000u | (uint32_t(((part) * 246) / 128) << 8) | uint32_t(((part) * 246) % 128), \
	  246, "Tone Temporary", true }

// Rhythm Setup Area, split into 128/128/84-byte chunks because one DT1 can't carry the full
// 340 bytes without truncation - see D110Core.cpp's comment for the chunk arithmetic.
#define D110_RHYTHM(chunk, len) \
	{ uint16_t(0x2090 + (chunk) * 128), \
	  0x030110u + (uint32_t(chunk) << 8), \
	  (len), "Rhythm Setup" }

const RamMirror::Region RamMirror::kRegions[] = {
	{ 0x2000, 0x030000, 9 * 16, "Timbre Temporary" },

	D110_RHYTHM(0, 128), D110_RHYTHM(1, 128), D110_RHYTHM(2, 84),

	D110_TONE(0), D110_TONE(1), D110_TONE(2), D110_TONE(3),
	D110_TONE(4), D110_TONE(5), D110_TONE(6), D110_TONE(7),

	// System Area: master tune, then reverb type/time/level. masterVol is still excluded -
	// the D-110's volume is a physical knob, so the firmware never fills that byte in, and
	// mirroring it zeroed the engine's master volume (see docs/sysex_address_map.md).
	//
	// masterTune was excluded for the same reason reverb type used to be (see D110Core.cpp's
	// history) - "no honest mapping" - but that reasoning doesn't actually hold for this
	// byte: mt32emu's own MASTER TUNE field (Structures.h) uses the IDENTICAL 0-127 scale and
	// the IDENTICAL default (0x4A, "confirmed on CM-64" per its own source) as the D-110's -
	// the only mismatch on record is between the D-110's own LCD label ("442") and what
	// Roland's documented byte-to-Hz FORMULA predicts for that byte (~447) - a display-label
	// quirk, not a functional scale disagreement between the two machines. Forwarding the
	// raw byte, unchanged, exactly like reverb type already does, is the correct fix - see
	// 2026-08-07's investigation notes.
	{ 0x2D94, 0x100000, 4, "System (master tune+reverb type+time+level)" },
	{ 0x2D98, 0x100004, 18, "System (reserve + channels)" },
};
#undef D110_TONE
#undef D110_RHYTHM

const int RamMirror::kNumRegions = int(sizeof(RamMirror::kRegions) / sizeof(RamMirror::kRegions[0]));

RamMirror::RamMirror() {
	prev_.resize(size_t(kNumRegions));
	for (int i = 0; i < kNumRegions; ++i)
		prev_[size_t(i)].assign(kRegions[i].length, 0);
}

void RamMirror::update(const uint8_t *ram, double elapsedSeconds) {
	if (bootResyncPending_ && elapsedSeconds >= kBootSettleSeconds) {
		bootResyncPending_ = false;
		resyncPending_ = true;
	}
	const bool resync = resyncPending_;
	resyncPending_ = false;

	bool timbreTempSent = false;
	for (int i = 0; i < kNumRegions; ++i) {
		const Region &region = kRegions[i];
		std::vector<uint8_t> &prev = prev_[size_t(i)];
		const uint8_t *now = ram + region.ramOffset;
		// toneReassert is unconditionally on in the shipping plugin (D110Core.cpp's own
		// comment: no user-facing switch) - hardcoded true here to match.
		const bool mustReassert = timbreTempSent && region.reassertAfterTimbreTemp;
		if (!resync && !mustReassert && primed_ && std::memcmp(prev.data(), now, region.length) == 0)
			continue;
		std::memcpy(prev.data(), now, region.length);
		if (primed_ || resync) {
			emitRegionSysex(region, ram);
			if (i == 0) timbreTempSent = true; // region 0 is Timbre Temporary
		}
	}
	primed_ = true;
}

void RamMirror::emitRegionSysex(const Region &region, const uint8_t *ramImage) {
	uint8_t msg[kMaxSysexBytes];
	int n = 0;
	msg[n++] = 0xF0;
	msg[n++] = 0x41; // Roland
	msg[n++] = 0x10; // device ID
	msg[n++] = 0x16; // model: MT-32 family, which is what the D-110 answers to
	msg[n++] = 0x12; // DT1

	const uint32_t addr = region.sysexAddress;
	const uint8_t a1 = uint8_t((addr >> 16) & 0x7f);
	const uint8_t a2 = uint8_t((addr >> 8) & 0x7f);
	const uint8_t a3 = uint8_t(addr & 0x7f);
	msg[n++] = a1;
	msg[n++] = a2;
	msg[n++] = a3;

	uint32_t sum = a1 + a2 + a3;
	const int payload = std::min<int>(region.length, kMaxSysexBytes - n - 2);
	for (int i = 0; i < payload; ++i) {
		const uint8_t v = ramImage[region.ramOffset + i] & 0x7f;
		msg[n++] = v;
		sum += v;
	}
	msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
	msg[n++] = 0xF7;

	queue_.emplace_back(msg, msg + n);
	++messagesEmitted_;
}

int RamMirror::popSysex(uint8_t *out) {
	if (queue_.empty()) return 0;
	const auto &msg = queue_.front();
	std::memcpy(out, msg.data(), msg.size());
	const int len = int(msg.size());
	queue_.pop_front();
	return len;
}
