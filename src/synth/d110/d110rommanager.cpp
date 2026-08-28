//
// d110rommanager.cpp
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

#include "synth/d110/d110rommanager.h"

#include <circle/logger.h>
#include <circle/string.h>
#include <fatfs/ff.h>

#include <cstddef>
#include <cstring>

LOGMODULE("d110rommanager");

namespace
{
	const char* const Disks[] = { "SD", "USB" };
	const char ROMDirectory[] = "roms/d110";

	constexpr size_t FirmwareSize = 0x8000;  // 32768
	constexpr size_t PresetsSize  = 0x20000; // 131072
	constexpr size_t CGROMSize    = 0x1000;  // 4096
	constexpr size_t WaveChipSize = 0x80000; // 524288

	bool EqualsIgnoreCase(const char* pA, const char* pB)
	{
		while (*pA && *pB)
		{
			char a = *pA, b = *pB;
			if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
			if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
			if (a != b) return false;
			++pA;
			++pB;
		}
		return *pA == *pB;
	}

	// True if pPath's filename (after the last '/') matches pName, case-insensitively.
	bool FileNameIs(const char* pPath, const char* pName)
	{
		const char* pSlash = strrchr(pPath, '/');
		return EqualsIgnoreCase(pSlash ? pSlash + 1 : pPath, pName);
	}
}

CD110ROMManager::CD110ROMManager()
	: m_pFirmwareData(nullptr), m_nFirmwareSize(0),
	  m_pPresetsData(nullptr), m_nPresetsSize(0),
	  m_pCGROMData(nullptr), m_nCGROMSize(0),
	  m_pControlImageData(nullptr), m_pControlFile(nullptr), m_pControlROMImage(nullptr),
	  m_pPCMImageData(nullptr), m_pPCMFile(nullptr), m_pPCMROMImage(nullptr)
{
}

CD110ROMManager::~CD110ROMManager()
{
	delete[] m_pFirmwareData;
	delete[] m_pPresetsData;
	delete[] m_pCGROMData;

	if (m_pControlROMImage)
		MT32Emu::ROMImage::freeROMImage(m_pControlROMImage);
	delete m_pControlFile;
	delete[] m_pControlImageData;

	if (m_pPCMROMImage)
		MT32Emu::ROMImage::freeROMImage(m_pPCMROMImage);
	delete m_pPCMFile;
	delete[] m_pPCMImageData;
}

u8* CD110ROMManager::LoadWholeFile(const char* pPath, size_t& nOutSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return nullptr;

	const FSIZE_t nSize = f_size(&File);
	u8* pData = new u8[nSize];

	UINT nRead;
	const FRESULT Result = f_read(&File, pData, nSize, &nRead);
	f_close(&File);

	if (Result != FR_OK || nRead != nSize)
	{
		delete[] pData;
		return nullptr;
	}

	nOutSize = nSize;
	return pData;
}

size_t CD110ROMManager::FindFiles(TFoundFile* pOutFiles, size_t nMaxFiles)
{
	size_t nCount = 0;

	for (auto pDisk : Disks)
	{
		CString DirectoryPath;
		DirectoryPath.Format("%s:/%s", pDisk, ROMDirectory);

		DIR Dir;
		FILINFO FileInfo;
		FRESULT Result = f_findfirst(&Dir, &FileInfo, DirectoryPath, "*");

		while (Result == FR_OK && *FileInfo.fname && nCount < nMaxFiles)
		{
			if (!(FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS)))
			{
				CString FilePath(static_cast<const char*>(DirectoryPath));
				FilePath.Append("/");
				FilePath.Append(FileInfo.fname);

				strncpy(pOutFiles[nCount].Path, static_cast<const char*>(FilePath), sizeof(pOutFiles[nCount].Path) - 1);
				pOutFiles[nCount].Path[sizeof(pOutFiles[nCount].Path) - 1] = '\0';
				pOutFiles[nCount].nSize = FileInfo.fsize;
				++nCount;
			}

			Result = f_findnext(&Dir, &FileInfo);
		}
	}

	return nCount;
}

// The three control-board files are matched by their documented exact chip filenames (see
// d110rommanager.h) - there is no content-based way to identify msm6222b-01.bin in particular.
bool CD110ROMManager::LoadControlBoardFiles(const TFoundFile* pFiles, size_t nFileCount)
{
	for (size_t i = 0; i < nFileCount; ++i)
	{
		if (!m_pFirmwareData && pFiles[i].nSize == FirmwareSize && FileNameIs(pFiles[i].Path, "d-110.v1.10.ic19.bin"))
			m_pFirmwareData = LoadWholeFile(pFiles[i].Path, m_nFirmwareSize);
		else if (!m_pPresetsData && pFiles[i].nSize == PresetsSize && FileNameIs(pFiles[i].Path, "r15179873-lh5310-97.ic12.bin"))
			m_pPresetsData = LoadWholeFile(pFiles[i].Path, m_nPresetsSize);
		else if (!m_pCGROMData && pFiles[i].nSize == CGROMSize && FileNameIs(pFiles[i].Path, "msm6222b-01.bin"))
			m_pCGROMData = LoadWholeFile(pFiles[i].Path, m_nCGROMSize);
	}

	return m_pFirmwareData && m_pPresetsData && m_pCGROMData;
}

