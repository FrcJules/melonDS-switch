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

#ifndef GFXDEKO_H
#define GFXDEKO_H

// deko3d presentation for the Switch frontend: screen-space quads (flat colour or textured) into a 1280x720
// swapchain. Same model as the GLES 3 path of main.cpp (Gfx*), without Mesa: no GL context may exist in the process.

#include <functional>
#include <string>

bool DkGfxInit(const std::function<void(const std::string&)>& say);
void DkGfxShutdown();
// the DkDevice (shared with the 3D renderer), null before DkGfxInit
void* DkGfxDevice();
// a GPU job never finished: at exit, leave the device alone (the system reclaims it with the process)
void DkGfxMarkBroken();

// textures: RGBA8, id >= 0 on success, -1 when the pool is full
int DkTexCreate(int w, int h, bool linear);
void DkTexSetLinear(int id, bool linear);
// pixels: 4 bytes per texel, pitch in bytes
void DkTexUpload(int id, int x, int y, int w, int h, const void* pixels, int pitch);
// freed once the GPU has finished the frames that may still use it
void DkTexDestroy(int id);

void DkClear();
// rect/uv/color: 4 floats each; mode 0 flat colour, 1 DS software frame, 2 RGBA texture; tex = -1 for none
void DkDraw(const float rect[4], const float uv[4], const float color[4], int mode, int tex, bool blend);
void DkPresent();

#endif
