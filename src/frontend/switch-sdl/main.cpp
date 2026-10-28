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

// Minimal Nintendo Switch frontend (SDL2, software renderer, FreeBIOS direct boot).
// Config: sdmc:/switch/melonDS-Switch/config.txt (also threaded3d=0|1), one "key=value" per line:
//   rom=<path of the .nds>   sav=<path of the save file>   jit=0|1   overclock=<MHz, 0 = off, auto>   gpuclock=<MHz, 0 = system default>
//   audiointerp=0..4 (none, linear, cosine, cubic, gaussian; default 3)   autosave=0|1   autoload=1|0 (default on)   loadslot=0..4 (state loaded at start)   jitblock=1..256 (default 32) jitliteral=0|1 jitbranch=0|1   itercycles=16..512 (main-loop step, default 128; melonDS uses 64)   audioctl=0|1   audioskew=<ratio>   audiodump=<seconds>   sampler=<seconds> (sampling profiler, samples.bin)
// Save states (next to the config, named after the ROM: <rom>.ss<slot>, slot 0 = automatic):
//   hold ZL + ZR, then X = save, Y = load, left/right = choose slot 1..4 (squares on the left show the slot, colour = result).
//   The state is also saved automatically on exit (slot 0) and loaded at start.
// Every second a line "t fps avg_ms max_ms" is appended to sdmc:/switch/melonDS-Switch/log.txt.
// Touch without the touch screen (docked): the right stick moves a cursor over the bottom screen, hold the right stick click to touch.
// The left stick works as the D-pad.
// Quit: hold L + R + PLUS + MINUS.

#include <stdio.h>
#include <lz4.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <GLES3/gl3.h>
#include <switch.h>

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#ifdef OGLRENDERER_ENABLED
#include "GPU_OpenGL.h"
#endif
#include "GPU_Soft.h"
#include "Platform.h"
#include "Prof.h"
#include "Savestate.h"
#ifdef SWITCH_DEKO3D
#include "deko3d/GfxDeko.h"
#endif

using namespace melonDS;

namespace melonDS { extern u64 SwitchJitTransitions; extern u64 SwitchJitTransitionNs; extern const char* SwitchCrashLogPath; extern u64 SwitchDbgWin9, SwitchDbgWin7, SwitchDbgCodeMem; extern u64 SwitchFaultCount, SwitchLastFaultAddr, SwitchLastFaultPC; }

extern std::string SwitchSavePath;
extern std::string SwitchLogPath;
extern std::mutex SwitchLogMutex;
namespace melonDS::Platform { extern int SwitchHelperCore; }

// where the main loop currently is, logged once per second by a watchdog thread (to locate hangs)
static std::atomic<int> DebugStage {0};
static std::atomic<int> DebugFrames {0};

// free memory as seen by the kernel, to find out whether an applet runs out of heap
static void MemLog(const char* label);
#define memlog(label) MemLog(label)

static const char* ConfigDir = "sdmc:/switch/melonDS-Switch";
static const char* OldConfigDir = "sdmc:/switch/melonds-fork";   // before version 0.2: its files are moved at start
#ifndef SWITCH_APP_VERSION
#define SWITCH_APP_VERSION "dev"
#endif

static bool ReadFile(const std::string& path, std::unique_ptr<u8[]>& data, u32& len)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = std::make_unique<u8[]>(size);
    len = (u32)fread(data.get(), 1, size, f);
    fclose(f);
    return len == (u32)size;
}

static std::map<std::string, std::string> ReadConfig(const char* file = "config.txt")
{
    std::map<std::string, std::string> cfg;
    FILE* f = fopen((std::string(ConfigDir) + "/" + file).c_str(), "r");
    if (!f) return cfg;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('=');
        if (eq != std::string::npos)
            cfg[l.substr(0, eq)] = l.substr(eq + 1);
    }
    fclose(f);
    return cfg;
}

// SDL joystick button numbers of a Switch controller -> DS key bits
static u32 DSKeyForButton(int button)
{
    switch (button)
    {
    case 0: return 1 << 0;   // A
    case 1: return 1 << 1;   // B
    case 2: return 1 << 10;  // X
    case 3: return 1 << 11;  // Y
    case 6: return 1 << 9;   // L
    case 7: return 1 << 8;   // R
    case 10: return 1 << 3;  // PLUS -> Start
    case 11: return 1 << 2;  // MINUS -> Select
    case 12: return 1 << 5;  // left
    case 13: return 1 << 6;  // up
    case 14: return 1 << 4;  // right
    case 15: return 1 << 7;  // down
    }
    return 0;
}



unsigned long long MelonGLProf[16];   // see PlatformOGL.h
unsigned MelonGL2DSections = 0;       // see PlatformOGL.h

// sampling profiler (config sampler=<seconds>): a thread on core 2 (unused otherwise) pauses the emulation thread about
// every 500 us, reads its PC, LR and up to 6 return addresses from the frame-pointer chain (only inside the thread's
// stack mapping), and resumes it; after a 5 s delay and <seconds> of sampling, the records are written to samples.bin
// ("SMP3", then 10 x u64 per sample: PC, LR, 6 return addresses, x0, x1). Off by default.
struct SamplerState
{
    Handle Target = INVALID_HANDLE;
    int Seconds = 0;
    std::atomic<bool> Stop {false}, Done {false};
    std::vector<u64> Samples;
    u64 Failures = 0;
};
static void SamplerThread(void* arg)
{
    auto* st = (SamplerState*)arg;
    svcSleepThread(5000000000ll);
    u64 end = armGetSystemTick() + armNsToTicks((u64)st->Seconds * 1000000000ull);
    u64 stackLo = 0, stackHi = 0;
    while (!st->Stop && armGetSystemTick() < end)
    {
        svcSleepThread(500000);
        if (R_FAILED(svcSetThreadActivity(st->Target, ThreadActivity_Paused))) { st->Failures++; continue; }
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, st->Target);
        u64 rec[10] {};
        if (R_SUCCEEDED(rc))
        {
            if (!stackHi)
            {
                MemoryInfo mi {}; u32 pi = 0;
                if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp))) { stackLo = mi.addr; stackHi = mi.addr + mi.size; }
            }
            rec[0] = ctx.pc.x;
            rec[1] = ctx.lr;
            rec[8] = ctx.cpu_gprs[0].x;
            rec[9] = ctx.cpu_gprs[1].x;
            u64 fp = ctx.fp;
            for (int i = 2; i < 8; i++)
            {
                if (fp < stackLo || fp + 16 > stackHi || (fp & 0xF)) break;
                rec[i] = ((u64*)fp)[1];
                u64 next = ((u64*)fp)[0];
                if (next <= fp) break;
                fp = next;
            }
        }
        svcSetThreadActivity(st->Target, ThreadActivity_Runnable);
        if (R_FAILED(rc)) { st->Failures++; continue; }
        st->Samples.insert(st->Samples.end(), rec, rec + 10);
    }
    if (FILE* f = fopen("sdmc:/switch/melonDS-Switch/samples.bin", "wb"))
    {
        fwrite("SMP3", 1, 4, f);
        fwrite(st->Samples.data(), sizeof(u64), st->Samples.size(), f);
        fclose(f);
    }
    st->Done = true;
}

// glColorMaskiOES (per-attachment colour masks, used by the GPU renderer) is an extension: resolved through EGL
extern "C" void glColorMaskiOES(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    typedef void (*Fn)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    static Fn fn = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = (Fn)SDL_GL_GetProcAddress("glColorMaskiOES");
        if (!fn) fn = (Fn)SDL_GL_GetProcAddress("glColorMaskiEXT");
    }
    if (fn) fn(index, r, g, b, a);
}

// ---- drawing: everything goes through one GLES 3 context (SDL window + EGL), so that the GPU renderer and the UI share it ----
struct GfxTex { GLuint Id = 0; int W = 0, H = 0; };   // deko3d: Id = texture id + 1

// presentation through deko3d instead of GLES 3 (render=deko3d): chosen at start, no GL context exists then
#ifdef SWITCH_DEKO3D
static bool GfxDk = false;
#else
static constexpr bool GfxDk = false;
static bool DkGfxInit(const std::function<void(const std::string&)>&) { return false; }
static void DkGfxShutdown() {}
static int DkTexCreate(int, int, bool) { return -1; }
static void DkTexSetLinear(int, bool) {}
static void DkTexUpload(int, int, int, int, int, const void*, int) {}
static void DkTexDestroy(int) {}
static void DkClear() {}
static void DkDraw(const float*, const float*, const float*, int, int, bool) {}
static void DkPresent() {}
#endif

struct Gfx
{
    SDL_Window* Window = nullptr;
    SDL_GLContext Context = nullptr;
    GLuint Prog = 0, ProgArray = 0, Vao = 0;
    GLint ULoc[2][6] {};          // uRect uUV uColor uMode uTex uLayer, per program
    float R = 0, G = 0, B = 0, A = 1;
    bool Blend = false;
    bool Linear = false;
};

