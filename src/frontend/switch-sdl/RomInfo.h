/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef SWITCH_ROMINFO_H
#define SWITCH_ROMINFO_H

#include <stdint.h>
#include <string>
#include <vector>

// Title and icon of a game, read from the banner of its ROM header (no emulator needed; tested on a PC).
struct RomInfo
{
    std::string Path;
    std::string FileName;
    std::string Title;       // first line of the banner title (file name without extension if there is no banner)
    std::string Subtitle;    // other lines (subtitle, publisher), joined with " - "
    std::string GameCode;    // 4 letters, e.g. "AMCP"
    bool HasIcon = false;
    uint32_t Icon[32 * 32];  // RGBA bytes in memory (R first), transparent where the palette index is 0
};

// language: 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish (falls back to English if empty)
bool ReadRomInfo(const std::string& path, RomInfo& info, int language = 2);

// banner bytes -> info (the part of ReadRomInfo that does not touch files)
void ParseBanner(const uint8_t* banner, size_t len, RomInfo& info, int language);

std::string Utf16ToUtf8(const uint16_t* s, size_t maxLen);

// "recent games" list (newest first): one path per line
std::vector<std::string> ReadRecent(const std::string& file);
void AddRecent(const std::string& file, const std::string& romPath, size_t maxCount = 30);

#endif
