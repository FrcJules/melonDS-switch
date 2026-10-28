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

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "RomInfo.h"

std::string Utf16ToUtf8(const uint16_t* s, size_t maxLen)
{
    std::string out;
    for (size_t i = 0; i < maxLen && s[i]; i++)
    {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < maxLen && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
        {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void ParseBanner(const uint8_t* banner, size_t len, RomInfo& info, int language)
{
    // 0x20: 32x32 icon, 4x4 tiles of 8x8 pixels, 4 bits per pixel (low nibble first); 0x220: 16 colours (BGR555,
    // colour 0 transparent); 0x240 + 0x100 * language: title, UTF-16, lines separated by '\n'
    if (len < 0x840) return;
    const uint8_t* bmp = banner + 0x20;
    uint32_t pal[16];
    for (int i = 0; i < 16; i++)
    {
        uint16_t c = banner[0x220 + i * 2] | (banner[0x221 + i * 2] << 8);
        uint32_t r = (c & 0x1F) * 255 / 31, g = ((c >> 5) & 0x1F) * 255 / 31, b = ((c >> 10) & 0x1F) * 255 / 31;
        pal[i] = r | (g << 8) | (b << 16) | (i ? 0xFF000000u : 0);
    }
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 4; tx++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                {
                    uint8_t byte = bmp[(ty * 4 + tx) * 32 + y * 4 + x / 2];
                    int idx = (x & 1) ? byte >> 4 : byte & 0xF;
                    info.Icon[(ty * 8 + y) * 32 + tx * 8 + x] = pal[idx];
                }
    info.HasIcon = true;

    auto title = [&](int lang) {
        uint16_t buf[128];
        memcpy(buf, banner + 0x240 + 0x100 * lang, sizeof(buf));
        return Utf16ToUtf8(buf, 128);
    };
    std::string t = title(std::clamp(language, 0, 5));
    if (Trim(t).empty()) t = title(1);
    if (Trim(t).empty()) return;
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= t.size())
    {
        size_t nl = t.find('\n', p);
        if (nl == std::string::npos) nl = t.size();
        std::string l = Trim(t.substr(p, nl - p));
        if (!l.empty()) lines.push_back(l);
        p = nl + 1;
    }
    if (lines.empty()) return;
    // three lines = title, subtitle, publisher; two lines = title, publisher
    info.Title = lines[0];
    info.Subtitle.clear();
    for (size_t i = 1; i < lines.size(); i++)
        info.Subtitle += (i > 1 ? " - " : "") + lines[i];
}

bool ReadRomInfo(const std::string& path, RomInfo& info, int language)
{
    info.Path = path;
    size_t sl = path.find_last_of('/');
    info.FileName = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t dot = info.FileName.find_last_of('.');
    info.Title = dot == std::string::npos ? info.FileName : info.FileName.substr(0, dot);
    info.Subtitle.clear();
    info.GameCode.clear();
    info.HasIcon = false;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t header[0x70];
    bool ok = fread(header, 1, sizeof(header), f) == sizeof(header);
    if (ok)
    {
        for (int i = 0; i < 4; i++)
        {
            char c = (char)header[0x0C + i];
            if (c >= 0x20 && c < 0x7F) info.GameCode += c;
        }
        uint32_t bannerOffset = header[0x68] | (header[0x69] << 8) | (header[0x6A] << 16) | ((uint32_t)header[0x6B] << 24);
        if (bannerOffset >= 0x200)
        {
            std::vector<uint8_t> banner(0x840);
            if (fseek(f, (long)bannerOffset, SEEK_SET) == 0 && fread(banner.data(), 1, banner.size(), f) == banner.size())
                ParseBanner(banner.data(), banner.size(), info, language);
        }
    }
    fclose(f);
    return ok;
}

bool BuiltinSoftware(const std::string& gameCode)
{
    for (const char* c : {"ADA", "APA", "CPU", "IPK", "IPG"})   // Diamond, Pearl, Platinum, HeartGold, SoulSilver
        if (!strncmp(gameCode.c_str(), c, 3)) return true;
    return false;
}

std::vector<std::string> ReadRecent(const std::string& file)
{
    std::vector<std::string> out;
    FILE* f = fopen(file.c_str(), "r");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string s = Trim(line);
        while (!s.empty() && (s.back() == '\n')) s.pop_back();
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    fclose(f);
    return out;
}

void AddRecent(const std::string& file, const std::string& romPath, size_t maxCount)
{
    std::vector<std::string> list = ReadRecent(file);
    list.erase(std::remove(list.begin(), list.end(), romPath), list.end());
    list.insert(list.begin(), romPath);
    if (list.size() > maxCount) list.resize(maxCount);
    FILE* f = fopen(file.c_str(), "w");
    if (!f) return;
    for (const std::string& s : list) fprintf(f, "%s\n", s.c_str());
    fclose(f);
}
