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

#ifndef SWITCH_UI_H
#define SWITCH_UI_H

#include <functional>
#include <string>
#include <vector>

#include "Gfx.h"

// Look and input of the menus (home screen, settings, in-game menu, messages): dark Switch-like style, text with the
// console's shared fonts (button icons from the Nintendo extended font), D-pad / stick / buttons and touch.
namespace Ui
{

// button icons of the shared "Nintendo extended" font
#define UI_A "\uE0E0"
#define UI_B "\uE0E1"
#define UI_X "\uE0E2"
#define UI_Y "\uE0E3"
#define UI_L "\uE0E4"
#define UI_R "\uE0E5"
#define UI_ZL "\uE0E6"
#define UI_ZR "\uE0E7"
#define UI_PLUS "\uE0EF"
#define UI_MINUS "\uE0F0"
#define UI_DPAD "\uE0EA"
#define UI_DPAD_UD "\uE0EB"
#define UI_DPAD_LR "\uE0EC"
#define UI_LSTICK "\uE0C1"
#define UI_RSTICK "\uE0C2"
#define UI_RSTICK_PRESS "\uE0C5"

extern const SDL_Color Text, TextDim, AccentText, Warn;
// background, header bar, panels, selection
void SetColorBg(Gfx* g);

enum FontSize { Small, Normal, Big };

bool Init();      // after TTF_Init and plInitialize
void Shutdown();
// clears the text cache (call once per screen change or when it grows)
void Trim();

// text, with button icons; maxWidth > 0: cut with "..." if wider. align: 0 left, 1 centre, 2 right (x = right edge)
int DrawText(Gfx* g, const std::string& text, int x, int y, SDL_Color c, FontSize size = Normal, int maxWidth = 0, int align = 0);
int TextWidth(const std::string& text, FontSize size = Normal);
// text broken into lines of at most width pixels; returns the height used
int DrawWrapped(Gfx* g, const std::string& text, int x, int y, int width, SDL_Color c, FontSize size = Normal, int maxLines = 0);

void Fill(Gfx* g, int x, int y, int w, int h, int r, int gr, int b, int a = 255);

// whole screen: background, header with a title, and the hints at the bottom right ("\uE0E0 Jouer" ...)
void DrawFrame(Gfx* g, const std::string& title, const std::vector<std::string>& hints);
void DrawHints(Gfx* g, const std::vector<std::string>& hints, int y = 666);

// ---- input ----
enum Button { BtnNone = -1, BtnA = 0, BtnB, BtnX, BtnY, BtnLStick, BtnRStick, BtnL, BtnR, BtnZL, BtnZR, BtnPlus, BtnMinus,
              BtnLeft, BtnUp, BtnRight, BtnDown };
struct Input
{
    int Button = BtnNone;    // pressed (D-pad and left stick repeat while held)
    bool Tap = false;        // touch released without moving much
    bool Touching = false;   // finger down
    int TouchX = 0, TouchY = 0;
    int DragY = 0;           // finger movement since the last event (scrolling)
};
// waits up to timeoutMs for an input; false if nothing happened (redraw anyway: animations)
bool Poll(Input& in, int timeoutMs = 50);

// ---- list of settings / actions ----
struct Item
{
    std::string Label;
    std::string Value;                                // shown on the right ("" = action)
    std::function<void(int dir, bool act)> Do;       // dir: -1/+1 (left/right), act: A or tap
    bool Header = false;                              // section title, not selectable
};
struct ListState { int Sel = 0, Top = 0; };
// draws the list in the rectangle; handles the input (D-pad, A, left/right, touch); returns true if the input was used
void DrawList(Gfx* g, const std::vector<Item>& items, ListState& st, int x, int y, int w, int rows, int rowH = 56);
bool ListInput(const std::vector<Item>& items, ListState& st, const Input& in, int x, int y, int w, int rows, int rowH = 56);

// full-screen message with an OK button (A, B, PLUS or a tap); used for errors
void Message(Gfx* g, const std::string& title, const std::string& body, const std::string& button = "OK");

// icon of a game (32x32 RGBA) drawn scaled; textures are cached by key
void DrawIcon(Gfx* g, const std::string& key, const uint32_t* rgba, int x, int y, int size);
void ForgetIcons();

}

#endif
