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

#ifndef SWITCH_GFX_H
#define SWITCH_GFX_H

#include <functional>
#include <string>

#include <SDL.h>
#include <SDL_ttf.h>
#include <GLES3/gl3.h>

// ---- drawing: everything goes through one GLES 3 context (SDL window + EGL), so that the GPU renderer and the UI share it ----
// coordinates are pixels of the 1280x720 screen
struct GfxTex { GLuint Id = 0; int W = 0, H = 0; };

struct Gfx
{
    SDL_Window* Window = nullptr;
    SDL_GLContext Context = nullptr;
    GLuint Prog = 0, ProgArray = 0, Vao = 0;
    GLint ULoc[2][7] {};          // uRow0 uUV uColor uMode uTex uLayer uRow1, per program
    float R = 0, G = 0, B = 0, A = 1;
    bool Blend = false;
    bool Linear = false;
};

bool GfxInit(Gfx* g, const std::function<void(const std::string&)>& say);
void GfxResetState(Gfx* g);
void GfxClear(Gfx* g);
void GfxColor(Gfx* g, int r, int gr, int b, int a);
void GfxBlendMode(Gfx* g, bool on);
void GfxFill(Gfx* g, const SDL_Rect* r);
// mode 1 = DS software frame, 2 = RGBA texture (text); src in texels (null = whole texture)
void GfxCopy(Gfx* g, const GfxTex* t, const SDL_Rect* src, const SDL_Rect* dst, int mode);
// DS screens placed with a ScreenLayout transform (6 floats): software frame texture (screen 0 = top, 1 = bottom) or
// a layer of the GPU renderer's output texture array
void GfxCopyScreen(Gfx* g, const GfxTex* t, int screen, const float* mtx);
void GfxCopyArray(Gfx* g, GLuint tex, int layer, const float* mtx, bool linear);
void GfxTexParams(const GfxTex* t, bool linear);
GfxTex GfxTexCreate(int w, int h, bool linear);
void GfxTexDestroy(GfxTex* t);
GfxTex GfxTexFromSurface(SDL_Surface* surf);
void GfxPresent(Gfx* g);

// text with the console's shared font
extern TTF_Font* UIFont;
extern const SDL_Color White, Grey, Accent;
void DrawText(Gfx* r, const std::string& text, int x, int y, SDL_Color c);

#endif
