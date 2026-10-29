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

#ifndef SWITCH_CONTROLS_H
#define SWITCH_CONTROLS_H

#include <map>
#include <string>
#include <vector>

// Control profiles: which Switch button gives which DS button, and what the sticks do. Independent of the console
// (tested on a PC: tests/controls_test.cpp).

// DS keys, in the bit order of melonDS key masks
enum DsKey { DsA, DsB, DsSelect, DsStart, DsRight, DsLeft, DsUp, DsDown, DsR, DsL, DsX, DsY, DsKeyCount };
// Switch buttons, as numbered by SDL on the console
enum SwButton { SwA, SwB, SwX, SwY, SwLStick, SwRStick, SwL, SwR, SwZL, SwZR, SwPlus, SwMinus, SwLeft, SwUp, SwRight, SwDown, SwButtonCount };

enum RightStickMode { RightNone, RightCursor, RightStylus, RightAim, RightModeCount };

struct ControlProfile
{
    std::string Id;            // file name in controls/ (standard, aim, stylus, custom1...)
    std::string Name;          // shown in the menus
    int Button[DsKeyCount];    // Switch button for each DS key, -1 = none
    bool LeftStickDpad = true; // left stick works as the D-pad
    int RightStick = RightCursor;
    int Sensitivity = 5;       // 1..10: speed of the cursor / of the aim
    bool InvertY = false;      // aim: stick up moves the stylus down

    ControlProfile();
    // DS key mask (melonDS bits) given by one Switch button
    unsigned MaskFor(int swButton) const;
    void Read(const std::map<std::string, std::string>& kv);
    std::string Write() const;
};

std::vector<ControlProfile> BuiltinProfiles();     // standard, aim (Metroid Prime Hunters), stylus (Star Fox Command)
bool IsBuiltinProfile(const std::string& id);
// profile chosen automatically for a game (from the title of its banner), "" if none
std::string AutoProfileFor(const std::string& title);

const char* DsKeyName(int k);
const char* SwButtonName(int b);   // with the button icons of the system font
const char* RightStickName(int m);

// Right stick -> touch screen, called once per frame. sx, sy: stick position, -1..1, y positive = up.
class StickTouch
{
public:
    struct Out
    {
        bool Touch = false;      // the stylus is down at (X, Y) this frame
        int X = 128, Y = 96;
        bool ShowMarker = false; // draw the cursor / stylus marker
    };
    Out Update(int mode, float sx, float sy, bool r3Held, int sensitivity, bool invertY);
    void Reset();
    float CursorX = 128, CursorY = 96;
private:
    bool AimDown = false;
    int AimLift = 0;             // frames left with the stylus up while it goes back to the centre
    int MarkerFrames = 0;
};

#endif
