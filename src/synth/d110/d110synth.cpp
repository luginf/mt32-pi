//
// d110synth.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// D-110 emulation support added 2026 by Alan <luginfo10@gmail.com>.
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

#include <circle/logger.h>

#include <cstring>

#include "lcd/ui.h"
#include "synth/d110/d110synth.h"

LOGMODULE("d110synth");

namespace
{
	// The D-110's five ROM chips live directly under this folder on either disk (see
	// d110rommanager.h) - NVRAM (battery RAM + memory-card window) is persisted as two flat
	// files in the same folder, rather than a subfolder, since this core has no directory-
	// creation support of its own (see d110corenative.cpp).
	const char D110RomFolder[] = "SD:/roms/d110";

	// Roundtrips a packed CMIDIParser::OnShortMessage() word back into the raw MIDI bytes
	// D110CoreNative::pushMidi() needs to feed the firmware's own UART - see midiparser.cpp's
	// own OnShortMessage(), which builds nMessage as byte[0] | byte[1]<<8 | byte[2]<<16 (status
	// first, in the low byte). Returns the message length (2 or 3).
	size_t UnpackShortMessage(u32 nMessage, u8* pOutBytes)
	{
		const u8 nStatus = nMessage & 0xFF;
		pOutBytes[0] = nStatus;

		const u8 nStatusType = nStatus & 0xF0;
		const size_t nLength = (nStatusType == 0xC0 || nStatusType == 0xD0 || nStatus == 0xF1 || nStatus == 0xF3) ? 2 : 3;

		for (size_t i = 1; i < nLength; ++i)
			pOutBytes[i] = (nMessage >> (8 * i)) & 0xFF;

		return nLength;
	}
}

CD110Synth::CD110Synth(unsigned nSampleRate, float nGain, float nReverbGain)
	: CSynthBase(nSampleRate),
	  m_pSynth(nullptr),
	  m_nGain(nGain),
	  m_nReverbGain(nReverbGain),
	  m_pSampleRateConverter(nullptr)
{
}

CD110Synth::~CD110Synth()
{
	m_Core.stop();

	if (m_pSynth)
		delete m_pSynth;

	if (m_pSampleRateConverter)
		delete m_pSampleRateConverter;
}

