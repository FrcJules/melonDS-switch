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

// Drawing layer of the Switch frontend: one GLES 3 context (SDL window + EGL) shared by the GPU renderer and the UI.

#include <functional>
#include <string>

#include "Gfx.h"

static const char* GfxVS = R"(#version 300 es
precision highp float;
uniform vec3 uRow0;   // pixel position = (dot(uRow0, (p, 1)), dot(uRow1, (p, 1))), p = corner of the unit square
uniform vec3 uRow1;
uniform vec4 uUV;
out vec2 vUV;
void main()
{
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUV = mix(uUV.xy, uUV.zw, p);
    vec2 px = vec2(dot(uRow0, vec3(p, 1.0)), dot(uRow1, vec3(p, 1.0)));
    gl_Position = vec4(px.x / 640.0 - 1.0, 1.0 - px.y / 360.0, 0.0, 1.0);
}
)";

static const char* GfxFS = R"(#version 300 es
precision highp float;
precision highp sampler2D;
uniform vec4 uColor;
uniform int uMode;
uniform sampler2D uTex;
in vec2 vUV;
out vec4 oColor;
void main()
{
    if (uMode == 0) oColor = uColor;                              // flat colour
    else if (uMode == 1) oColor = vec4(texture(uTex, vUV).bgr, 1.0); // DS software frame (ARGB in memory = BGRA bytes)
    else oColor = texture(uTex, vUV);                              // text (RGBA)
}
)";

static const char* GfxFSArray = R"(#version 300 es
precision highp float;
precision highp sampler2DArray;
uniform sampler2DArray uTex;
uniform float uLayer;
in vec2 vUV;
out vec4 oColor;
void main()
{
    oColor = vec4(texture(uTex, vec3(vUV, uLayer)).rgb, 1.0);
}
)";

static GLuint GfxCompile(GLenum type, const char* src, const std::function<void(const std::string&)>& say)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        say(std::string("shader error: ") + log);
        return 0;
    }
    return sh;
}

static GLuint GfxLink(const char* vs, const char* fs, const std::function<void(const std::string&)>& say)
{
    GLuint v = GfxCompile(GL_VERTEX_SHADER, vs, say), f = GfxCompile(GL_FRAGMENT_SHADER, fs, say);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { say("program link error"); return 0; }
    return p;
}

bool GfxInit(Gfx* g, const std::function<void(const std::string&)>& say)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    g->Window = SDL_CreateWindow("melonDS", 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
    if (!g->Window) { say(std::string("window: ") + SDL_GetError()); return false; }
    g->Context = SDL_GL_CreateContext(g->Window);
    if (!g->Context) { say(std::string("GL context: ") + SDL_GetError()); return false; }
    SDL_GL_SetSwapInterval(1);
    say(std::string("GL: ") + (const char*)glGetString(GL_VERSION) + " / " + (const char*)glGetString(GL_RENDERER));

    g->Prog = GfxLink(GfxVS, GfxFS, say);
    g->ProgArray = GfxLink(GfxVS, GfxFSArray, say);
    if (!g->Prog || !g->ProgArray) return false;
    const char* names[7] = {"uRow0", "uUV", "uColor", "uMode", "uTex", "uLayer", "uRow1"};
    GLuint progs[2] = {g->Prog, g->ProgArray};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 7; k++)
            g->ULoc[i][k] = glGetUniformLocation(progs[i], names[k]);
    glGenVertexArrays(1, &g->Vao);
    glBindVertexArray(g->Vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return true;
}

void GfxResetState(Gfx* g)
{
    // the GPU renderer leaves its own state behind
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 1280, 720);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(g->Vao);
    if (g->Blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);
}

void GfxClear(Gfx* g)
{
    GfxResetState(g);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void GfxColor(Gfx* g, int r, int gr, int b, int a) { g->R = r / 255.f; g->G = gr / 255.f; g->B = b / 255.f; g->A = a / 255.f; }

void GfxBlendMode(Gfx* g, bool on)
{
    g->Blend = on;
    if (on) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);
}

// position uniforms of an axis-aligned rectangle (program 0 = Prog, 1 = ProgArray)
static void GfxSetRect(Gfx* g, int prog, const SDL_Rect* r)
{
    glUniform3f(g->ULoc[prog][0], (float)r->w, 0.f, (float)r->x);
    glUniform3f(g->ULoc[prog][6], 0.f, (float)r->h, (float)r->y);
}

