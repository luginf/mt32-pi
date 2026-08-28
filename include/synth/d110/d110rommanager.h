//
// d110rommanager.h
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

#ifndef _synth_d110_d110rommanager_h
#define _synth_d110_d110rommanager_h

#include <circle/types.h>
#include <mt32emu/mt32emu.h>

// Locates and loads the D-110's five ROM chip dumps from "<disk>:/roms/d110" (SD and USB, same
// two disks CROMManager scans for MT-32). Two different consumers need the same underlying
// bytes in different shapes:
//
//  - CD110CoreNative (the emulated 8x9x CPU board) needs the control firmware (IC19) and
//    presets ROM (IC12) as two separate raw buffers, plus the MSM6222B-01 LCD character
//    generator ROM - it executes the real firmware directly, so these have to be the literal
//    chip contents, not anything mt32emu-shaped.
//  - MT32Emu::Synth (the LA32 sound engine) needs a "Control" ROMImage (the same firmware+
//    presets bytes, concatenated - vanilla munt's own ROM database already recognises this
//    exact combination as "ctrl_d110_1_10_1"/"_2", no local patch required) and a "PCM"
//    ROMImage (the two wave chips IC7+IC8, concatenated in IC8-then-IC7 order, recognised as
//    "pcm_d110").
//
// Unlike CROMManager, the three control-board files (firmware/presets/cgrom) are matched by
// their documented exact chip filenames, not by content - the LCD character ROM in particular
// isn't part of any mt32emu-recognised image, so there is no content-based way to identify it
// (see docs/roms.md in the D-110 VST Emulator project this was ported from). The two PCM wave
// chips ARE matched by content (any two 524288-byte files in the folder, whichever pairing/
// order mt32emu's own database recognises), since real-world dumps circulate under varied
// filenames.
class CD110ROMManager
{
public:
	CD110ROMManager();
	~CD110ROMManager();

	// Scans both disks' "roms/d110" folder. Returns true once everything is present: the
	// three control-board files by name, and a wave-chip pairing mt32emu recognises as PCM.
	bool ScanROMs();

	bool HaveROMs() const;

	// Raw chip contents for CD110CoreNative::start(). Valid only after a successful ScanROMs().
	const u8* GetFirmware() const { return m_pFirmwareData; }
	size_t GetFirmwareSize() const { return m_nFirmwareSize; }
	const u8* GetPresets() const { return m_pPresetsData; }
	size_t GetPresetsSize() const { return m_nPresetsSize; }
	const u8* GetCGROM() const { return m_pCGROMData; }
	size_t GetCGROMSize() const { return m_nCGROMSize; }

	// ROMImages for MT32Emu::Synth::open(). Valid only after a successful ScanROMs(); owned by
	// this manager, destroyed with it.
	const MT32Emu::ROMImage* GetControlROMImage() const { return m_pControlROMImage; }
	const MT32Emu::ROMImage* GetPCMROMImage() const { return m_pPCMROMImage; }

private:
	struct TFoundFile
	{
		char Path[256];
		size_t nSize;
	};

	// Reads an entire file via FatFs into a freshly-allocated buffer (caller takes ownership).
	static u8* LoadWholeFile(const char* pPath, size_t& nOutSize);

	// Finds every regular file directly inside "<disk>:/roms/d110" across both disks.
	static size_t FindFiles(TFoundFile* pOutFiles, size_t nMaxFiles);

	bool LoadControlBoardFiles(const TFoundFile* pFiles, size_t nFileCount);
	bool BuildPCMROMImage(const TFoundFile* pFiles, size_t nFileCount);
	void BuildControlROMImage();

	u8* m_pFirmwareData;
	size_t m_nFirmwareSize;
	u8* m_pPresetsData;
	size_t m_nPresetsSize;
	u8* m_pCGROMData;
	size_t m_nCGROMSize;

	// Firmware+presets, concatenated - owned buffer backing m_pControlFile/m_pControlROMImage.
	u8* m_pControlImageData;
	MT32Emu::File* m_pControlFile;
	const MT32Emu::ROMImage* m_pControlROMImage;

	// Whichever wave-chip pairing/order mt32emu recognised - owned buffer backing
	// m_pPCMFile/m_pPCMROMImage.
	u8* m_pPCMImageData;
	MT32Emu::File* m_pPCMFile;
	const MT32Emu::ROMImage* m_pPCMROMImage;
};

#endif
