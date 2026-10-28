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

#include "Gfx.h"

typedef std::map<std::string, std::string> KeyValues;

KeyValues ReadKeyValues(const std::string& path);
bool WriteKeyValues(const std::string& path, const KeyValues& kv);

// Home screen: recent games, game browser (titles and icons from the ROM banners), general settings (ui.txt), settings
// of one game (<rom>.ui), help. Returns the ROM to start, or "" to quit.
std::string RunHome(Gfx* g, const std::string& configDir, const std::string& startDir, const std::string& version);

#endif