// Firmware+presets concatenated is the exact byte layout vanilla munt's own ROM database
// already recognises as "ctrl_d110_1_10_1"/"_2" (see d110rommanager.h) - no local munt patch
// needed.
void CD110ROMManager::BuildControlROMImage()
{
	const size_t nSize = m_nFirmwareSize + m_nPresetsSize;
	m_pControlImageData = new u8[nSize];
	memcpy(m_pControlImageData, m_pFirmwareData, m_nFirmwareSize);
	memcpy(m_pControlImageData + m_nFirmwareSize, m_pPresetsData, m_nPresetsSize);

	m_pControlFile = new MT32Emu::ArrayFile(m_pControlImageData, nSize);
	const MT32Emu::ROMImage* pImage = MT32Emu::ROMImage::makeROMImage(m_pControlFile);
	const MT32Emu::ROMInfo* pInfo = pImage->getROMInfo();

	if (pInfo && pInfo->type == MT32Emu::ROMInfo::Type::Control)
		m_pControlROMImage = pImage;
	else
	{
		LOGERR("Firmware+presets did not match a known D-110 control ROM image");
		MT32Emu::ROMImage::freeROMImage(pImage);
	}
}

// The two 524288-byte wave chips are matched by content, not name (real-world dumps circulate
// under varied filenames) - every pairing/order found in the folder is tried against mt32emu's
// own ROM database until one is recognised as "pcm_d110".
bool CD110ROMManager::BuildPCMROMImage(const TFoundFile* pFiles, size_t nFileCount)
{
	static constexpr size_t MaxWaveFiles = 8;
	const TFoundFile* pWaveFiles[MaxWaveFiles];
	size_t nWaveCount = 0;

	for (size_t i = 0; i < nFileCount && nWaveCount < MaxWaveFiles; ++i)
		if (pFiles[i].nSize == WaveChipSize)
			pWaveFiles[nWaveCount++] = &pFiles[i];

	for (size_t i = 0; i < nWaveCount && !m_pPCMROMImage; ++i)
	{
		for (size_t j = 0; j < nWaveCount && !m_pPCMROMImage; ++j)
		{
			if (i == j)
				continue;

			size_t nSizeA, nSizeB;
			u8* pA = LoadWholeFile(pWaveFiles[i]->Path, nSizeA);
			if (!pA)
				continue;

			u8* pB = LoadWholeFile(pWaveFiles[j]->Path, nSizeB);
			if (!pB)
			{
				delete[] pA;
				continue;
			}

			u8* pJoined = new u8[nSizeA + nSizeB];
			memcpy(pJoined, pA, nSizeA);
			memcpy(pJoined + nSizeA, pB, nSizeB);
			delete[] pA;
			delete[] pB;

			MT32Emu::File* pFile = new MT32Emu::ArrayFile(pJoined, nSizeA + nSizeB);
			const MT32Emu::ROMImage* pImage = MT32Emu::ROMImage::makeROMImage(pFile);
			const MT32Emu::ROMInfo* pInfo = pImage->getROMInfo();

			if (pInfo && pInfo->type == MT32Emu::ROMInfo::Type::PCM)
			{
				m_pPCMImageData = pJoined;
				m_pPCMFile = pFile;
				m_pPCMROMImage = pImage;
			}
			else
			{
				MT32Emu::ROMImage::freeROMImage(pImage);
				delete pFile;
				delete[] pJoined;
			}
		}
	}

	return m_pPCMROMImage != nullptr;
}

bool CD110ROMManager::ScanROMs()
{
	if (HaveROMs())
		return true;

	static constexpr size_t MaxFiles = 32;
	TFoundFile Files[MaxFiles];
	const size_t nFileCount = FindFiles(Files, MaxFiles);

	if (!LoadControlBoardFiles(Files, nFileCount))
	{
		LOGERR("Missing one or more of d-110.v1.10.ic19.bin, r15179873-lh5310-97.ic12.bin, msm6222b-01.bin in roms/d110");
		return false;
	}

	BuildControlROMImage();
	if (!m_pControlROMImage)
		return false;

	if (!BuildPCMROMImage(Files, nFileCount))
	{
		LOGERR("Could not find a recognised D-110 PCM wave ROM pairing (e.g. r15179878.ic7.bin + r15179880.ic8.bin) in roms/d110");
		return false;
	}

	return true;
}

bool CD110ROMManager::HaveROMs() const
{
	return m_pFirmwareData && m_pPresetsData && m_pCGROMData && m_pControlROMImage && m_pPCMROMImage;
}