bool CD110Synth::Initialize()
{
	if (!m_ROMManager.ScanROMs())
	{
		LOGERR("Could not find a complete set of D-110 ROMs in roms/d110");
		return false;
	}

	if (!m_Core.start(m_ROMManager.GetFirmware(), m_ROMManager.GetFirmwareSize(),
	                   m_ROMManager.GetPresets(), m_ROMManager.GetPresetsSize(),
	                   m_ROMManager.GetCGROM(), m_ROMManager.GetCGROMSize(),
	                   D110RomFolder))
	{
		LOGERR("%s", m_Core.lastStartError().c_str());
		return false;
	}

	// Real hardware's firmware waits on an EXTINT from the physical LA32 sound board to know
	// a dispatched note has actually reached a hardware voice slot - without this, its note
	// dispatch routine spins forever the first time it's needed and no further notes ever
	// complete (patch/channel changes still work: those don't go through this wait). La32Stub
	// is the D-110 VST Emulator project's own shipping default (PluginProcessor::setPoweredOn()).
	m_Core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	m_pSynth = new MT32Emu::Synth(this);

	if (!m_pSynth->open(*m_ROMManager.GetControlROMImage(), *m_ROMManager.GetPCMROMImage()))
		return false;

	m_pSynth->setOutputGain(m_nGain);
	m_pSynth->setReverbOutputGain(m_nReverbGain);

	m_pSampleRateConverter = new MT32Emu::SampleRateConverter(*m_pSynth, m_nSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

	// Let the firmware boot (same idea as a real D-110's power-on self-test) before the first
	// audio block is pulled, so the panel/LCD/patch memory are already settled.
	m_Core.runForSeconds(1.5);

	// No persisted NVRAM (first boot, or the SD card's roms/d110 folder never had one) means
	// battery RAM just came up zero-filled, not with real factory defaults - notably missing a
	// working channel-to-part routing table, so nothing responds to any MIDI channel at all
	// until the firmware's own real Write+Copy sequence runs and populates it from the presets
	// ROM (confirmed empirically: zero note dispatch on any of the 16 channels without this,
	// correct dispatch on all of them with it). Skipped when NVRAM was actually loaded, so a
	// returning user's saved patches aren't wiped on every boot.
	if (!m_Core.nvramWasLoaded())
		m_Core.factoryReset();

	return true;
}

void CD110Synth::HandleMIDIShortMessage(u32 nMessage)
{
	// m_Core's MIDI-in queue is a plain std::deque, not the lock-free single-producer/
	// single-consumer design MT32Emu::Synth's own MidiEventQueue uses internally (which is
	// why CMT32Synth::HandleMIDIShortMessage() doesn't need to lock here) - this method runs
	// on whichever thread parses incoming MIDI (see CMT32Pi::OnShortMessage()), concurrently
	// with Step() popping from the same queue on the audio thread, so it needs m_Lock too.
	u8 Bytes[3];
	const size_t nLength = UnpackShortMessage(nMessage, Bytes);
	m_Lock.Acquire();
	m_Core.pushMidi(Bytes, static_cast<int>(nLength));
	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CD110Synth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	// See HandleMIDIShortMessage()'s own comment on why this needs to lock.
	m_Lock.Acquire();
	m_Core.pushMidi(pData, static_cast<int>(nSize));
	m_Lock.Release();
}

void CD110Synth::AllSoundOff()
{
	// Hold pedal off + All Notes Off on every channel, sent both to the firmware (so its own
	// voice-context RAM ends up consistent - the ordinary popNoteEvent() path above then plays
	// the resulting note-offs through m_pSynth) and directly to m_pSynth (immediate stop,
	// belt-and-braces - ported from the D-110 VST Emulator project's own midiPanic()).
	u8 FirmwareBytes[16 * 2 * 3];
	size_t nFirmwareLength = 0;

	for (unsigned nChannel = 0; nChannel < 16; ++nChannel)
	{
		const u8 nStatus = 0xB0 | nChannel;
		for (u8 nController : { u8(64), u8(123) })
		{
			FirmwareBytes[nFirmwareLength++] = nStatus;
			FirmwareBytes[nFirmwareLength++] = nController;
			FirmwareBytes[nFirmwareLength++] = 0;

			if (m_pSynth)
				m_pSynth->playMsg(u32(nStatus) | (u32(nController) << 8));
		}
	}

	// See HandleMIDIShortMessage()'s own comment on why m_Core's queue needs locking; the
	// direct m_pSynth->playMsg() calls above don't (mt32emu's own queue is lock-free by
	// design, same reasoning CMT32Synth::AllSoundOff() relies on).
	m_Lock.Acquire();
	m_Core.pushMidi(FirmwareBytes, static_cast<int>(nFirmwareLength));
	m_Lock.Release();

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CD110Synth::SetMasterVolume(u8 nVolume)
{
	// The D-110's master volume is a physical knob the firmware never reads or mirrors (see
	// docs/sysex_address_map.md in the originating VST project: writing its SYSTEM-area byte
	// silently drops output by ~30dB and is otherwise ignored) - so unlike CMT32Synth, this
	// scales MT32Emu::Synth's own output gain directly, modelling that knob rather than a
	// firmware register.
	if (m_pSynth)
		m_pSynth->setOutputGain(m_nGain * (nVolume / 100.0f));
}

void CD110Synth::Step(size_t nFrames)
{
	m_Core.runForSeconds(double(nFrames) / double(m_nSampleRate));

	// Parameters before notes - see d110corenative.h's own comment: a note that depends on a
	// rhythm map/patch just loaded in the same tick must see it applied first.
	u8 Sysex[D110CoreNative::kMaxSysexBytes];
	while (const int nLength = m_Core.popSysex(Sysex))
		m_pSynth->playSysexNow(Sysex, MT32Emu::Bit32u(nLength));

	D110CoreNative::NoteEvent Event;
	while (m_Core.popNoteEvent(Event))
	{
		if (Event.part > 8)
			continue;

		// playMsgOnPart addresses the part directly (0-7 voice, 8 rhythm) with whatever
		// key/velocity the firmware's own note dispatch already resolved - no channel-to-part
		// mapping needs to be (re-)derived here, and none of the D-110's own key-range/voice-
		// stealing logic needs to be reimplemented either.
		m_pSynth->playMsgOnPart(Event.part, Event.on ? 0x9 : 0x8, Event.note, Event.velocity);
	}
}

size_t CD110Synth::Render(s16* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	Step(nFrames);
	if (m_pSampleRateConverter)
		m_pSampleRateConverter->getOutputSamples(pOutBuffer, nFrames);
	else
		m_pSynth->render(pOutBuffer, nFrames);
	m_Lock.Release();

	return nFrames;
}

size_t CD110Synth::Render(float* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	Step(nFrames);
	if (m_pSampleRateConverter)
		m_pSampleRateConverter->getOutputSamples(pOutBuffer, nFrames);
	else
		m_pSynth->render(pOutBuffer, nFrames);
	m_Lock.Release();

	return nFrames;
}

void CD110Synth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage("D-110 mode");
}

void CD110Synth::UpdateLCD(CLCD& LCD, unsigned int /*nTicks*/)
{
	static constexpr int kLines = D110CoreNative::kLines;
	static constexpr int kCols = D110CoreNative::kCols;

	// m_Core is mutated continuously by Render()/Step() on the audio core while this runs on
	// the UI core (see UITask() in mt32pi.cpp) - unlike CMT32Synth/CSoundFontSynth's own
	// UpdateLCD(), which only touch data their underlying engine already guarantees is safe
	// for lock-free cross-core reads, D110CoreNative makes no such guarantee for its own
	// state, so this needs the same lock Render() takes.
	u8 Text[kLines * kCols];
	m_Lock.Acquire();
	const bool bHaveLcd = m_Core.getLcdText(Text);
	m_Lock.Release();
	if (!bHaveLcd)
		return;

	// DDRAM codes >=16 are a direct CGROM index, which is ASCII-compatible for the printable
	// range on this (HD44780-style) controller - see Msm6222b::charAt()'s own comment - so
	// they print as-is. Codes <16 select one of the controller's 8 CGRAM custom characters and
	// have no ASCII equivalent; the only one the real firmware actually uses is code 1, the
	// "active part" solid block (see Display.cpp's D-110 LCD comment) - remapped to '\xFF'
	// exactly like CMT32Synth::UpdateLCD() remaps the analogous MT-32 indicator. Anything else
	// below 16 (blank cells, mostly) falls back to a space.
	for (u8& c : Text)
	{
		if (c >= 16)
			continue;
		c = (c == 1) ? '\xFF' : ' ';
	}

	char Line[kCols + 1];
	for (int nLine = 0; nLine < kLines; ++nLine)
	{
		memcpy(Line, &Text[nLine * kCols], kCols);
		Line[kCols] = '\0';
		LCD.Print(Line, 0, nLine, true, false);
	}
}

bool CD110Synth::onMIDIQueueOverflow()
{
	LOGERR("MIDI queue overflow");
	return false;
}

void CD110Synth::printDebug(const char* /*pFmt*/, va_list /*pList*/)
{
}

void CD110Synth::showLCDMessage(const char* pMessage)
{
	LOGNOTE("LCD: %s", pMessage);
}

void CD110Synth::onDeviceReset()
{
	LOGDBG("D-110 sound engine reset");
	m_MIDIMonitor.AllNotesOff();
	m_MIDIMonitor.ResetControllers(false);
}
