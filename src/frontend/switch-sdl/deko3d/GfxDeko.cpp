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

#include "GfxDeko.h"

#include <switch.h>
#include <deko3d.h>

#include <map>
#include <string.h>
#include <vector>

#include "gfx_shaders.h"   // generated: compiled .dksh of gfx_vsh.glsl / gfx_fsh.glsl (uam)

namespace
{

constexpr int FbNum = 3;                     // like the Mesa window: 3 buffers, swap interval 1
constexpr int FbW = 1280, FbH = 720;
constexpr int Slots = FbNum;                 // command memory / staging / deferred frees, per frame in flight
constexpr u32 CmdSize = 512 * 1024;
constexpr u32 StagingSize = 4 * 1024 * 1024;
constexpr u32 TexPoolSize = 48 * 1024 * 1024;
constexpr int MaxTex = 512;

struct QuadUniforms
{
    float Rect[4];
    float UV[4];
    float Color[4];
    s32 Mode[4];
};

struct Tex
{
    bool Used = false;
    bool Linear = false;
    int W = 0, H = 0;
    u32 Offset = 0, Size = 0;
    DkImage Image;
};

struct State
{
    DkDevice Device = nullptr;
    DkQueue Queue = nullptr;
    DkMemBlock FbMem = nullptr, CodeMem = nullptr, CmdMem = nullptr, DataMem = nullptr, TexMem = nullptr;
    DkImage Fb[FbNum];
    DkSwapchain Swapchain = nullptr;
    DkShader Vsh, Fsh;
    DkCmdBuf Cmd = nullptr;
    DkFence Fences[Slots];
    bool Submitted[Slots] {};
    std::vector<int> PendingFree[Slots];

    // DataMem layout: uniform buffer, image descriptors, sampler descriptors, staging (one area per slot)
    u32 UboOff = 0, ImgDescOff = 0, SmpDescOff = 0, StagingOff = 0;

    Tex Textures[MaxTex];
    std::map<u32, u32> FreeRanges;   // texture pool: offset -> size

    int Slot = 0;
    bool FrameOpen = false;
    bool TargetBound = false;
    int FbSlot = -1;
    u32 StagingUsed = 0;
    int Blend = -1;
} S;

DkGpuAddr DataGpu(u32 off) { return dkMemBlockGetGpuAddr(S.DataMem) + off; }
u8* DataCpu(u32 off) { return (u8*)dkMemBlockGetCpuAddr(S.DataMem) + off; }

u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

void LoadShader(DkShader* sh, u32& codeOff, const u8* code, u32 size)
{
    memcpy((u8*)dkMemBlockGetCpuAddr(S.CodeMem) + codeOff, code, size);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, S.CodeMem, codeOff);
    dkShaderInitialize(sh, &sm);
    codeOff = AlignUp(codeOff + size, DK_SHADER_CODE_ALIGNMENT);
}

// texture pool: first fit, neighbours merged on free
bool PoolAlloc(u32 size, u32 align, u32& off)
{
    for (auto it = S.FreeRanges.begin(); it != S.FreeRanges.end(); ++it)
    {
        u32 start = AlignUp(it->first, align);
        if (start + size > it->first + it->second) continue;
        u32 rStart = it->first, rEnd = it->first + it->second;
        S.FreeRanges.erase(it);
        if (start > rStart) S.FreeRanges[rStart] = start - rStart;
        if (rEnd > start + size) S.FreeRanges[start + size] = rEnd - (start + size);
        off = start;
        return true;
    }
    return false;
}

void PoolFree(u32 off, u32 size)
{
    auto it = S.FreeRanges.emplace(off, size).first;
    auto next = std::next(it);
    if (next != S.FreeRanges.end() && it->first + it->second == next->first)
    {
        it->second += next->second;
        S.FreeRanges.erase(next);
    }
    if (it != S.FreeRanges.begin())
    {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first)
        {
            prev->second += it->second;
            S.FreeRanges.erase(it);
        }
    }
}