static const char* GfxVS = R"(#version 300 es
precision highp float;
uniform vec4 uRect;
uniform vec4 uUV;
out vec2 vUV;
void main()
{
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUV = mix(uUV.xy, uUV.zw, p);
    vec2 px = uRect.xy + p * uRect.zw;
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

static bool GfxInit(Gfx* g, const std::function<void(const std::string&)>& say)
{
    if (GfxDk) return DkGfxInit(say);
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
    const char* names[6] = {"uRect", "uUV", "uColor", "uMode", "uTex", "uLayer"};
    GLuint progs[2] = {g->Prog, g->ProgArray};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 6; k++)
            g->ULoc[i][k] = glGetUniformLocation(progs[i], names[k]);
    glGenVertexArrays(1, &g->Vao);
    glBindVertexArray(g->Vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return true;
}

static void GfxResetState(Gfx* g)
{
    if (GfxDk) return;
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

static void GfxClear(Gfx* g)
{
    if (GfxDk) { DkClear(); return; }
    GfxResetState(g);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void GfxColor(Gfx* g, int r, int gr, int b, int a) { g->R = r / 255.f; g->G = gr / 255.f; g->B = b / 255.f; g->A = a / 255.f; }

static void GfxBlendMode(Gfx* g, bool on)
{
    g->Blend = on;
    if (GfxDk) return;
    if (on) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);
}

static void GfxFill(Gfx* g, const SDL_Rect* r)
{
    if (GfxDk)
    {
        float rect[4] {(float)r->x, (float)r->y, (float)r->w, (float)r->h}, uv[4] {0, 0, 1, 1}, col[4] {g->R, g->G, g->B, g->A};
        DkDraw(rect, uv, col, 0, -1, g->Blend);
        return;
    }
    glUseProgram(g->Prog);
    glUniform4f(g->ULoc[0][0], (float)r->x, (float)r->y, (float)r->w, (float)r->h);
    glUniform4f(g->ULoc[0][1], 0, 0, 1, 1);
    glUniform4f(g->ULoc[0][2], g->R, g->G, g->B, g->A);
    glUniform1i(g->ULoc[0][3], 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// mode 1 = DS software frame, 2 = RGBA texture (text); src in texels (null = whole texture)
static void GfxCopy(Gfx* g, const GfxTex* t, const SDL_Rect* src, const SDL_Rect* dst, int mode)
{
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src) { u0 = (float)src->x / t->W; v0 = (float)src->y / t->H; u1 = (float)(src->x + src->w) / t->W; v1 = (float)(src->y + src->h) / t->H; }
    if (GfxDk)
    {
        float rect[4] {(float)dst->x, (float)dst->y, (float)dst->w, (float)dst->h}, uv[4] {u0, v0, u1, v1}, col[4] {g->R, g->G, g->B, g->A};
        DkDraw(rect, uv, col, mode, (int)t->Id - 1, g->Blend);
        return;
    }
    glUseProgram(g->Prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glUniform1i(g->ULoc[0][4], 0);
    glUniform4f(g->ULoc[0][0], (float)dst->x, (float)dst->y, (float)dst->w, (float)dst->h);
    glUniform4f(g->ULoc[0][1], u0, v0, u1, v1);
    glUniform1i(g->ULoc[0][3], mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// one layer (= one screen) of the GPU renderer's output texture array
static void GfxCopyArray(Gfx* g, GLuint tex, int layer, const SDL_Rect* dst, bool linear)
{
    glUseProgram(g->ProgArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glUniform1i(g->ULoc[1][4], 0);
    glUniform1f(g->ULoc[1][5], (float)layer);
    glUniform4f(g->ULoc[1][0], (float)dst->x, (float)dst->y, (float)dst->w, (float)dst->h);
    glUniform4f(g->ULoc[1][1], 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void GfxTexParams(const GfxTex* t, bool linear)
{
    if (GfxDk) { DkTexSetLinear((int)t->Id - 1, linear); return; }
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static GfxTex GfxTexCreate(int w, int h, bool linear)
{
    GfxTex t;
    t.W = w; t.H = h;
    if (GfxDk) { t.Id = (GLuint)(DkTexCreate(w, h, linear) + 1); return t; }
    glGenTextures(1, &t.Id);
    glBindTexture(GL_TEXTURE_2D, t.Id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GfxTexParams(&t, linear);
    return t;
}

static void GfxTexDestroy(GfxTex* t)
{
    if (t->Id) { if (GfxDk) DkTexDestroy((int)t->Id - 1); else glDeleteTextures(1, &t->Id); }
    *t = GfxTex();
}

// RGBA texture from an SDL surface (text)
static GfxTex GfxTexFromSurface(SDL_Surface* surf)
{
    GfxTex t;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);   // bytes R,G,B,A
    if (!rgba) return t;
    t.W = rgba->w; t.H = rgba->h;
    if (GfxDk)
    {
        int id = DkTexCreate(t.W, t.H, true);
        if (id >= 0) DkTexUpload(id, 0, 0, t.W, t.H, rgba->pixels, rgba->pitch);
        t.Id = (GLuint)(id + 1);
        SDL_FreeSurface(rgba);
        return t;
    }
    glGenTextures(1, &t.Id);
    glBindTexture(GL_TEXTURE_2D, t.Id);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.W, t.H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GfxTexParams(&t, true);
    SDL_FreeSurface(rgba);
    return t;
}

// part of a texture, RGBA8 rows of w texels
static void GfxTexUpload(const GfxTex* t, int x, int y, int w, int h, const void* pixels)
{
    if (!t->Id) return;
    if (GfxDk) { DkTexUpload((int)t->Id - 1, x, y, w, h, pixels, w * 4); return; }
    glBindTexture(GL_TEXTURE_2D, t->Id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void GfxPresent(Gfx* g)
{
    if (GfxDk) DkPresent();
    else SDL_GL_SwapWindow(g->Window);
}

// ---- minimal UI: text with the console's shared font, ROM browser ----
static TTF_Font* UIFont = nullptr;
static const SDL_Color White {240, 240, 240, 255}, Grey {150, 160, 175, 255}, Accent {120, 210, 255, 255};

static void DrawText(Gfx* r, const std::string& text, int x, int y, SDL_Color c)
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

static bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string BaseName(const std::string& path)
{
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

static std::string StripExt(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static std::string LowerCase(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// returns the chosen .nds path, or "" if the user quit (PLUS)
static std::string BrowseRom(Gfx* r, std::string dir)
{
    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> entries;
    int sel = 0, top = 0;
    const int rows = 14;

    auto join = [](const std::string& d, const std::string& n) { return d.back() == '/' ? d + n : d + "/" + n; };
    auto load = [&]() {
        entries.clear();
        if (dir.size() > 6) entries.push_back({"..", true});
        if (DIR* d = opendir(dir.c_str()))
        {
            std::vector<Entry> dirs, files;
            while (dirent* e = readdir(d))
            {
                std::string n = e->d_name;
                if (n.empty() || n[0] == '.') continue;
                struct stat st;
                bool isDir = e->d_type == DT_DIR || (stat(join(dir, n).c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (isDir) dirs.push_back({n, true});
                else
                {
                    std::string l = LowerCase(n);
                    if (l.size() > 4 && (l.compare(l.size() - 4, 4, ".nds") == 0 || l.compare(l.size() - 4, 4, ".srl") == 0))
                        files.push_back({n, false});
                }
            }
            closedir(d);
            auto byName = [](const Entry& a, const Entry& b) { return LowerCase(a.name) < LowerCase(b.name); };
            std::sort(dirs.begin(), dirs.end(), byName);
            std::sort(files.begin(), files.end(), byName);
            entries.insert(entries.end(), dirs.begin(), dirs.end());
            entries.insert(entries.end(), files.begin(), files.end());
        }
        sel = 0; top = 0;
    };
    load();

    while (appletMainLoop())
    {
        GfxClear(r);
        GfxColor(r, 18, 22, 30, 255);
        { SDL_Rect bg {0, 0, 1280, 720}; GfxFill(r, &bg); }
        DrawText(r, "Choisir une ROM", 60, 24, Accent);
        DrawText(r, dir, 60, 66, Grey);
        for (int i = 0; i < rows && top + i < (int)entries.size(); i++)
        {
            const Entry& e = entries[top + i];
            int y = 120 + i * 36;
            if (top + i == sel)
            {
                GfxColor(r, 40, 70, 110, 255);
                SDL_Rect bar {40, y - 2, 1200, 36};
                GfxFill(r, &bar);
            }
            DrawText(r, (e.isDir ? "[dossier]  " : "") + e.name, 60, y, top + i == sel ? White : Grey);
        }
        if (entries.empty()) DrawText(r, "(aucune ROM .nds ici)", 60, 120, Grey);
        DrawText(r, "A : choisir     B : dossier parent     +/- : quitter", 60, 656, Grey);
        GfxPresent(r);

        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 200) || ev.type != SDL_JOYBUTTONDOWN) continue;
        int n = (int)entries.size();
        switch (ev.jbutton.button)
        {
        case 13: if (n) sel = std::max(sel - 1, 0); break;
        case 15: if (n) sel = std::min(sel + 1, n - 1); break;
        case 12: if (n) sel = std::max(sel - rows, 0); break;
        case 14: if (n) sel = std::min(sel + rows, n - 1); break;
        case 10: case 11: return "";
        case 1:
        {
            if (dir.size() > 6)
            {
                size_t sl = dir.find_last_of('/');
                dir = sl <= 5 ? "sdmc:/" : dir.substr(0, sl);
                load();
            }
            break;
        }
        case 0:
        {
            if (!n) break;
            const Entry& e = entries[sel];
            if (e.isDir)
            {
                if (e.name == "..")
                {
                    size_t sl = dir.find_last_of('/');
                    dir = sl <= 5 ? "sdmc:/" : dir.substr(0, sl);
                }
                else dir = join(dir, e.name);
                load();
            }
            else return join(dir, e.name);
            break;
        }
        }
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
    return "";
}


// Probe of the GLES3 stack (config gltest=1): context version, extensions, swap rate. Writes to the log and exits.
static int GlProbe(std::function<void(const std::string&)> say)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_Window* win = nullptr;
    SDL_GLContext ctx = nullptr;
    int tryVersions[][2] = {{3, 2}, {3, 1}, {3, 0}};
    for (auto& v : tryVersions)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v[0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v[1]);
        win = SDL_CreateWindow("gl", 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
        if (!win) { say(std::string("window failed: ") + SDL_GetError()); continue; }
        ctx = SDL_GL_CreateContext(win);
        say("context ES " + std::to_string(v[0]) + "." + std::to_string(v[1]) + ": " + (ctx ? "ok" : std::string("FAILED ") + SDL_GetError()));
        if (ctx) break;
        SDL_DestroyWindow(win);
        win = nullptr;
    }
    if (!ctx) return 1;
    auto str = [](GLenum e) { const GLubyte* g = glGetString(e); return std::string(g ? (const char*)g : "(null)"); };
    say("GL_VERSION: " + str(GL_VERSION));
    say("GL_RENDERER: " + str(GL_RENDERER));
    say("GL_VENDOR: " + str(GL_VENDOR));
    say("GLSL: " + str(GL_SHADING_LANGUAGE_VERSION));
    std::string ext = str(GL_EXTENSIONS);
    for (const char* e : {"GL_OES_draw_buffers_indexed", "GL_EXT_draw_buffers_indexed", "GL_EXT_color_buffer_float", "GL_OES_texture_float", "GL_EXT_texture_border_clamp", "GL_OES_texture_border_clamp", "GL_EXT_shader_framebuffer_fetch", "GL_NV_framebuffer_blit", "GL_OES_texture_stencil8"})
        say(std::string(e) + ": " + (ext.find(e) != std::string::npos ? "yes" : "no"));
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v); say("max texture " + std::to_string(v));
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v); say("max draw buffers " + std::to_string(v));
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &v); say("max UBO bindings " + std::to_string(v));
    SDL_GL_SetSwapInterval(1);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 180; i++)
    {
        glClearColor((i % 60) / 60.f, 0.3f, 0.5f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    say("180 swaps in " + std::to_string(sec) + " s = " + std::to_string(180 / sec) + " fps");
    glGetError();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    return 0;
}

static void MemLog(const char* label)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    FILE* f = fopen((SwitchLogPath).c_str(), "a");
    if (!f) return;
    fprintf(f, "mem %s: used %llu / %llu MB\n", label, (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
    fclose(f);
}

int main(int argc, char** argv)
{
    mkdir(ConfigDir, 0777);
    // settings, saves and states of the versions before 0.2 (sdmc:/switch/melonds-fork): moved here, existing files kept
    int movedFiles = 0;
    if (DIR* d = opendir(OldConfigDir))
    {
        while (dirent* e = readdir(d))
        {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "melonds-fork.nro")) continue;
            std::string from = std::string(OldConfigDir) + "/" + e->d_name, to = std::string(ConfigDir) + "/" + e->d_name;
            struct stat st;
            if (stat(to.c_str(), &st) != 0 && rename(from.c_str(), to.c_str()) == 0) movedFiles++;
        }
        closedir(d);
        rmdir(OldConfigDir);   // only if empty
    }
    SwitchLogPath = std::string(ConfigDir) + "/log.txt";
    melonDS::SwitchCrashLogPath = SwitchLogPath.c_str();
    // the file is closed after every line: FAT only records its size on close, and the log must be readable while the app runs
    // one lock for every writer of the log (main thread, statistics thread, Platform::Log): two appends at once
    // would overwrite each other
    auto logf = [](const char* fmt, auto... a) {
        std::lock_guard<std::mutex> lock(SwitchLogMutex);
        FILE* f = fopen(SwitchLogPath.c_str(), "a");
        if (!f) return;
        fprintf(f, fmt, a...);
        fclose(f);
    };

    auto cfg = ReadConfig();
    if (cfg.count("gltest") && cfg["gltest"] == "1")
    {
        logf("=== gl probe ===\n");
        int rc = GlProbe([&](const std::string& m) { logf("%s\n", m.c_str()); });
        logf("=== gl probe done rc=%d ===\n", rc);
        return rc;
    }
    if (cfg.count("helpercore")) Platform::SwitchHelperCore = atoi(cfg["helpercore"].c_str());
    // defaults (best measured setup); config.txt overrides, then ui.txt (menu), then the defaults below
    auto ui0 = ReadConfig("ui.txt");
    const bool useJit = !cfg.count("jit") || cfg["jit"] != "0";
    logf("=== melonDS Switch %s start, jit=%d ===\n", SWITCH_APP_VERSION, useJit ? 1 : 0);
    if (movedFiles) logf("moved %d files from %s\n", movedFiles, OldConfigDir);

    // optional CPU overclock (off by default), restored on exit
    ClkrstSession cpuSession {};
    bool clkOk = false;
    u32 oldClock = 0;
    // CPU clock: config.txt overclock= (fixed) > ui.txt cpu= (0 = auto) > auto. Auto starts at 1224 MHz and goes to 1785 MHz
    // when the game drops under 57 fps for 3 seconds (it never goes back down during a session)
    int overclockMHz = cfg.count("overclock") ? atoi(cfg["overclock"].c_str()) : (ui0.count("cpu") ? atoi(ui0["cpu"].c_str()) : 0);
    bool autoClock = overclockMHz == 0 && (!cfg.count("overclock") || cfg["overclock"] == "auto");   // overclock=auto forces the auto mode
    if (autoClock) overclockMHz = 1224;
    // 3D renderer: the GLES 3 renderer (default, 2x) or software; software is also the fallback if the GL renderer fails. The GLES 3 renderer on the console's GPU (config gpu=0|1, menu, ui.txt)
    bool useGpu = cfg.count("gpu") ? cfg["gpu"] == "1" : (ui0.count("gpu") ? ui0["gpu"] == "1" : true);
    int gpuScale = std::clamp(cfg.count("gpuscale") ? atoi(cfg["gpuscale"].c_str()) : (ui0.count("gpuscale") ? atoi(ui0["gpuscale"].c_str()) : 2), 1, 3);
    bool threaded3d = cfg.count("threaded3d") ? cfg["threaded3d"] == "1" : (ui0.count("threaded") ? ui0["threaded"] == "1" : true);
    if (R_SUCCEEDED(clkrstInitialize()))
    {
        if (R_SUCCEEDED(clkrstOpenSession(&cpuSession, PcvModuleId_CpuBus, 3)))
        {
            clkOk = true;
            clkrstGetClockRate(&cpuSession, &oldClock);
            logf("cpu clock: %u MHz\n", oldClock / 1000000);
            if (overclockMHz > 0)
            {
                Result rc = clkrstSetClockRate(&cpuSession, (u32)overclockMHz * 1000000u);
                logf("overclock %d MHz: rc=0x%x\n", overclockMHz, rc);
                u32 applied = 0;
                clkrstGetClockRate(&cpuSession, &applied);
                logf("cpu clock after set: %u MHz\n", applied / 1000000);
            }
        }
    }

    // memory and GPU clocks, to see what else changes with the CPU clock
    ClkrstSession emcSession {}, gpuSession {};
    bool emcOk = clkOk && R_SUCCEEDED(clkrstOpenSession(&emcSession, PcvModuleId_EMC, 3));
    bool gpuOk = clkOk && R_SUCCEEDED(clkrstOpenSession(&gpuSession, PcvModuleId_GPU, 3));
    // gpuclock=<MHz> (0 = system default), restored on exit
    u32 oldGpuClock = 0;
    if (gpuOk && cfg.count("gpuclock") && atoi(cfg["gpuclock"].c_str()) > 0)
    {
        clkrstGetClockRate(&gpuSession, &oldGpuClock);
        Result rc = clkrstSetClockRate(&gpuSession, (u32)std::clamp(atoi(cfg["gpuclock"].c_str()), 76, 921) * 1000000u);
        u32 applied = 0;
        clkrstGetClockRate(&gpuSession, &applied);
        logf("gpu clock %u -> %u MHz (rc=0x%x)\n", oldGpuClock / 1000000, applied / 1000000, rc);
    }

    // temperature probe (board sensor), logged each second
    TsSession tsSession {};
    bool tsOk = R_SUCCEEDED(tsInitialize()) && R_SUCCEEDED(tsOpenSession(&tsSession, TsDeviceCode_LocationExternal));
    logf("temperature sensor: %s\n", tsOk ? "ok" : "unavailable");

    memlog("before SDL");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
#ifdef SWITCH_DEKO3D
    GfxDk = cfg.count("render") && cfg["render"] == "deko3d";
    logf("presentation: %s\n", GfxDk ? "deko3d" : "GLES 3 (Mesa)");
#endif
    Gfx gfx;
    if (!GfxInit(&gfx, [&](const std::string& m) { logf("%s\n", m.c_str()); }))
    {
        logf("cannot create the GL context, giving up\n");
        return 1;
    }
    Gfx* renderer = &gfx;
    GfxTex tex;
    std::vector<SDL_Joystick*> joysticks;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        joysticks.push_back(SDL_JoystickOpen(i));

    // text: the console's shared font
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && TTF_Init() == 0)
    {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)))
            UIFont = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 0, 28);
    }
    logf("ui font: %s\n", UIFont ? "ok" : "unavailable");

    // display settings (menu), kept in ui.txt
    auto ui = ReadConfig("ui.txt");
    int layout = ui.count("layout") ? atoi(ui["layout"].c_str()) : 0;     // 0 stacked, 1 side by side
    int filter = ui.count("filter") ? atoi(ui["filter"].c_str()) : 0;     // 0 sharp pixels, 1 smooth
    bool showFps = ui.count("showfps") && ui["showfps"] == "1";
    // display settings are global (ui.txt); renderer, resolution and CPU chosen in the menu are saved for the running
    // game (<rom>.ui), ui.txt keeps them as the defaults of the other games
    std::string gameUiFile;
    auto saveUi = [&]() {
        FILE* f = fopen((std::string(ConfigDir) + "/ui.txt").c_str(), "w");
        if (!f) return;
        fprintf(f, "layout=%d\nfilter=%d\nshowfps=%d\n", layout, filter, showFps ? 1 : 0);
        for (const char* k : {"cpu", "threaded", "gpu", "gpuscale"})
            if (ui0.count(k)) fprintf(f, "%s=%s\n", k, ui0[k].c_str());
        fclose(f);
        if (gameUiFile.empty()) return;
        f = fopen((std::string(ConfigDir) + "/" + gameUiFile).c_str(), "w");
        if (!f) return;
        fprintf(f, "cpu=%d\nthreaded=%d\ngpu=%d\ngpuscale=%d\n", autoClock ? 0 : overclockMHz, threaded3d ? 1 : 0, useGpu ? 1 : 0, gpuScale);
        fclose(f);
    };

    auto makeTexture = [&]() {
        if (tex.Id) GfxTexParams(&tex, filter != 0);
        else tex = GfxTexCreate(256, 384, filter != 0);
    };

    // which ROM: rom= in config.txt wins, else the last one chosen (lastrom.txt); the browser opens if none, or with --browse
    bool wantBrowse = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--browse")) wantBrowse = true;
    std::string romPath = cfg.count("rom") ? cfg["rom"] : "";
    const std::string configRom = romPath;
    if (romPath.empty())
    {
        auto last = ReadConfig("lastrom.txt");
        if (last.count("rom")) romPath = last["rom"];
    }
    if (wantBrowse || romPath.empty() || !FileExists(romPath))
    {
        std::string startDir = cfg.count("romdir") ? cfg["romdir"] : "";
        if (startDir.empty() && !romPath.empty())
        {
            size_t sl = romPath.find_last_of('/');
            if (sl != std::string::npos) startDir = romPath.substr(0, sl);
        }
        if (startDir.empty())
        {
            struct stat st;
            startDir = stat("sdmc:/roms/nds", &st) == 0 ? "sdmc:/roms/nds" : "sdmc:/";
        }
        romPath = BrowseRom(renderer, startDir);
        if (romPath.empty())
        {
            logf("no ROM chosen\n");
            return 0;
        }
        FILE* f = fopen((std::string(ConfigDir) + "/lastrom.txt").c_str(), "w");
        if (f) { fprintf(f, "rom=%s\n", romPath.c_str()); fclose(f); }
    }
    logf("rom: %s\n", romPath.c_str());
    makeTexture();

    SDL_AudioSpec want {}, have {};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    const int audioRate = have.freq ? have.freq : want.freq;
    logf("audio: %d Hz, %d channels, format 0x%x, buffer %d samples\n", have.freq, have.channels, (unsigned)have.format, have.samples);

    memlog("after SDL");

    std::atomic<bool> watchdogStop {false};
    std::thread watchdog([&]() {
        int last = -1, idle = 0;
        while (!watchdogStop)
        {
            SDL_Delay(1000);
            int frames = DebugFrames;
            idle = (frames == last) ? idle + 1 : 0;
            last = frames;
            if (idle >= 1 && DebugStage != 99)
                logf("watchdog: no frame progress for %ds (frames=%d, stage=%d, faults=%llu, lastfault addr=%llx pc=%llx)\n", idle, frames, (int)DebugStage, (unsigned long long)SwitchFaultCount, (unsigned long long)SwitchLastFaultAddr, (unsigned long long)SwitchLastFaultPC);
        }
    });

    std::unique_ptr<u8[]> romData;
    u32 romLen;
    if (!ReadFile(romPath, romData, romLen))
    {
        logf("cannot read rom %s\n", romPath.c_str());
        return 1;
    }

    // per-game settings: <rom>.ui (menu choices made while this game runs) > built-in defaults for games known to need
    // them > ui.txt; config.txt still wins. Built-in: 4th generation Pokemon in software rendering (the GL renderer leaves
    // dotted black lines between map polygons, upstream issue #458; software holds 60 fps at 1224 MHz on HeartGold)
    gameUiFile = StripExt(BaseName(romPath)) + ".ui";
    {
        auto gu = ReadConfig(gameUiFile.c_str());
        const bool fromFile = !gu.empty();
        char code[5] = {};
        if (romLen >= 0x10) memcpy(code, romData.get() + 0x0C, 4);
        bool builtinSoft = false;
        for (const char* c : {"ADA", "APA", "CPU", "IPK", "IPG"})   // Diamond, Pearl, Platinum, HeartGold, SoulSilver
            if (!strncmp(code, c, 3)) builtinSoft = true;
        if (builtinSoft && !gu.count("gpu")) gu["gpu"] = "0";
        if (!cfg.count("gpu") && gu.count("gpu")) useGpu = gu["gpu"] == "1";
        if (!cfg.count("gpuscale") && gu.count("gpuscale")) gpuScale = std::clamp(atoi(gu["gpuscale"].c_str()), 1, 3);
        if (!cfg.count("threaded3d") && gu.count("threaded")) threaded3d = gu["threaded"] == "1";
        if (!cfg.count("overclock") && gu.count("cpu"))
        {
            int mhz = atoi(gu["cpu"].c_str());
            autoClock = mhz == 0;
            overclockMHz = autoClock ? 1224 : mhz;
            if (clkOk) clkrstSetClockRate(&cpuSession, (u32)overclockMHz * 1000000u);
        }
        logf("game %s: %s%s, gpu=%d scale=%d cpu=%d%s\n", code, fromFile ? gameUiFile.c_str() : "global settings", builtinSoft ? " (built-in: software rendering)" : "", (int)useGpu, gpuScale, overclockMHz, autoClock ? " auto" : "");
    }

    NDSArgs args;
    if (!useJit)
        args.JIT = std::nullopt;
    else
    {
        if (cfg.count("fastmem") && cfg["fastmem"] == "0")
            args.JIT->FastMemory = false;
        if (cfg.count("jitblock")) args.JIT->MaxBlockSize = (unsigned)std::clamp(atoi(cfg["jitblock"].c_str()), 1, 256);
        if (cfg.count("jitliteral")) args.JIT->LiteralOptimizations = cfg["jitliteral"] != "0";
        if (cfg.count("jitbranch")) args.JIT->BranchOptimizations = cfg["jitbranch"] != "0";
        logf("jit: max block %u, literal opt %d, branch opt %d\n", args.JIT->MaxBlockSize, (int)args.JIT->LiteralOptimizations, (int)args.JIT->BranchOptimizations);
    }
    args.OutputSampleRate = audioRate;
    args.Interpolation = static_cast<AudioInterpolation>(cfg.count("audiointerp") ? std::clamp(atoi(cfg["audiointerp"].c_str()), 0, 4) : 3);
    logf("audio interpolation: %d\n", (int)args.Interpolation);
    DebugStage = 10;
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    // main-loop step: 128 cycles here (melonDS uses 64), about +2 to +4 fps; events still fire at their exact time
    nds->MaxIterationCycles = cfg.count("itercycles") ? std::clamp(atoi(cfg["itercycles"].c_str()), 16, 512) : 128;
    logf("main loop iteration: %d cycles\n", (int)nds->MaxIterationCycles);
    logf("addrs: main=%llx win9=%llx win7=%llx codemem=%llx\n", (unsigned long long)(u64)&main, (unsigned long long)SwitchDbgWin9, (unsigned long long)SwitchDbgWin7, (unsigned long long)SwitchDbgCodeMem);
    memlog("after NDS");

    NDSCart::NDSCartArgs cartArgs {};
    {
        // sav= only applies to the ROM named in config.txt; any other ROM gets <name>.sav next to the config
        SwitchSavePath = (cfg.count("sav") && romPath == configRom) ? cfg["sav"] : std::string(ConfigDir) + "/" + StripExt(BaseName(romPath)) + ".sav";
        std::unique_ptr<u8[]> sav;
        u32 savLen = 0;
        if (ReadFile(SwitchSavePath, sav, savLen))
        {
            cartArgs.SRAM = std::move(sav);
            cartArgs.SRAMLength = savLen;
        }
        else
            logf("no save at %s, starting empty\n", SwitchSavePath.c_str());
    }
    auto cart = NDSCart::ParseROM(std::move(romData), romLen, nullptr, std::move(cartArgs));
    if (!cart)
    {
        logf("failed to parse the ROM\n");
        return 1;
    }
    nds->SetNDSCart(std::move(cart));
    DebugStage = 11;
    nds->Reset();
    DebugStage = 12;

    // 3D renderer: software (optionally on its own thread: 3 cores are available to homebrew) or GLES 3 on the GPU
    auto applyRenderer = [&](bool gpu) -> bool {
        if (gpu && GfxDk)
        {
            logf("GPU renderer (GLES 3) unavailable with the deko3d presentation\n");
            return false;
        }
#ifdef OGLRENDERER_ENABLED
        if (gpu)
        {
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
            RendererSettings rs {};
            rs.ScaleFactor = gpuScale;
            rs.Threaded = false;
            rs.HiresCoordinates = true;
            rs.BetterPolygons = false;
            nds->GetRenderer().SetRenderSettings(rs);
            int current = 0, count = 0;
            while (nds->GetRenderer().NeedsShaderCompile())
                nds->GetRenderer().ShaderCompileStep(current, count);
            logf("GPU renderer (GLES 3) enabled, scale %d\n", gpuScale);
            return true;
        }
#endif
        if (gpu)
        {
            logf("this build has no GPU renderer\n");
            return false;
        }
        nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
        if (threaded3d)
        {
            RendererSettings rs {};
            rs.ScaleFactor = 1;
            rs.Threaded = true;
            nds->GetRenderer().SetRenderSettings(rs);
            logf("threaded 3D renderer enabled\n");
        }
        return true;
    };
    if (useGpu && !applyRenderer(true))
        useGpu = false;
    else if (!useGpu && threaded3d)
        applyRenderer(false);

    DebugStage = 13;
    nds->SetupDirectBoot(romPath);
    DebugStage = 14;
    nds->Start();
    DebugStage = 15;
    logf("fastmem: %s, main thread on core %d, helper core %d\n", nds->JIT.FastMemoryEnabled() ? "on" : "off", (int)svcGetCurrentProcessorNumber(), Platform::SwitchHelperCore);
    memlog("after Start");

    SamplerState sampler;
    Thread samplerThread {};
    bool samplerOn = false, samplerLogged = false;
    if (cfg.count("sampler") && atoi(cfg["sampler"].c_str()) > 0)
    {
        sampler.Target = threadGetCurHandle();
        sampler.Seconds = std::clamp(atoi(cfg["sampler"].c_str()), 1, 120);
        sampler.Samples.reserve((size_t)sampler.Seconds * 2000 * 10);
        samplerOn = R_SUCCEEDED(threadCreate(&samplerThread, SamplerThread, &sampler, nullptr, 0x10000, 0x2B, 2)) && R_SUCCEEDED(threadStart(&samplerThread));
        logf("sampler: %s, %d s after a 5 s delay\n", samplerOn ? "started" : "FAILED to start", sampler.Seconds);
    }

    // screens: stacked (480x720, centred) or side by side (2 x 640x480)
    SDL_Rect topR {}, botR {};
    auto computeLayout = [&]() {
        if (layout == 1) { topR = {0, 120, 640, 480}; botR = {640, 120, 640, 480}; }
        else { topR = {400, 0, 480, 360}; botR = {400, 360, 480, 360}; }
    };
    computeLayout();
    GfxTex fpsTex;
    GfxTex noteTex;   // short message at the bottom of the screen (5 s)
    auto noteUntil = std::chrono::steady_clock::now();
    bool relaunch = false;

    // save states
    const std::string romBase = std::string(ConfigDir) + "/" + StripExt(BaseName(romPath));
    auto statePath = [&](int slot) { return romBase + ".ss" + std::to_string(slot); };
    // the last state saved or loaded stays in memory: loading it again does not read the SD card (~19 MB, the game
    // froze ~1.5 s and the sound cut). Saves during the game are written to the SD card by a background thread.
    int cachedSlot = -1;
    std::shared_ptr<std::vector<u8>> cachedState;
    std::thread stateWriter;
    // state files are compressed with LZ4: "MDSZ", u32 size of the state, LZ4 block (a 19.5 MB Mario Kart DS state is
    // ~3 MB: less to write and read on the SD card). Files without this header (older states) are read as they are.
    auto writeStateFile = [](const std::string& path, const std::vector<u8>& data) -> bool {
        std::vector<u8> out(8 + LZ4_compressBound((int)data.size()));
        memcpy(out.data(), "MDSZ", 4);
        u32 rawSize = (u32)data.size();
        memcpy(out.data() + 4, &rawSize, 4);
        int n = LZ4_compress_default((const char*)data.data(), (char*)out.data() + 8, (int)data.size(), (int)out.size() - 8);
        if (n <= 0) return false;
        out.resize(8 + n);
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        ok = (fclose(f) == 0) && ok;
        if (!ok) { remove(tmp.c_str()); return false; }
        remove(path.c_str());
        return rename(tmp.c_str(), path.c_str()) == 0;
    };
    auto saveState = [&](int s, bool background) -> bool {
        Savestate st;
        if (st.Error) return false;
        if (!nds->DoSavestate(&st) || st.Error) return false;
        auto data = std::make_shared<std::vector<u8>>((u8*)st.Buffer(), (u8*)st.Buffer() + st.Length());
        if (stateWriter.joinable()) stateWriter.join();   // one write at a time
        cachedSlot = s;
        cachedState = data;
        std::string path = statePath(s);
        if (!background) return writeStateFile(path, *data);
        stateWriter = std::thread([path, data, s, writeStateFile, logf]() {
            if (!writeStateFile(path, *data)) logf("state slot %d: writing the file FAILED\n", s);
        });
        return true;
    };
    auto loadState = [&](int s) -> bool {
        std::shared_ptr<std::vector<u8>> data = s == cachedSlot ? cachedState : nullptr;
        if (!data)
        {
            std::unique_ptr<u8[]> buf;
            u32 len = 0;
            if (!ReadFile(statePath(s), buf, len) || len < 16) return false;
            if (!memcmp(buf.get(), "MDSZ", 4))
            {
                u32 rawSize;
                memcpy(&rawSize, buf.get() + 4, 4);
                data = std::make_shared<std::vector<u8>>(rawSize);
                if (LZ4_decompress_safe((const char*)buf.get() + 8, (char*)data->data(), (int)(len - 8), (int)rawSize) != (int)rawSize)
                    return false;
            }
            else
                data = std::make_shared<std::vector<u8>>(buf.get(), buf.get() + len);
        }
        // keep the running state so that a bad file does not leave the emulator half-loaded
        Savestate backup;
        if (backup.Error || !nds->DoSavestate(&backup) || backup.Error) return false;
        Savestate st(data->data(), (u32)data->size(), false);
        if (!st.Error && nds->DoSavestate(&st) && !st.Error)
        {
            cachedSlot = s;
            cachedState = data;
            return true;
        }
        Savestate restore(backup.Buffer(), backup.Length(), false);
        nds->DoSavestate(&restore);
        return false;
    };
    const bool autosave = !cfg.count("autosave") || cfg["autosave"] != "0";
    bool stateLoaded = false;   // auto CPU mode: a state often lands in the middle of the action, go to 1785 MHz at once
    if (!cfg.count("autoload") || cfg["autoload"] != "0")
    {
        int loadSlot = cfg.count("loadslot") ? std::clamp(atoi(cfg["loadslot"].c_str()), 0, 4) : 0;   // slot loaded at start (0 = the automatic one)
        FILE* f = fopen(statePath(loadSlot).c_str(), "rb");
        if (f)
        {
            fclose(f);
            stateLoaded = loadState(loadSlot);
            logf("autoload slot %d: %s\n", loadSlot, stateLoaded ? "ok" : "FAILED");
        }
    }
    int slot = 1;
    // CPU auto mode: starts at 1224 MHz, raised to 1785 MHz as soon as the game falls behind (the last 30 frames took
    // more than 540 ms, i.e. 3 missed refreshes in ~0.5 s; before: 3 s under 57 fps), lowered back to 1224 MHz after
    // autoHold seconds at full speed with CPU headroom (see busyPermille). If the game falls behind within 10 s of a lowering, the hold time doubles (4 min max).
    std::chrono::steady_clock::time_point autoFrameTimes[31];
    int autoFrameCount = 0;   // frames recorded since the last reset (state loaded, menu, clock change)
    int autoFastSeconds = 0, autoHold = 30, autoLoweredAt = -1000;
    // CPU time of the emulation thread (ticks at 19.2 MHz, waits in the GPU driver/vsync excluded), per second, in 1/1000
    u64 busyTicks0 = 0;   // read at the first second (the first value is not used)
    int busyPermille = 0;
    // GL renderer: games that change display registers mid-frame (Professor Layton videos: ~14 sections per screen and
    // per frame, 17-25 fps at 1785 MHz; software rendering: 60 fps at 1020 MHz) are switched to software rendering, and
    // the choice is saved for the game
    int sectionsTenths = 0, heavySectionSeconds = 0;
    double audioSkew = 1.0;
    // audio rate control: audioctl=1 (default) uses the queue level averaged over ~20 frames, audioctl=0 the raw level
    // of each frame (previous behaviour). audioskew=<ratio> fixes the rate (reference recordings).
    // audiodump=<seconds>: records what is sent to the speaker (dump.wav) and the per-frame rate (dump-skew.txt)
    const int audioCtl = cfg.count("audioctl") ? atoi(cfg["audioctl"].c_str()) : 1;
    const double audioFixedSkew = cfg.count("audioskew") ? atof(cfg["audioskew"].c_str()) : 0.0;
    const int audioDumpSec = cfg.count("audiodump") ? std::clamp(atoi(cfg["audiodump"].c_str()), 0, 120) : 0;
    std::vector<s16> audioDump;
    std::vector<float> audioDumpSkew;
    bool audioDumpDone = audioDumpSec == 0;
    double audioQueueAvg = -1.0;
    // the rate control aims at 100 ms of queued audio: after the queue is emptied (start, state loaded, menu closed), fill
    // it with 100 ms of silence, otherwise the control stretched the sound (lower pitch, down to 0.7x) for about a second
    // (250 ms at start, 200 ms after a state load or the menu: the next frame takes ~100 ms, the GL renderer reloading)
    auto primeAudio = [&](int ms) {
        if (!audio) return;
        SDL_ClearQueuedAudio(audio);
        std::vector<s16> silence((size_t)audioRate * ms / 1000 * 2, 0);
        SDL_QueueAudio(audio, silence.data(), (u32)(silence.size() * sizeof(s16)));
        audioQueueAvg = -1.0;
    };
    if (audioDumpSec) audioDump.reserve((size_t)audioRate * 2 * audioDumpSec);
    logf("audio control %d, fixed skew %.4f, dump %d s\n", audioCtl, audioFixedSkew, audioDumpSec);
    u32 pad = 0;                      // raw SDL buttons held (bit = button number)
    u8 fbR = 0, fbG = 0, fbB = 0;     // feedback colour of the last state action
    int fbSlot = 0;
    auto fbUntil = std::chrono::steady_clock::now();
    auto feedback = [&](u8 r, u8 g, u8 b, int sl) { fbR = r; fbG = g; fbB = b; fbSlot = sl; fbUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000); };

    GLuint gpuTex = 0;   // output texture array of the GPU renderer (layer 0 = top screen, 1 = bottom)
    auto drawScreens = [&]() {
        if (gpuTex)
        {
            GfxCopyArray(renderer, gpuTex, 0, &topR, filter != 0);
            GfxCopyArray(renderer, gpuTex, 1, &botR, filter != 0);
            return;
        }
        SDL_Rect srcTop {0, 0, 256, 192}, srcBot {0, 192, 256, 192};
        GfxCopy(renderer, &tex, &srcTop, &topR, 1);
        GfxCopy(renderer, &tex, &srcBot, &botR, 1);
    };

    // in-game menu (ZL + ZR + PLUS): emulation is paused. Returns 0 = resume, 1 = change ROM, 2 = quit
    auto runMenu = [&]() -> int {
        if (audio) SDL_PauseAudioDevice(audio, 1);
        const int prevStage = DebugStage;
        DebugStage = 99;   // paused: the watchdog must not report a hang
        const char* names[] = {"Reprendre", "Disposition", "Filtre", "Afficher les fps", "Fréquence CPU", "Rendu 3D", "Résolution GPU", "3D en thread séparé", "Emplacement d'état", "Sauvegarder l'état", "Charger l'état", "Changer de ROM", "Quitter"};
        const int count = 13;
        int sel = 0, result = -1;
        std::string note;
        while (result < 0 && appletMainLoop())
        {
            GfxClear(renderer);
            drawScreens();
            GfxBlendMode(renderer, true);
            GfxColor(renderer, 10, 14, 22, 215);
            SDL_Rect full {0, 0, 1280, 720};
            GfxFill(renderer, &full);
            GfxBlendMode(renderer, false);
            DrawText(renderer, "Menu  -  melonDS Switch " SWITCH_APP_VERSION, 420, 40, Accent);
            for (int i = 0; i < count; i++)
            {
                std::string label = names[i];
                if (i == 1) label += std::string(" : ") + (layout ? "côte à côte" : "empilées");
                if (i == 2) label += std::string(" : ") + (filter ? "lissé" : "pixels nets");
                if (i == 3) label += std::string(" : ") + (showFps ? "oui" : "non");
                if (i == 4) label += autoClock ? " : auto (" + std::to_string(overclockMHz) + " MHz)" : " : " + std::to_string(overclockMHz) + " MHz";
                if (i == 5) label += std::string(" : ") + (useGpu ? "GPU (GLES 3)" : "logiciel");
                if (i == 6) label += " : " + std::to_string(gpuScale) + "x" + (useGpu ? "" : " (rendu GPU seulement)");
                if (i == 7) label += std::string(" : ") + (threaded3d ? "oui (logiciel)" : "non");
                if (i == 8) label += " : " + std::to_string(slot);
                int y = 86 + i * 40;
                if (i == sel)
                {
                    GfxColor(renderer, 40, 70, 110, 255);
                    SDL_Rect bar {400, y - 3, 480, 38};
                    GfxFill(renderer, &bar);
                }
                DrawText(renderer, label, 420, y, i == sel ? White : Grey);
            }
            if (!note.empty()) DrawText(renderer, note, 420, 86 + count * 40 + 4, Accent);
            DrawText(renderer, "A : valider   gauche/droite : changer   B : retour", 330, 672, Grey);
            GfxPresent(renderer);

            SDL_Event ev;
            if (!SDL_WaitEventTimeout(&ev, 200) || ev.type != SDL_JOYBUTTONDOWN) continue;
            int b = ev.jbutton.button;
            int dir = b == 14 ? 1 : b == 12 ? -1 : 0;
            if (b == 13) sel = (sel + count - 1) % count;
            else if (b == 15) sel = (sel + 1) % count;
            else if (b == 1 || b == 10) result = 0;
            else if (b == 0 || dir)
            {
                bool act = b == 0;
                switch (sel)
                {
                case 0: if (act) result = 0; break;
                case 1: layout ^= 1; computeLayout(); saveUi(); break;
                case 2: filter ^= 1; makeTexture(); saveUi(); break;
                case 3: showFps = !showFps; saveUi(); break;
                case 4:
                {
                    const int steps[] = {0, 1020, 1224, 1581, 1785};   // 0 = auto
                    int idx = 0;
                    if (!autoClock)
                        for (int k = 1; k < 5; k++) if (steps[k] <= overclockMHz) idx = k;
                    idx = dir ? std::clamp(idx + dir, 0, 4) : (idx + 1) % 5;
                    autoClock = steps[idx] == 0;
                    overclockMHz = autoClock ? 1224 : steps[idx];
                    autoFrameCount = 0;
                    autoFastSeconds = 0;
                    autoHold = 30;
                    if (clkOk) clkrstSetClockRate(&cpuSession, (u32)overclockMHz * 1000000u);
                    saveUi();
                    break;
                }
                case 5:
                {
                    bool want = !useGpu;
                    if (applyRenderer(want)) useGpu = want;
                    else applyRenderer(useGpu);
                    gpuTex = 0;
                    saveUi();
                    break;
                }
                case 6:
                {
                    gpuScale = dir ? std::clamp(gpuScale + dir, 1, 3) : gpuScale % 3 + 1;
                    if (useGpu) applyRenderer(true);
                    saveUi();
                    break;
                }
                case 7:
                {
                    threaded3d = !threaded3d;
                    if (!useGpu) applyRenderer(false);
                    saveUi();
                    break;
                }
                case 8: slot = dir ? std::clamp(slot + dir, 1, 4) : slot % 4 + 1; break;
                case 9: if (act) { bool ok = saveState(slot, true); logf("save state slot %d: %s\n", slot, ok ? "ok" : "FAILED"); note = ok ? "État sauvegardé." : "Échec de la sauvegarde."; } break;
                case 10: if (act) { bool ok = loadState(slot); stateLoaded |= ok; logf("load state slot %d: %s\n", slot, ok ? "ok" : "FAILED"); note = ok ? "État chargé." : "Aucun état dans cet emplacement."; if (ok) result = 0; } break;
                case 11: if (act) result = 1; break;
                case 12: if (act) result = 2; break;
                }
            }
        }
        DebugStage = prevStage;
        if (audio) { primeAudio(200); SDL_PauseAudioDevice(audio, 0); }
        return result < 0 ? 2 : result;
    };

    u32 keys = 0;
    bool running = true;
    bool touching = false;
    // touch cursor (docked play, no touch screen): the right stick moves it over the bottom screen, holding the right
    // stick click (R3) touches; it hides 3 s after the last use
    float cursorX = 128, cursorY = 96;
    int stickRX = 0, stickRY = 0;
    u32 stickKeys = 0;   // D-pad keys from the left stick
    // the sticks are read through libnx: SDL sent no axis event at all here (8 controllers opened, 4 axes each), and
    // padInitializeAny merges the handheld mode and the controllers of players 1-8
    PadState stickPad;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&stickPad);
    int cursorShownFrames = 0;
    bool cursorTouching = false;
    std::vector<s16> audioBuf(2048 * 2);

    using clock = std::chrono::steady_clock;
    auto secStart = clock::now();
    int frames = 0;
    double sumMs = 0, maxMs = 0;
    int seconds = 0;
    u64 audioUnderruns = 0, audioDrops = 0;

    // the per-second statistics line is written by a separate thread, which also reads the clocks and the temperature:
    // opening/closing the log on the SD card and these service calls blocked the emulation thread ~5 ms every second
    std::mutex logMutex;
    std::condition_variable logCv;
    std::deque<std::string> logQueue;
    bool logStop = false;
    std::atomic<u32> statClock {0}, statEmc {0}, statGpu {0};
    std::atomic<float> statTemp {0.f};
    auto readStats = [&]() {
        u32 v = 0;
        if (clkOk && R_SUCCEEDED(clkrstGetClockRate(&cpuSession, &v))) statClock = v;
        if (emcOk && R_SUCCEEDED(clkrstGetClockRate(&emcSession, &v))) statEmc = v;
        if (gpuOk && R_SUCCEEDED(clkrstGetClockRate(&gpuSession, &v))) statGpu = v;
        float t = 0;
        if (tsOk && R_SUCCEEDED(tsSessionGetTemperature(&tsSession, &t))) statTemp = t;
    };
    readStats();
    std::thread logThread([&]() {
        std::unique_lock<std::mutex> lock(logMutex);
        for (;;)
        {
            logCv.wait(lock, [&] { return logStop || !logQueue.empty(); });
            while (!logQueue.empty())
            {
                std::string line = std::move(logQueue.front());
                logQueue.pop_front();
                lock.unlock();
                logf("%s", line.c_str());
                readStats();
                lock.lock();
            }
            if (logStop) break;
        }
    });
    auto logAsync = [&](const char* fmt, auto... a) {
        char buf[1024];
        snprintf(buf, sizeof(buf), fmt, a...);
        {
            std::lock_guard<std::mutex> lock(logMutex);
            logQueue.emplace_back(buf);
        }
        logCv.notify_one();
    };

    primeAudio(250);
    while (running && appletMainLoop())
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_JOYBUTTONDOWN)
            {
                int b = ev.jbutton.button;
                if (b < 32) pad |= 1u << b;
                if ((pad & (1u << 8)) && (pad & (1u << 9)))   // ZL + ZR held: hotkeys, the game gets no buttons
                {
                    if (b == 2)
                    {
                        bool ok = saveState(slot, true);
                        logf("save state slot %d: %s\n", slot, ok ? "ok" : "FAILED");
                        feedback(ok ? 40 : 220, ok ? 200 : 40, 40, slot);
                    }
                    else if (b == 3)
                    {
                        bool ok = loadState(slot);
                        stateLoaded |= ok;
                        logf("load state slot %d: %s\n", slot, ok ? "ok" : "FAILED");
                        if (ok) primeAudio(200);
                        feedback(ok ? 50 : 220, ok ? 120 : 40, ok ? 230 : 40, slot);
                        autoFrameCount = 0;
                    }
                    else if (b == 10)
                    {
                        int choice = runMenu();
                        pad = 0;
                        keys = 0;
                        autoFrameCount = 0;   // the menu and the states it saves/loads stall frames
                        if (cursorTouching) { cursorTouching = false; if (!touching) nds->ReleaseScreen(); }
                        if (choice == 1) relaunch = true;
                        if (choice != 0) running = false;
                    }
                    else if (b == 12 || b == 14)
                    {
                        slot = b == 14 ? std::min(slot + 1, 4) : std::max(slot - 1, 1);
                        feedback(230, 200, 40, slot);
                    }
                }
                else
                {
                    keys |= DSKeyForButton(b);
                    if (b == 5) cursorTouching = true;
                }
            }
            else if (ev.type == SDL_JOYBUTTONUP)
            {
                int b = ev.jbutton.button;
                if (b < 32) pad &= ~(1u << b);
                keys &= ~DSKeyForButton(b);
                if (b == 5 && cursorTouching)
                {
                    cursorTouching = false;
                    if (!touching) nds->ReleaseScreen();
                }
            }
            else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION)
            {
                int px = (int)(ev.tfinger.x * 1280), py = (int)(ev.tfinger.y * 720);
                int sx = (px - botR.x) * 256 / botR.w;
                int sy = (py - botR.y) * 192 / botR.h;
                if (sx >= 0 && sx < 256 && sy >= 0 && sy < 192)
                {
                    nds->TouchScreen(sx, sy);
                    touching = true;
                    cursorShownFrames = 0;
                }
            }
            else if (ev.type == SDL_FINGERUP && touching)
            {
                if (!cursorTouching) nds->ReleaseScreen();
                touching = false;
            }
        }

        padUpdate(&stickPad);
        {
            HidAnalogStickState ls = padGetStickPos(&stickPad, 0), rs = padGetStickPos(&stickPad, 1);
            stickRX = rs.x;
            stickRY = -rs.y;   // libnx: up is positive
            stickKeys = (ls.x < -16000 ? 1u << 5 : 0) | (ls.x > 16000 ? 1u << 4 : 0) | (ls.y > 16000 ? 1u << 6 : 0) | (ls.y < -16000 ? 1u << 7 : 0);
        }
        // right stick: dead zone, then a speed that grows with the square of the deflection (up to 3.5 DS pixels per frame)
        auto stickSpeed = [](int v) { float f = (std::abs(v) - 6000) / 26767.f; return f <= 0 ? 0.f : (v < 0 ? -3.5f : 3.5f) * f * f; };
        float cdx = stickSpeed(stickRX), cdy = stickSpeed(stickRY);
        if (cdx != 0 || cdy != 0)
        {
            cursorX = std::clamp(cursorX + cdx, 0.f, 255.f);
            cursorY = std::clamp(cursorY + cdy, 0.f, 191.f);
            cursorShownFrames = 180;
        }
        if (cursorTouching)
        {
            nds->TouchScreen((int)cursorX, (int)cursorY);
            cursorShownFrames = 180;
        }
        else if (cursorShownFrames > 0)
            cursorShownFrames--;

        const u32 quit = (1 << 8) | (1 << 9) | (1 << 3) | (1 << 2); // R L Start Select
        if ((keys & quit) == quit)
            break;

        DebugStage = 2;
        nds->SetKeyMask(((pad & (1u << 8)) && (pad & (1u << 9))) ? 0xFFF : (~(keys | stickKeys) & 0xFFF));

        auto t0 = clock::now();
        nds->RunFrame();
        double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
        frames++;
        DebugFrames++;
        DebugStage = 3;

        void *top, *bottom;
        bool softFrame = nds->GPU.GetFramebuffers(&top, &bottom);   // true = software framebuffers, false = GPU renderer texture
        DebugStage = 4;
        GfxClear(renderer);
        if (softFrame)
        {
            GfxTexUpload(&tex, 0, 0, 256, 192, top);
            GfxTexUpload(&tex, 0, 192, 256, 192, bottom);
            gpuTex = 0;
        }
        else if (top)
            gpuTex = *(GLuint*)top;
        drawScreens();
        if (noteTex.Id && clock::now() < noteUntil)
        {
            SDL_Rect d {16, 720 - noteTex.H - 8, noteTex.W, noteTex.H};
            GfxBlendMode(renderer, true);
            GfxCopy(renderer, &noteTex, nullptr, &d, 2);
            GfxBlendMode(renderer, false);
        }
        if (showFps && fpsTex.Id)
        {
            SDL_Rect d {1280 - fpsTex.W - 16, 8, fpsTex.W, fpsTex.H};
            GfxBlendMode(renderer, true);
            GfxCopy(renderer, &fpsTex, nullptr, &d, 2);
            GfxBlendMode(renderer, false);
        }
        if (clock::now() < fbUntil)
        {
            GfxColor(renderer, fbR, fbG, fbB, 255);
            for (int i = 0; i < std::max(fbSlot, 1); i++)
            {
                SDL_Rect r {40 + i * 48, 40, fbSlot == 0 ? 16 : 36, 36};
                GfxFill(renderer, &r);
            }
        }
        if (cursorShownFrames > 0)
        {
            // cross: black outline, white (red while touching)
            int cx = botR.x + (int)cursorX * botR.w / 256 + botR.w / 512, cy = botR.y + (int)cursorY * botR.h / 192 + botR.h / 384;
            SDL_Rect outline[2] {{cx - 12, cy - 3, 25, 7}, {cx - 3, cy - 12, 7, 25}};
            SDL_Rect inner[2] {{cx - 10, cy - 1, 21, 3}, {cx - 1, cy - 10, 3, 21}};
            GfxColor(renderer, 0, 0, 0, 255);
            for (const SDL_Rect& r : outline) GfxFill(renderer, &r);
            GfxColor(renderer, 255, cursorTouching ? 60 : 255, cursorTouching ? 60 : 255, 255);
            for (const SDL_Rect& r : inner) GfxFill(renderer, &r);
        }
        GfxPresent(renderer);

        DebugStage = 5;
        // drain the SPU every frame. The DS runs at 59.83 fps and the display at 60: instead of dropping audio
        // (clicks), nudge the SPU output rate so that the SDL queue stays around 100 ms.
        if (audio)
        {
            if (SDL_GetQueuedAudioSize(audio) == 0) audioUnderruns++;
            double queuedMs = SDL_GetQueuedAudioSize(audio) / 4.0 * 1000.0 / audioRate;
            if (audioFixedSkew > 0)
                audioSkew = audioFixedSkew;
            else if (audioCtl == 0)
            {
                double err = std::clamp((queuedMs - 100.0) / 100.0, -1.0, 1.0);
                // queue too full: drop a little audio (+0.4% at most); queue running dry (emulation slower than real time):
                // stretch the audio instead of letting it underrun (pitch drops a little, no crackling)
                audioSkew = std::max(1.0029 + (err >= 0 ? 0.004 * err : 0.5 * err), 0.7);
            }
            else
            {
                // same law, applied to the queue level averaged over ~20 frames: SDL drains its queue in 1024-sample
                // blocks, so the raw level is a 21 ms sawtooth, which made the rate (pitch) jump by up to ~3% from one
                // frame to the next whenever the emulation ran below 60 fps (simulation: work/sim_audioctl2.py)
                audioQueueAvg = audioQueueAvg < 0 ? queuedMs : audioQueueAvg + (queuedMs - audioQueueAvg) * 0.05;
                double err = std::clamp((audioQueueAvg - 100.0) / 100.0, -1.0, 1.0);
                audioSkew = std::max(1.0029 + (err >= 0 ? 0.004 * err : 0.5 * err), 0.7);
            }
            nds->SPU.SetOutputSkew(audioSkew);
        }
        for (;;)
        {
            int avail = nds->SPU.GetOutputSize();
            if (avail <= 0) break;
            int n = std::min(avail, (int)(audioBuf.size() / 2));
            n = nds->SPU.ReadOutput(audioBuf.data(), n);
            if (n <= 0) break;
            if (audio && SDL_GetQueuedAudioSize(audio) < (u32)audioRate * 4 * 400 / 1000)
            {
                SDL_QueueAudio(audio, audioBuf.data(), n * 2 * sizeof(s16));
                if (!audioDumpDone) audioDump.insert(audioDump.end(), audioBuf.data(), audioBuf.data() + n * 2);
            }
            else
                audioDrops++;
        }

        if (!audioDumpDone)
        {
            audioDumpSkew.push_back((float)audioSkew);
            if (audioDump.size() >= (size_t)audioRate * 2 * audioDumpSec)
            {
                audioDumpDone = true;
                std::string base = std::string(ConfigDir) + "/dump";
                if (FILE* f = fopen((base + ".wav").c_str(), "wb"))
                {
                    u32 bytes = audioDump.size() * 2, rate = audioRate, brate = audioRate * 4, riff = 36 + bytes, fmtLen = 16;
                    u16 pcm = 1, ch = 2, align = 4, bits = 16;
                    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
                    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
                    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
                    fwrite(audioDump.data(), 2, audioDump.size(), f);
                    fclose(f);
                }
                if (FILE* f = fopen((base + "-skew.txt").c_str(), "w"))
                {
                    for (float k : audioDumpSkew) fprintf(f, "%.5f\n", k);
                    fclose(f);
                }
                logf("audio dump written: %u samples, %u frames\n", (unsigned)(audioDump.size() / 2), (unsigned)audioDumpSkew.size());
                std::vector<s16>().swap(audioDump);
            }
        }

        auto now = clock::now();
        if (autoClock && stateLoaded && overclockMHz == 1785)
            autoFastSeconds = 0;   // a state loaded at 1785 MHz restarts the hold time
        if (autoClock)
        {
            autoFrameTimes[autoFrameCount % 31] = now;
            autoFrameCount++;
            // after the increment, the oldest of the last 31 timestamps (30 frame intervals) is at autoFrameCount % 31
            bool behind = DebugFrames > 10 && autoFrameCount >= 31 && now - autoFrameTimes[autoFrameCount % 31] > std::chrono::milliseconds(540);
            if (overclockMHz < 1785 && (behind || stateLoaded))
            {
                overclockMHz = 1785;
                if (clkOk) clkrstSetClockRate(&cpuSession, 1785u * 1000000u);
                if (behind && seconds - autoLoweredAt <= 10) autoHold = std::min(autoHold * 2, 240);
                autoFrameCount = 0;
                autoFastSeconds = 0;
                logAsync("auto clock: %s, CPU raised to 1785 MHz (s%d, next hold %d s)\n", behind ? "game behind" : "state loaded", seconds, autoHold);
            }
        }
        stateLoaded = false;
        if (now - secStart >= std::chrono::seconds(1))
        {
            seconds++;
            {
                u64 t = 0;
                if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, threadGetCurHandle(), TickCountInfo_Total)))
                {
                    double el = std::chrono::duration<double>(now - secStart).count();
                    busyPermille = busyTicks0 ? (int)((t - busyTicks0) / 19200.0 / el) : 0;
                    busyTicks0 = t;
                }
            }
            sectionsTenths = (int)(MelonGL2DSections * 10 / std::max(frames, 1));
            MelonGL2DSections = 0;
            heavySectionSeconds = (useGpu && sectionsTenths >= 80) ? heavySectionSeconds + 1 : 0;
            if (heavySectionSeconds >= 2 && !cfg.count("gpu"))
            {
                heavySectionSeconds = 0;
                if (applyRenderer(false))
                {
                    useGpu = false;
                    gpuTex = 0;
                    saveUi();
                    logAsync("renderer: %d.%d screen sections per frame, switched to software rendering for this game\n", sectionsTenths / 10, sectionsTenths % 10);
                    GfxTexDestroy(&noteTex);
                    if (UIFont)
                        if (SDL_Surface* sf = TTF_RenderUTF8_Blended(UIFont, "Rendu logiciel pour ce jeu (plus rapide ici), réglable dans le menu", SDL_Color {255, 230, 120, 255}))
                        {
                            noteTex = GfxTexFromSurface(sf);
                            SDL_FreeSurface(sf);
                        }
                    noteUntil = now + std::chrono::seconds(5);
                }
                else applyRenderer(true);
            }
            if (showFps && UIFont)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d fps", frames);
                GfxTexDestroy(&fpsTex);
                if (SDL_Surface* sf = TTF_RenderUTF8_Blended(UIFont, buf, SDL_Color {120, 255, 140, 255}))
                {
                    fpsTex = GfxTexFromSurface(sf);
                    SDL_FreeSurface(sf);
                }
            }
            // clocks and temperature are read by the log thread (service calls), values up to 1 s old
            u32 curClock = statClock, emcClock = statEmc, gpuClock = statGpu;
            float temp = statTemp;
            // emulation-loop timing, microseconds per frame (virtual counter ticks at 19.2 MHz)
