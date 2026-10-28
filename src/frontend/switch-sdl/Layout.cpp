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

#include <algorithm>
#include <cmath>

#include "Layout.h"

void DisplayOptions::Read(const std::map<std::string, std::string>& kv)
{
    auto get = [&](const char* k, int& v, int lo, int hi) {
        auto it = kv.find(k);
        if (it != kv.end()) v = std::clamp(atoi(it->second.c_str()), lo, hi);
    };
    auto getb = [&](const char* k, bool& v) {
        auto it = kv.find(k);
        if (it != kv.end()) v = it->second == "1";
    };
    get("layout", Layout, 0, 2);
    get("sizing", Sizing, 0, 4);
    getb("swap", Swap);
    get("rotation", Rotation, 0, 3);
    if (Rotation == 2) Rotation = 0;   // upside down: not offered
    getb("intscale", IntegerScale);
    get("gap", Gap, 0, 96);
    getb("autotop", AutoTop);
}

std::string DisplayOptions::Write() const
{
    char buf[160];
    snprintf(buf, sizeof(buf), "layout=%d\nsizing=%d\nswap=%d\nrotation=%d\nintscale=%d\ngap=%d\nautotop=%d\n",
             Layout, Sizing, Swap ? 1 : 0, Rotation, IntegerScale ? 1 : 0, Gap, AutoTop ? 1 : 0);
    return buf;
}

const char* LayoutName(int layout)
{
    static const char* names[] = {"verticale", "horizontale", "hybride"};
    return names[std::clamp(layout, 0, 2)];
}

const char* SizingName(int sizing)
{
    static const char* names[] = {"égale", "haut plus grand", "bas plus grand", "haut seul", "bas seul"};
    return names[std::clamp(sizing, 0, 4)];
}

const char* RotationName(int rotation)
{
    return rotation == 1 ? "livre (90°)" : rotation == 3 ? "livre (270°)" : "normale";
}

void SwitchLayout::Setup(const DisplayOptions& o, bool topOnly, int width, int height)
{
    static const ScreenLayoutType types[] = {screenLayout_Natural, screenLayout_Horizontal, screenLayout_Hybrid};
    static const ScreenSizing sizings[] = {screenSizing_Even, screenSizing_EmphTop, screenSizing_EmphBot, screenSizing_TopOnly, screenSizing_BotOnly};
    ScreenLayoutType type = types[std::clamp(o.Layout, 0, 2)];
    ScreenSizing sizing = sizings[std::clamp(o.Sizing, 0, 4)];
    if (topOnly)
    {
        // the big screen alone: same rotation and scaling, no hybrid
        type = screenLayout_Natural;
        sizing = screenSizing_TopOnly;
    }
    float mtx[6 * kMaxScreenTransforms];
    int kind[kMaxScreenTransforms];
    for (bool integer : {o.IntegerScale, false})
    {
        Layout.Setup(width, height, type, (ScreenRotation)(o.Rotation & 3), sizing, o.Gap, integer,
                     type == screenLayout_Hybrid ? o.Swap : (o.Swap && !topOnly), 1.f, 1.f);
        NumQuads = Layout.GetScreenTransforms(mtx, kind);
        // integer scale where even 1x does not fit (hybrid in book mode: scale 0): fitted scale instead
        bool degenerate = false;
        for (int i = 0; i < NumQuads; i++)
            if (std::abs(mtx[6 * i]) + std::abs(mtx[6 * i + 2]) < 0.25f) degenerate = true;
        if (!degenerate) break;
    }
    for (int i = 0; i < NumQuads; i++)
    {
        Quads[i].Screen = kind[i];
        std::copy(mtx + 6 * i, mtx + 6 * i + 6, Quads[i].Mtx);
    }
}

bool SwitchLayout::Touch(int px, int py, int& x, int& y)
{
    x = px; y = py;
    return Layout.GetTouchCoords(x, y, false);
}

int SwitchLayout::BottomToScreen(float x, float y, float* outX, float* outY, float* outScale) const
{
    int n = 0;
    for (int i = 0; i < NumQuads; i++)
    {
        if (Quads[i].Screen != 1) continue;
        const float* m = Quads[i].Mtx;
        outX[n] = m[0] * x + m[2] * y + m[4];
        outY[n] = m[1] * x + m[3] * y + m[5];
        outScale[n] = std::max(std::abs(m[0]) + std::abs(m[2]), 0.01f);
        n++;
    }
    return n;
}

bool SoftScreenIsBlack(const unsigned int* px)
{
    // 8 pixels at a time; a lit pixel usually shows up in the first lines
    for (int i = 0; i < 256 * 192; i += 8)
    {
        unsigned int acc = px[i] | px[i + 1] | px[i + 2] | px[i + 3] | px[i + 4] | px[i + 5] | px[i + 6] | px[i + 7];
        if (acc & 0x00F8F8F8) return false;   // channels under 8/255 count as black (end of a fade)
    }
    return true;
}
