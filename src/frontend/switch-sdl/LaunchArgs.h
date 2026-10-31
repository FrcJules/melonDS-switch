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

#ifndef SWITCH_LAUNCHARGS_H
#define SWITCH_LAUNCHARGS_H

#include <string.h>
#include <string>

// Command line given by the launcher (hbmenu, Sphaira file association: "<nro>" "<file>"). Header only, tested on a PC.
struct LaunchArgs
{
    bool Browse = false;     // --browse: go to the home screen even if a ROM is given
    std::string Rom;         // first argument that is not an option, as a libnx path (sdmc:/...); empty if none
};

// Launchers give SD paths without the device name ("/roms/DS/x.nds"); the rest of the frontend uses "sdmc:/...".
inline std::string NormalizeSdPath(const char* path)
{
    if (!path || !*path) return "";
    if (path[0] == '/') return std::string("sdmc:") + path;
    return path;
}

inline LaunchArgs ParseLaunchArgs(int argc, char** argv)
{
    LaunchArgs a;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--browse")) a.Browse = true;
        else if (strncmp(argv[i], "--", 2) != 0 && a.Rom.empty()) a.Rom = NormalizeSdPath(argv[i]);
    }
    return a;
}

#endif
