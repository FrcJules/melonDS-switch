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
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>

#include "Controls.h"

static const char* DsKeyIds[DsKeyCount] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};

ControlProfile::ControlProfile()
{
    // standard: the same places as on a DS (PLUS = Start, MINUS = Select)
    const int def[DsKeyCount] = {SwA, SwB, SwMinus, SwPlus, SwRight, SwLeft, SwUp, SwDown, SwR, SwL, SwX, SwY};
    for (int k = 0; k < DsKeyCount; k++) Button[k] = def[k];
}

unsigned ControlProfile::MaskFor(int swButton) const
{
    unsigned m = 0;
    for (int k = 0; k < DsKeyCount; k++)
        if (Button[k] == swButton) m |= 1u << k;
    return m;
}

void ControlProfile::Read(const std::map<std::string, std::string>& kv)
{
    auto get = [&](const char* key) -> const std::string* { auto it = kv.find(key); return it == kv.end() ? nullptr : &it->second; };
    if (auto v = get("name")) Name = *v;
    for (int k = 0; k < DsKeyCount; k++)
        if (auto v = get(DsKeyIds[k])) Button[k] = std::clamp(atoi(v->c_str()), -1, SwButtonCount - 1);
    if (auto v = get("leftstick")) LeftStickDpad = *v == "dpad";
    if (auto v = get("rightstick"))
    {
        static const char* modes[] = {"none", "cursor", "stylus", "aim"};
        for (int m = 0; m < RightModeCount; m++) if (*v == modes[m]) RightStick = m;
    }
    if (auto v = get("sensitivity")) Sensitivity = std::clamp(atoi(v->c_str()), 1, 10);
    if (auto v = get("inverty")) InvertY = *v == "1";
}

std::string ControlProfile::Write() const
{
    static const char* modes[] = {"none", "cursor", "stylus", "aim"};
    std::string s = "name=" + Name + "\n";
    for (int k = 0; k < DsKeyCount; k++) s += std::string(DsKeyIds[k]) + "=" + std::to_string(Button[k]) + "\n";
    s += std::string("leftstick=") + (LeftStickDpad ? "dpad" : "none") + "\n";
    s += std::string("rightstick=") + modes[std::clamp(RightStick, 0, RightModeCount - 1)] + "\n";
    s += "sensitivity=" + std::to_string(Sensitivity) + "\n";
    s += std::string("inverty=") + (InvertY ? "1" : "0") + "\n";
    return s;
}

std::vector<ControlProfile> BuiltinProfiles()
{
    std::vector<ControlProfile> v;
    ControlProfile std_;
    std_.Id = "standard";
    std_.Name = "Standard";
    v.push_back(std_);

    // Metroid Prime Hunters: move with the left stick (D-pad), aim with the right stick (stylus dragged like a mouse),
    // shoot with R (the DS L button, under the left index on a DS for right-handed players), DS R on L
    ControlProfile aim;
    aim.Id = "aim";
    aim.Name = "Visée (Metroid)";
    aim.RightStick = RightAim;
    aim.Button[DsL] = SwR;
    aim.Button[DsR] = SwL;
    aim.Sensitivity = 5;
    v.push_back(aim);

    // Star Fox Command: the ship follows the stylus: the right stick is the stylus, pressed as soon as it moves
    ControlProfile sty;
    sty.Id = "stylus";
    sty.Name = "Stylet (Star Fox)";
    sty.RightStick = RightStylus;
    v.push_back(sty);
    return v;
}

bool IsBuiltinProfile(const std::string& id)
{
    return id == "standard" || id == "aim" || id == "stylus";
}

std::string AutoProfileFor(const std::string& title)
{
    std::string t = title;
    for (auto& c : t) c = (char)tolower((unsigned char)c);
    if (t.find("metroid prime hunters") != std::string::npos) return "aim";
    if (t.find("star fox command") != std::string::npos || t.find("starfox command") != std::string::npos) return "stylus";
    return "";
}