// position uniforms of a DS screen (256x192) drawn with a ScreenLayout transform
// (x' = m[0] x + m[2] y + m[4], y' = m[1] x + m[3] y + m[5])
static void GfxSetMtx(Gfx* g, int prog, const float* m)
{
    glUniform3f(g->ULoc[prog][0], m[0] * 256.f, m[2] * 192.f, m[4]);
    glUniform3f(g->ULoc[prog][6], m[1] * 256.f, m[3] * 192.f, m[5]);
}

void GfxFill(Gfx* g, const SDL_Rect* r)
{
    glUseProgram(g->Prog);
    GfxSetRect(g, 0, r);
    glUniform4f(g->ULoc[0][1], 0, 0, 1, 1);
    glUniform4f(g->ULoc[0][2], g->R, g->G, g->B, g->A);
    glUniform1i(g->ULoc[0][3], 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// mode 1 = DS software frame, 2 = RGBA texture (text); src in texels (null = whole texture)
void GfxCopy(Gfx* g, const GfxTex* t, const SDL_Rect* src, const SDL_Rect* dst, int mode)
{
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src) { u0 = (float)src->x / t->W; v0 = (float)src->y / t->H; u1 = (float)(src->x + src->w) / t->W; v1 = (float)(src->y + src->h) / t->H; }
    glUseProgram(g->Prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glUniform1i(g->ULoc[0][4], 0);
    GfxSetRect(g, 0, dst);
    glUniform4f(g->ULoc[0][1], u0, v0, u1, v1);
    glUniform1i(g->ULoc[0][3], mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// one screen of the software frame texture (256x384, top screen above the bottom one), placed with a ScreenLayout transform
void GfxCopyScreen(Gfx* g, const GfxTex* t, int screen, const float* mtx)
{
    glUseProgram(g->Prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glUniform1i(g->ULoc[0][4], 0);
    GfxSetMtx(g, 0, mtx);
    glUniform4f(g->ULoc[0][1], 0, screen * 0.5f, 1, screen * 0.5f + 0.5f);
    glUniform1i(g->ULoc[0][3], 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// one layer (= one screen) of the GPU renderer's output texture array, placed with a ScreenLayout transform
void GfxCopyArray(Gfx* g, GLuint tex, int layer, const float* mtx, bool linear)
{
    glUseProgram(g->ProgArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glUniform1i(g->ULoc[1][4], 0);
    glUniform1f(g->ULoc[1][5], (float)layer);
    GfxSetMtx(g, 1, mtx);
    glUniform4f(g->ULoc[1][1], 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GfxTexParams(const GfxTex* t, bool linear)
{
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

GfxTex GfxTexCreate(int w, int h, bool linear)
{
    GfxTex t;
    t.W = w; t.H = h;
    glGenTextures(1, &t.Id);
    glBindTexture(GL_TEXTURE_2D, t.Id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GfxTexParams(&t, linear);
    return t;
}

void GfxTexDestroy(GfxTex* t)
{
    if (t->Id) glDeleteTextures(1, &t->Id);
    *t = GfxTex();
}

// RGBA texture from an SDL surface (text)
GfxTex GfxTexFromSurface(SDL_Surface* surf)
{
    GfxTex t;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);   // bytes R,G,B,A
    if (!rgba) return t;
    t.W = rgba->w; t.H = rgba->h;
    glGenTextures(1, &t.Id);
    glBindTexture(GL_TEXTURE_2D, t.Id);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.W, t.H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GfxTexParams(&t, true);
    SDL_FreeSurface(rgba);
    return t;
}

void GfxPresent(Gfx* g) { SDL_GL_SwapWindow(g->Window); }

// ---- text with the console's shared font, ROM browser ----
TTF_Font* UIFont = nullptr;
const SDL_Color White {240, 240, 240, 255}, Grey {150, 160, 175, 255}, Accent {120, 210, 255, 255};

void DrawText(Gfx* r, const std::string& text, int x, int y, SDL_Color c)
{
    if (!UIFont || text.empty()) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(UIFont, text.c_str(), c);
    if (!surf) return;
    GfxTex t = GfxTexFromSurface(surf);
    SDL_Rect d {x, y, surf->w, surf->h};
    bool wasBlend = r->Blend;
    GfxBlendMode(r, true);
    if (t.Id) GfxCopy(r, &t, nullptr, &d, 2);
    GfxBlendMode(r, wasBlend);
    GfxTexDestroy(&t);
    SDL_FreeSurface(surf);
}