void ReleaseTex(int id)
{
    Tex& t = S.Textures[id];
    if (!t.Used) return;
    PoolFree(t.Offset, t.Size);
    t = Tex();
}

void WriteImageDescriptor(int id)
{
    DkImageView view;
    dkImageViewDefaults(&view, &S.Textures[id].Image);
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, false, false);
    memcpy(DataCpu(S.ImgDescOff) + id * sizeof(DkImageDescriptor), &desc, sizeof(desc));
}

// starts recording the next frame: waits until the GPU is done with this slot's memory
void EnsureFrame()
{
    if (S.FrameOpen) return;
    S.Slot = (S.Slot + 1) % Slots;
    if (S.Submitted[S.Slot])
    {
        dkFenceWait(&S.Fences[S.Slot], -1);
        S.Submitted[S.Slot] = false;
    }
    for (int id : S.PendingFree[S.Slot]) ReleaseTex(id);
    S.PendingFree[S.Slot].clear();
    dkCmdBufClear(S.Cmd);
    dkCmdBufAddMemory(S.Cmd, S.CmdMem, S.Slot * CmdSize, CmdSize);
    S.StagingUsed = 0;
    S.FrameOpen = true;
    S.TargetBound = false;
    S.Blend = -1;
    // previous frames' draws are complete before this frame's copies overwrite a texture; new descriptors are seen
    dkCmdBufBarrier(S.Cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    dkCmdBufBindImageDescriptorSet(S.Cmd, DataGpu(S.ImgDescOff), MaxTex);
    dkCmdBufBindSamplerDescriptorSet(S.Cmd, DataGpu(S.SmpDescOff), 2);
}

void EnsureTarget()
{
    EnsureFrame();
    if (S.TargetBound) return;
    S.FbSlot = dkQueueAcquireImage(S.Queue, S.Swapchain);
    DkImageView view;
    dkImageViewDefaults(&view, &S.Fb[S.FbSlot]);
    dkCmdBufBindRenderTarget(S.Cmd, &view, nullptr);
    DkViewport vp {0.f, 0.f, (float)FbW, (float)FbH, 0.f, 1.f};
    DkScissor sc {0, 0, FbW, FbH};
    dkCmdBufSetViewports(S.Cmd, 0, &vp, 1);
    dkCmdBufSetScissors(S.Cmd, 0, &sc, 1);
    DkShader const* shaders[] = {&S.Vsh, &S.Fsh};
    dkCmdBufBindShaders(S.Cmd, DkStageFlag_GraphicsMask, shaders, 2);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(S.Cmd, &rs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(S.Cmd, &cw);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);   // src alpha, 1 - src alpha
    dkCmdBufBindBlendState(S.Cmd, 0, &bs);
    dkCmdBufBindUniformBuffer(S.Cmd, DkStage_Vertex, 0, DataGpu(S.UboOff), DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindUniformBuffer(S.Cmd, DkStage_Fragment, 0, DataGpu(S.UboOff), DK_UNIFORM_BUF_ALIGNMENT);
    S.TargetBound = true;
}

void SubmitFrame(bool present)
{
    DkCmdList list = dkCmdBufFinishList(S.Cmd);
    dkQueueSubmitCommands(S.Queue, list);
    dkQueueSignalFence(S.Queue, &S.Fences[S.Slot], false);
    S.Submitted[S.Slot] = true;
    if (present && S.TargetBound)
        dkQueuePresentImage(S.Queue, S.Swapchain, S.FbSlot);
    else
        dkQueueFlush(S.Queue);
    S.FrameOpen = false;
    S.TargetBound = false;
}

}

bool DkGfxInit(const std::function<void(const std::string&)>& say)
{
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    S.Device = dkDeviceCreate(&dm);
    if (!S.Device) { say("deko3d: no device"); return false; }

    // framebuffers + swapchain on the default window (no EGL surface may exist)
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, S.Device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FbW;
    lm.dimensions[1] = FbH;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &lm);
    u32 fbSize = AlignUp(dkImageLayoutGetSize(&fbLayout), dkImageLayoutGetAlignment(&fbLayout));
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, S.Device, AlignUp(FbNum * fbSize, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    S.FbMem = dkMemBlockCreate(&mm);
    DkImage const* fbs[FbNum];
    for (int i = 0; i < FbNum; i++)
    {
        dkImageInitialize(&S.Fb[i], &fbLayout, S.FbMem, i * fbSize);
        fbs[i] = &S.Fb[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, S.Device, nwindowGetDefault(), fbs, FbNum);
    S.Swapchain = dkSwapchainCreate(&sm);
    if (!S.Swapchain) { say("deko3d: no swapchain"); return false; }
    dkSwapchainSetSwapInterval(S.Swapchain, 1);

    // shaders
    dkMemBlockMakerDefaults(&mm, S.Device, AlignUp(GfxVshDkshSize + GfxFshDkshSize + 2 * DK_SHADER_CODE_ALIGNMENT + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    S.CodeMem = dkMemBlockCreate(&mm);
    u32 codeOff = 0;
    LoadShader(&S.Vsh, codeOff, GfxVshDksh, GfxVshDkshSize);
    LoadShader(&S.Fsh, codeOff, GfxFshDksh, GfxFshDkshSize);

    // command memory, one area per frame in flight
    dkMemBlockMakerDefaults(&mm, S.Device, Slots * CmdSize);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    S.CmdMem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, S.Device);
    S.Cmd = dkCmdBufCreate(&cm);

    // uniforms, descriptors, staging
    S.UboOff = 0;
    S.ImgDescOff = DK_UNIFORM_BUF_ALIGNMENT;
    S.SmpDescOff = AlignUp(S.ImgDescOff + MaxTex * sizeof(DkImageDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
    S.StagingOff = AlignUp(S.SmpDescOff + 2 * sizeof(DkSamplerDescriptor), 0x100);
    dkMemBlockMakerDefaults(&mm, S.Device, AlignUp(S.StagingOff + Slots * StagingSize, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    S.DataMem = dkMemBlockCreate(&mm);
    memset(DataCpu(0), 0, S.StagingOff);
    for (int i = 0; i < 2; i++)   // 0 nearest, 1 linear; clamped
    {
        DkSampler smp;
        dkSamplerDefaults(&smp);
        smp.minFilter = smp.magFilter = i ? DkFilter_Linear : DkFilter_Nearest;
        smp.wrapMode[0] = smp.wrapMode[1] = smp.wrapMode[2] = DkWrapMode_ClampToEdge;
        DkSamplerDescriptor sd;
        dkSamplerDescriptorInitialize(&sd, &smp);
        memcpy(DataCpu(S.SmpDescOff) + i * sizeof(sd), &sd, sizeof(sd));
    }

    // texture pool
    dkMemBlockMakerDefaults(&mm, S.Device, TexPoolSize);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    S.TexMem = dkMemBlockCreate(&mm);
    S.FreeRanges.clear();
    S.FreeRanges[0] = TexPoolSize;

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, S.Device);
    qm.flags = DkQueueFlags_Graphics;
    S.Queue = dkQueueCreate(&qm);
    if (!S.Queue) { say("deko3d: no queue"); return false; }
    say("deko3d: device, 3 x 1280x720 swapchain, quad shaders ready");
    return true;
}

void* DkGfxDevice()
{
    return S.Device;
}

void DkGfxShutdown()
{
    if (!S.Device) return;
    if (S.FrameOpen) SubmitFrame(false);
    dkQueueWaitIdle(S.Queue);
    dkQueueDestroy(S.Queue);
    dkCmdBufDestroy(S.Cmd);
    dkSwapchainDestroy(S.Swapchain);
    for (DkMemBlock b : {S.TexMem, S.DataMem, S.CmdMem, S.CodeMem, S.FbMem})
        if (b) dkMemBlockDestroy(b);
    dkDeviceDestroy(S.Device);
    S = State();
}

int DkTexCreate(int w, int h, bool linear)
{
    int id = -1;
    for (int i = 0; i < MaxTex; i++)
        if (!S.Textures[i].Used) { id = i; break; }
    if (id < 0 || w <= 0 || h <= 0) return -1;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, S.Device);
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    u32 size = (u32)dkImageLayoutGetSize(&layout), off;
    if (!PoolAlloc(size, dkImageLayoutGetAlignment(&layout), off)) return -1;
    Tex& t = S.Textures[id];
    t.Used = true;
    t.Linear = linear;
    t.W = w;
    t.H = h;
    t.Offset = off;
    t.Size = size;
    dkImageInitialize(&t.Image, &layout, S.TexMem, off);
    // the slot is free: no frame in flight reads this descriptor (frees are deferred until their frame completes)
    WriteImageDescriptor(id);
    return id;
}

void DkTexSetLinear(int id, bool linear)
{
    if (id >= 0 && id < MaxTex) S.Textures[id].Linear = linear;
}

void DkTexUpload(int id, int x, int y, int w, int h, const void* pixels, int pitch)
{
    if (id < 0 || id >= MaxTex || !S.Textures[id].Used) return;
    EnsureFrame();
    u32 bytes = (u32)w * h * 4;
    u32 off = AlignUp(S.StagingUsed, 0x100);
    if (off + bytes > StagingSize) return;   // more than 4 MB of uploads in one frame: dropped
    u8* dst = DataCpu(S.StagingOff + S.Slot * StagingSize + off);
    if (pitch == w * 4)
        memcpy(dst, pixels, bytes);
    else
        for (int row = 0; row < h; row++)
            memcpy(dst + row * w * 4, (const u8*)pixels + row * pitch, w * 4);
    S.StagingUsed = off + bytes;
    DkCopyBuf src {DataGpu(S.StagingOff + S.Slot * StagingSize + off), 0, 0};
    DkImageView view;
    dkImageViewDefaults(&view, &S.Textures[id].Image);
    DkImageRect rect {(u32)x, (u32)y, 0, (u32)w, (u32)h, 1};
    dkCmdBufCopyBufferToImage(S.Cmd, &src, &view, &rect, 0);
    dkCmdBufBarrier(S.Cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
}

void DkTexDestroy(int id)
{
    if (id < 0 || id >= MaxTex || !S.Textures[id].Used) return;
    EnsureFrame();
    S.PendingFree[S.Slot].push_back(id);
}

void DkClear()
{
    EnsureTarget();
    dkCmdBufClearColorFloat(S.Cmd, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);
}

void DkDraw(const float rect[4], const float uv[4], const float color[4], int mode, int tex, bool blend)
{
    EnsureTarget();
    if ((int)blend != S.Blend)
    {
        DkColorState cs;
        dkColorStateDefaults(&cs);
        dkColorStateSetBlendEnable(&cs, 0, blend);
        dkCmdBufBindColorState(S.Cmd, &cs);
        S.Blend = blend;
    }
    if (tex >= 0 && tex < MaxTex && S.Textures[tex].Used)
        dkCmdBufBindTexture(S.Cmd, DkStage_Fragment, 0, dkMakeTextureHandle(tex, S.Textures[tex].Linear ? 1 : 0));
    else if (mode != 0)
        return;
    QuadUniforms u;
    memcpy(u.Rect, rect, sizeof(u.Rect));
    memcpy(u.UV, uv, sizeof(u.UV));
    memcpy(u.Color, color, sizeof(u.Color));
    u.Mode[0] = mode;
    u.Mode[1] = u.Mode[2] = u.Mode[3] = 0;
    dkCmdBufPushConstants(S.Cmd, DataGpu(S.UboOff), DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof(u), &u);
    dkCmdBufDraw(S.Cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void DkPresent()
{
    EnsureTarget();
    SubmitFrame(true);
}