#if defined(MELONDS_PROF) && defined(__aarch64__)
#define P(c) ((unsigned long long)(MelonProf[c] / 19.2 / std::max(frames, 1)))
#else
#define P(c) 0ull
enum { ProfARM9, ProfARM7, ProfEvtLCD, ProfEvtSPU, ProfEvtOther, ProfCompile, ProfGeom, ProfTimers };
#endif
            logAsync("%d %d %.1f %.1f jit_trans=%llu jit_trans_ms=%llu audio_ms=%u under=%llu drops=%llu skew=%.4f glprof=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu prof=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu faults=%llu clk=%u emc=%u gpu=%u temp=%.1f gprof=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu busy=%d sect=%d\n", seconds, frames, sumMs / frames, maxMs, (unsigned long long)SwitchJitTransitions, (unsigned long long)(SwitchJitTransitionNs / 1000000), audio ? (unsigned)(SDL_GetQueuedAudioSize(audio) / 4 * 1000 / audioRate) : 0u, (unsigned long long)audioUnderruns, (unsigned long long)audioDrops, audioSkew, MelonGLProf[0] / 1000 / std::max(frames, 1), MelonGLProf[1] / 1000 / std::max(frames, 1), MelonGLProf[2] / 1000 / std::max(frames, 1), MelonGLProf[3] / 1000 / std::max(frames, 1), MelonGLProf[4] / 1000 / std::max(frames, 1), MelonGLProf[5] / 1000 / std::max(frames, 1), MelonGLProf[6] / 1000 / std::max(frames, 1), MelonGLProf[8] / 1000 / std::max(frames, 1), MelonGLProf[9] / 1000 / std::max(frames, 1), MelonGLProf[10] / 1000 / std::max(frames, 1), MelonGLProf[11] / 1000 / std::max(frames, 1), MelonGLProf[12] / std::max(frames, 1), MelonGLProf[13] / std::max(frames, 1), MelonGLProf[14] / 1000 / std::max(frames, 1), MelonGLProf[15] / 1000 / std::max(frames, 1), P(ProfARM9), P(ProfARM7), P(ProfEvtLCD), P(ProfEvtSPU), P(ProfEvtOther), P(ProfCompile), P(ProfGeom), P(ProfTimers), (unsigned long long)SwitchFaultCount, curClock / 1000000, emcClock / 1000000, gpuClock / 1000000, temp, P(8), P(9), P(10), P(11), P(12), P(13), P(14), P(15), busyPermille, sectionsTenths);
            if (autoClock && overclockMHz == 1785)
            {
                // a second counts only with headroom: under 60 % of CPU time at 1785 MHz is ~84 % at 1224 MHz (measured ratio
                // ~1.4; at 1224 MHz the game falls behind from ~86 %). Mario Kart DS replay: 52-55 % light parts, 76-85 % heavy
                autoFastSeconds = (frames >= 59 && busyPermille < 600) ? autoFastSeconds + 1 : 0;
                if (autoFastSeconds >= autoHold)
                {
                    overclockMHz = 1224;
                    if (clkOk) clkrstSetClockRate(&cpuSession, 1224u * 1000000u);
                    autoLoweredAt = seconds;
                    autoFrameCount = 0;
                    autoFastSeconds = 0;
                    logAsync("auto clock: %d s at full speed, CPU lowered to 1224 MHz (s%d)\n", autoHold, seconds);
                }
            }
            else if (autoClock && seconds - autoLoweredAt > 60)
                autoHold = 30;
            if (seconds == 2)
            {
                NWindow* nw = nwindowGetDefault();
                logf("window: %d buffers configured (mask 0x%llx), swap interval %u\n", __builtin_popcountll(nw->slots_configured), (unsigned long long)nw->slots_configured, nw->swap_interval);
            }
            if (samplerOn && sampler.Done && !samplerLogged)
            {
                samplerLogged = true;
                logf("sampler: %u samples written, %llu failures\n", (unsigned)(sampler.Samples.size() / 10), (unsigned long long)sampler.Failures);
            }
            memset(MelonGLProf, 0, sizeof(MelonGLProf));
