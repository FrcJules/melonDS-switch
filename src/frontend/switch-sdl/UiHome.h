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

#ifndef SWITCH_UIHOME_H
#define SWITCH_UIHOME_H

#include <map>
#include <string>
#include <vector>
#include <stdint.h>

#include "Gfx.h"

typedef std::map<std::string, std::string> KeyValues;

KeyValues ReadKeyValues(const std::string& path);
bool WriteKeyValues(const std::string& path, const KeyValues& kv);

// What the home screen chose: the ROM to start ("" = quit) and the state to load (-2 = as configured, i.e. the
// automatic state if there is one; -1 = none; 0..4 = that slot)
struct HomeChoice
{
    std::string Path;
    int LoadSlot = -2;
};

// Home screen: recent games, game library (every game of the library folders, grid or list, titles and icons from the
// ROM banners, favourites), file browser, game page (continue, states with screenshots, settings), general settings
// (ui.txt), help.
HomeChoice RunHome(Gfx* g, const std::string& configDir, const std::string& startDir, const std::string& version);

// screenshot saved next to a state (<state file>.thumb): "MDTH", u16 width, u16 height, RGBA pixels
bool WriteThumb(const std::string& path, const std::vector<unsigned char>& rgba, int w, int h);
bool ReadThumb(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h);

#endif