const char* DsKeyName(int k)
{
    static const char* n[DsKeyCount] = {"A", "B", "Select", "Start", "Droite", "Gauche", "Haut", "Bas", "R", "L", "X", "Y"};
    return k >= 0 && k < DsKeyCount ? n[k] : "?";
}

const char* SwButtonName(int b)
{
    static const char* n[SwButtonCount] = {"", "", "", "", " (clic)", " (clic)", "", "",
                                           "", "", "", "", "", "", "", ""};
    return b >= 0 && b < SwButtonCount ? n[b] : "aucun";
}

const char* RightStickName(int m)
{
    static const char* n[RightModeCount] = {"rien", "curseur (" "" " clic pour toucher)", "stylet direct", "visée (glisser)"};
    return n[std::clamp(m, 0, RightModeCount - 1)];
}

void StickTouch::Reset()
{
    AimDown = false;
    AimLift = 0;
    MarkerFrames = 0;
}

StickTouch::Out StickTouch::Update(int mode, float sx, float sy, bool r3Held, int sensitivity, bool invertY)
{
    Out o;
    const float dead = 0.18f;
    float mag = std::sqrt(sx * sx + sy * sy);
    float speedScale = sensitivity / 5.f;
    if (mode == RightCursor)
    {
        // dead zone, then a speed that grows with the square of the deflection (3.5 DS pixels per frame at 5)
        auto speed = [&](float v) { float f = (std::fabs(v) - dead) / (1.f - dead); return f <= 0 ? 0.f : (v < 0 ? -3.5f : 3.5f) * f * f * speedScale; };
        float dx = speed(sx), dy = speed(-sy);
        if (dx != 0 || dy != 0)
        {
            CursorX = std::clamp(CursorX + dx, 0.f, 255.f);
            CursorY = std::clamp(CursorY + dy, 0.f, 191.f);
            MarkerFrames = 180;
        }
        o.Touch = r3Held;
        if (r3Held) MarkerFrames = 180;
        else if (MarkerFrames > 0) MarkerFrames--;
        o.X = (int)CursorX; o.Y = (int)CursorY;
        o.ShowMarker = MarkerFrames > 0;
        return o;
    }
    if (mode == RightStylus)
    {
        // the stick position is the stylus position (full deflection = edge of the screen), down while the stick is out
        // of the dead zone
        if (mag > dead)
        {
            const float k = 1.f / 0.92f;   // 92 % of the deflection already reaches the edge
            CursorX = std::clamp(128.f + sx * k * 128.f, 0.f, 255.f);
            CursorY = std::clamp(96.f - sy * k * 96.f, 0.f, 191.f);
            o.Touch = true;
            o.ShowMarker = true;
        }
        o.X = (int)CursorX; o.Y = (int)CursorY;
        return o;
    }
    if (mode == RightAim)
    {
        // the stylus is dragged like a mouse from the centre; near an edge it is lifted for 2 frames and put back at
        // the centre, so that turning never stops
        if (mag <= dead)
        {
            AimDown = false;
            AimLift = 0;
            return o;
        }
        if (AimLift > 0)
        {
            AimLift--;
            return o;
        }
        if (!AimDown)
        {
            AimDown = true;
            CursorX = 128; CursorY = 96;
        }
        else
        {
            float f = (mag - dead) / (1.f - dead);
            float v = 6.f * f * f * speedScale / mag;   // DS pixels per frame at full deflection, sensitivity 5
            CursorX += sx * v;
            CursorY += (invertY ? sy : -sy) * v;
            if (CursorX < 8 || CursorX > 247 || CursorY < 8 || CursorY > 183)
            {
                AimDown = false;
                AimLift = 2;
                return o;
            }
        }
        o.Touch = true;
        o.X = (int)CursorX; o.Y = (int)CursorY;
        return o;
    }
    return o;
}