#if defined(MELONDS_PROF) && defined(__aarch64__)
            memset(MelonProf, 0, sizeof(MelonProf));
#endif
            frames = 0; sumMs = 0; maxMs = 0;
            secStart = now;
        }
    }

    if (autosave)
        logf("autosave slot 0: %s\n", saveState(0, false) ? "ok" : "FAILED");
    if (stateWriter.joinable()) stateWriter.join();

    watchdogStop = true;
    watchdog.join();
    logf("=== stop ===\n");
    if (audio) SDL_CloseAudioDevice(audio);
    for (SDL_Joystick* j : joysticks)
        if (j) SDL_JoystickClose(j);
    GfxTexDestroy(&fpsTex);
    if (UIFont) TTF_CloseFont(UIFont);
    TTF_Quit();
    if (samplerOn)
    {
        sampler.Stop = true;
        threadWaitForExit(&samplerThread);
        threadClose(&samplerThread);
    }
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logStop = true;
    }
    logCv.notify_one();
    logThread.join();
    logf("exit step 1\n");
    plExit();
    logf("exit step 2\n");
    GfxTexDestroy(&tex);
    logf("exit step 3\n");
    if (GfxDk) DkGfxShutdown();
    if (gfx.Context) SDL_GL_DeleteContext(gfx.Context);
    logf("exit step 4\n");
    if (gfx.Window) SDL_DestroyWindow(gfx.Window);
    logf("exit step 5\n");
    SDL_Quit();
    logf("exit step 6\n");
    if (clkOk)
    {
        if (oldClock) clkrstSetClockRate(&cpuSession, oldClock);
        clkrstCloseSession(&cpuSession);
    }
    if (emcOk) clkrstCloseSession(&emcSession);
    if (gpuOk)
    {
        if (oldGpuClock) clkrstSetClockRate(&gpuSession, oldGpuClock);
        clkrstCloseSession(&gpuSession);
    }
    if (tsOk) tsSessionClose(&tsSession);
    tsExit();
    logf("exit step 7\n");
    clkrstExit();
    logf("exit step 8\n");
    if (relaunch)
    {
        // come back to this homebrew with the ROM browser open
        std::string line = std::string("\"") + argv[0] + "\" --browse";
        envSetNextLoad(argv[0], line.c_str());
    }
    logf("exit: main returns, emulator destructors next\n");
    return 0;
}
