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

#ifndef SWITCH_LAYOUT_H
#define SWITCH_LAYOUT_H

#include <map>
#include <string>

#include "../ScreenLayout.h"

// Screen layouts of the Switch frontend, on top of the melonDS ScreenLayout (the one of the Qt frontend). Independent of
// the renderer and of the console: built and tested on a PC (tools/layout_test.cpp).

struct DisplayOptions
{
    int Layout = 0;          // 0 vertical (top screen above the bottom one), 1 horizontal (side by side),
                             // 2 hybrid (one big screen on the left, both screens small on the right)
    int Sizing = 0;          // 0 same size, 1 top screen bigger, 2 bottom screen bigger, 3 top screen only, 4 bottom screen only
    bool Swap = false;       // swap the two screens (hybrid: the bottom screen is the big one)
    int Rotation = 0;        // 0, 1 = 90 degrees, 3 = 270 degrees ("book": console held vertically)
    bool IntegerScale = false;
    int Gap = 0;             // pixels between the screens (DS pixels)
    bool AutoTop = true;     // show the top screen alone while the bottom screen is black

    // "key=value" settings (ui.txt, <rom>.ui); missing keys keep their value
    void Read(const std::map<std::string, std::string>& kv);
    std::string Write() const;
};

// names shown in the menu
const char* LayoutName(int layout);
const char* SizingName(int sizing);
const char* RotationName(int rotation);

struct ScreenQuad
{
    int Screen;              // 0 top, 1 bottom
    float Mtx[6];            // ScreenLayout transform: DS pixel (x, y) -> (m0 x + m2 y + m4, m1 x + m3 y + m5)
};

class SwitchLayout
{
public:
    // topOnly: the bottom screen is hidden (automatic big screen)
    void Setup(const DisplayOptions& o, bool topOnly, int width = 1280, int height = 720);

    int NumQuads = 0;
    ScreenQuad Quads[kMaxScreenTransforms];

    // touch: screen pixel -> bottom screen pixel; false if outside the bottom screen (or hidden)
    bool Touch(int px, int py, int& x, int& y);
    // where a bottom screen pixel is drawn, for each quad of the bottom screen (cursor); returns the count
    int BottomToScreen(float x, float y, float* outX, float* outY, float* outScale) const;

private:
    ScreenLayout Layout;
};

// Automatic big screen: says when to show the top screen alone. The bottom screen must be black for HoldFrames frames
// in a row (0.5 s) before it is hidden, and it comes back on the first frame it is not black (no flicker on short fades).
class BlackScreenHysteresis
{
public:
    static constexpr int HoldFrames = 30;
    // returns true if the state changed
    bool Update(bool bottomBlack)
    {
        bool before = Hidden;
        if (bottomBlack) { if (BlackFrames < HoldFrames) BlackFrames++; }
        else BlackFrames = 0;
        Hidden = BlackFrames >= HoldFrames;
        return Hidden != before;
    }
    void Reset() { BlackFrames = 0; Hidden = false; }
    bool Hidden = false;
private:
    int BlackFrames = 0;
};

// true if all 256x192 pixels of a software frame are black (BGRA words of the software renderer, alpha ignored, channels
// under 8/255 count as black). Stops at the first lit pixel.
bool SoftScreenIsBlack(const unsigned int* px);

#endif
