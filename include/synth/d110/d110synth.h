//
// d110synth.h
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

#ifndef _synth_d110_d110synth_h
#define _synth_d110_d110synth_h

#include <circle/types.h>

#include <mt32emu/mt32emu.h>

#include "synth/d110/d110corenative.h"
#include "synth/d110/d110rommanager.h"
#include "synth/synthbase.h"

// Roland D-110 emulation: the real firmware runs on CD110CoreNative's emulated 8x9x CPU board
// (panel, LCD, patch/timbre/system memory, MIDI protocol - all the genuine device logic), while
// actual audio is produced by a real MT32Emu::Synth instance, exactly like CMT32Synth's. The
// two are kept in sync one-directionally: whenever the firmware's own memory changes (a panel
// button, an incoming SysEx dump), CD110CoreNative's RAM mirror hands us the equivalent Roland
// DT1 SysEx and we replay it into MT32Emu::Synth; whenever the firmware itself starts or
// releases a note (its own voice-context RAM, which already has the real key-range/part-
// assignment/voice-stealing logic applied), CD110CoreNative hands us the resolved
// part/key/velocity and we call MT32Emu::Synth::playMsgOnPart() directly - incoming MIDI is
// never forwarded to MT32Emu::Synth's own channel-to-part logic, since the D-110's channel/part
// mapping is a user-configurable SYSTEM setting only the real firmware actually applies.
class CD110Synth : public CSynthBase, public MT32Emu::ReportHandler
{
public:
	CD110Synth(unsigned nSampleRate, float nGain, float nReverbGain);
	virtual ~CD110Synth();

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override { return m_pSynth && m_pSynth->isActive(); }
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pBuffer, size_t nFrames) override;
	virtual size_t Render(float* pBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;

private:
	// Advances the emulated machine by nFrames worth of time, then drains everything it
	// produced (state-mirror SysEx, then resolved note events, in that order - see
	// d110corenative.h's own comment on why parameters must apply before the notes that
	// depend on them) into m_pSynth. Called from both Render() overloads.
	void Step(size_t nFrames);

	// MT32Emu::ReportHandler
	virtual bool onMIDIQueueOverflow() override;
	virtual void printDebug(const char* pFmt, va_list pList) override;
	virtual void showLCDMessage(const char* pMessage) override;
	virtual void onDeviceReset() override;

	CD110ROMManager m_ROMManager;
	D110CoreNative m_Core;
	MT32Emu::Synth* m_pSynth;

	float m_nGain;
	float m_nReverbGain;
	MT32Emu::SampleRateConverter* m_pSampleRateConverter;
};

#endif
