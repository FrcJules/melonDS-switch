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

#include "DekoRenderer.h"

#include <switch.h>

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>

#include "NDS.h"
#include "Platform.h"
#include "compute_shaders.h"
#include "GfxDeko.h"   // generated: Cs<name>[] / Cs<name>Size, see GenComputeShaders.cpp

namespace melonDS
{

namespace
{

u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

constexpr u32 CmdSize = 1024 * 1024;
constexpr u32 SamplerCount = 9;   // wrap S x wrap T: clamp, repeat, mirrored repeat
constexpr u32 SamplerRepeat = 1 + 1 * 3;

struct ShaderBlob { const u8* Code; u32 Size; };

}

u32 TexcacheDekoLoader::GenerateTexture(u32 width, u32 height, u32 layers)
{
    return Owner->AllocTextureArray(width, height, layers);
}

void TexcacheDekoLoader::UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data)
{
    Owner->UploadTextureLayer(handle, width, height, layer, data);
}

void TexcacheDekoLoader::DeleteTexture(u32 handle)
{
    Owner->FreeTextureArray(handle);
}

DekoRenderer3D::DekoRenderer3D(melonDS::GPU3D& gpu3D, DkDevice device)
    : Renderer3D(gpu3D), Texcache(gpu3D.GPU, TexcacheDekoLoader(this)), Device(device)
{
    ClearBitmap[0] = new u32[256*256];
    ClearBitmap[1] = new u32[256*256];
    ClearBitmapDirty = 0x3;
    YSpanIndices.resize(MaxYSpanIndices);
}

bool DekoRenderer3D::Init()
{
    if (Queue) return true;
    if (!Device) return false;

    DkMemBlockMaker mm;

    // shaders
    const ShaderBlob blobs[shCount] =
    {
        {CsInterpXSpansZ, CsInterpXSpansZSize}, {CsInterpXSpansW, CsInterpXSpansWSize},
        {CsBinCombined, CsBinCombinedSize},
        {CsDepthBlendZ, CsDepthBlendZSize}, {CsDepthBlendW, CsDepthBlendWSize},
        {CsRasteriseNoTextureZ, CsRasteriseNoTextureZSize}, {CsRasteriseNoTextureW, CsRasteriseNoTextureWSize},
        {CsRasteriseNoTextureToonZ, CsRasteriseNoTextureToonZSize}, {CsRasteriseNoTextureToonW, CsRasteriseNoTextureToonWSize},
        {CsRasteriseNoTextureHighlightZ, CsRasteriseNoTextureHighlightZSize}, {CsRasteriseNoTextureHighlightW, CsRasteriseNoTextureHighlightWSize},
        {CsRasteriseUseTextureDecalZ, CsRasteriseUseTextureDecalZSize}, {CsRasteriseUseTextureDecalW, CsRasteriseUseTextureDecalWSize},
        {CsRasteriseUseTextureModulateZ, CsRasteriseUseTextureModulateZSize}, {CsRasteriseUseTextureModulateW, CsRasteriseUseTextureModulateWSize},
        {CsRasteriseUseTextureToonZ, CsRasteriseUseTextureToonZSize}, {CsRasteriseUseTextureToonW, CsRasteriseUseTextureToonWSize},
        {CsRasteriseUseTextureHighlightZ, CsRasteriseUseTextureHighlightZSize}, {CsRasteriseUseTextureHighlightW, CsRasteriseUseTextureHighlightWSize},
        {CsRasteriseShadowMaskZ, CsRasteriseShadowMaskZSize}, {CsRasteriseShadowMaskW, CsRasteriseShadowMaskWSize},
        {CsClearCoarseBinMask, CsClearCoarseBinMaskSize}, {CsClearIndirectWorkCount, CsClearIndirectWorkCountSize},
        {CsCalculateWorkOffsets, CsCalculateWorkOffsetsSize}, {CsSortWork, CsSortWorkSize},
        {CsFinalPass, CsFinalPassSize}, {CsFinalPassEdge, CsFinalPassEdgeSize},
        {CsFinalPassFog, CsFinalPassFogSize}, {CsFinalPassEdgeFog, CsFinalPassEdgeFogSize},
        {CsFinalPassAA, CsFinalPassAASize}, {CsFinalPassAAEdge, CsFinalPassAAEdgeSize},
        {CsFinalPassAAFog, CsFinalPassAAFogSize}, {CsFinalPassAAEdgeFog, CsFinalPassAAEdgeFogSize},
    };
    u32 codeSize = DK_SHADER_CODE_UNUSABLE_SIZE;
    for (const ShaderBlob& b : blobs) codeSize += AlignUp(b.Size, DK_SHADER_CODE_ALIGNMENT);
    dkMemBlockMakerDefaults(&mm, Device, AlignUp(codeSize, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    CodeMem = dkMemBlockCreate(&mm);
    if (!CodeMem) return false;
    u32 codeOff = 0;
    for (int i = 0; i < shCount; i++)
    {
        memcpy((u8*)dkMemBlockGetCpuAddr(CodeMem) + codeOff, blobs[i].Code, blobs[i].Size);
        DkShaderMaker sm;
        dkShaderMakerDefaults(&sm, CodeMem, codeOff);
        dkShaderInitialize(&Shaders[i], &sm);
        codeOff += AlignUp(blobs[i].Size, DK_SHADER_CODE_ALIGNMENT);
    }

    // CPU-written data
    u32 off = 0;
    OffYSpanSetups = off;    off = AlignUp(off + sizeof(SpanSetupY) * MaxYSpanSetups, 0x100);
    OffRenderPolygons = off; off = AlignUp(off + sizeof(RenderPolygon) * 2048, 0x100);
    OffMeta = off;           off = AlignUp(off + sizeof(MetaUniform), DK_UNIFORM_BUF_ALIGNMENT);
    OffRasterise = off;      off = AlignUp(off + sizeof(RasteriseUniform), DK_UNIFORM_BUF_ALIGNMENT);
    OffImgDesc = off;        off = AlignUp(off + sizeof(DkImageDescriptor) * MaxDescriptors, 0x100);
    OffSmpDesc = off;        off = AlignUp(off + sizeof(DkSamplerDescriptor) * SamplerCount, 0x100);
    OffStaging = off;
    StagingSize = 16 * 1024 * 1024;
    off += StagingSize;
    OffYSpanIndices = 0; // unused: setup indices go through the staging area into their buffer image
    dkMemBlockMakerDefaults(&mm, Device, AlignUp(off, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DataMem = dkMemBlockCreate(&mm);
    if (!DataMem) return false;
    memset(DataCpu(0), 0, OffStaging);

    // GPU-only buffers (CPU access only to clear them once)
    SizeXSpanSetups = sizeof(SpanSetupX) * MaxYSpanIndices;
    SizeBinResult = sizeof(BinResultHeader)
        + TilesPerLine*TileLines*CoarseBinStride*4 // BinnedMaskCoarse
        + TilesPerLine*TileLines*BinStride*4 // BinnedMask
        + TilesPerLine*TileLines*BinStride*4; // WorkOffsets
    SizeWorkDesc = MaxWorkTiles*2*4*2;
    SizeTiles = 4*TileSize*TileSize*MaxWorkTiles;
    SizeFinalTiles = 4*3*2*ScreenWidth*ScreenHeight;
    off = 0;
    OffXSpanSetups = off; off = AlignUp(off + SizeXSpanSetups, 0x100);
    OffBinResult = off;   off = AlignUp(off + SizeBinResult, 0x100);
    OffWorkDesc = off;    off = AlignUp(off + SizeWorkDesc, 0x100);
    for (int i = 0; i < 3; i++) { OffTiles[i] = off; off = AlignUp(off + SizeTiles, 0x100); }
    OffFinalTiles = off;  off = AlignUp(off + SizeFinalTiles, 0x100);
    SizeGpuMem = AlignUp(off, DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&mm, Device, SizeGpuMem);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    GpuMem = dkMemBlockCreate(&mm);
    if (!GpuMem) return false;
    memset(dkMemBlockGetCpuAddr(GpuMem), 0, SizeGpuMem);

    // images: setup indices (buffer image), clear bitmaps, final image
    DkImageLayoutMaker lm;
    DkImageLayout yspanLayout, clearLayout, finalLayout;
    dkImageLayoutMakerDefaults(&lm, Device);
    lm.type = DkImageType_Buffer;
    lm.format = DkImageFormat_RGBA16_Uint;
    lm.dimensions[0] = MaxYSpanIndices;
    dkImageLayoutInitialize(&yspanLayout, &lm);
    dkImageLayoutMakerDefaults(&lm, Device);
    lm.format = DkImageFormat_R32_Uint;
    lm.dimensions[0] = 256;
    lm.dimensions[1] = 256;
    dkImageLayoutInitialize(&clearLayout, &lm);
    dkImageLayoutMakerDefaults(&lm, Device);
    lm.flags = DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_R32_Uint;
    lm.dimensions[0] = ScreenWidth;
    lm.dimensions[1] = ScreenHeight;
    dkImageLayoutInitialize(&finalLayout, &lm);
    u32 offY = 0;
    u32 offC0 = AlignUp(offY + (u32)dkImageLayoutGetSize(&yspanLayout), dkImageLayoutGetAlignment(&clearLayout));
    u32 offC1 = AlignUp(offC0 + (u32)dkImageLayoutGetSize(&clearLayout), dkImageLayoutGetAlignment(&clearLayout));
    u32 offF = AlignUp(offC1 + (u32)dkImageLayoutGetSize(&clearLayout), dkImageLayoutGetAlignment(&finalLayout));
    u32 imageSize = AlignUp(offF + (u32)dkImageLayoutGetSize(&finalLayout), DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&mm, Device, imageSize);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    ImageMem = dkMemBlockCreate(&mm);
    if (!ImageMem) return false;
    dkImageInitialize(&YSpanIndicesImage, &yspanLayout, ImageMem, offY);
    dkImageInitialize(&ClearBitmapImage[0], &clearLayout, ImageMem, offC0);
    dkImageInitialize(&ClearBitmapImage[1], &clearLayout, ImageMem, offC1);
    dkImageInitialize(&FinalFBImage, &finalLayout, ImageMem, offF);
    WriteDescriptor(descYSpanIndices, YSpanIndicesImage, true);
    WriteDescriptor(descClearBitmap0, ClearBitmapImage[0], false);
    WriteDescriptor(descClearBitmap1, ClearBitmapImage[1], false);
    WriteDescriptor(descFinalFB, FinalFBImage, true);

    for (u32 j = 0; j < 3; j++)
    {
        for (u32 i = 0; i < 3; i++)
        {
            const DkWrapMode translateWrapMode[3] = {DkWrapMode_ClampToEdge, DkWrapMode_Repeat, DkWrapMode_MirroredRepeat};
            DkSampler smp;
            dkSamplerDefaults(&smp);
            smp.wrapMode[0] = translateWrapMode[i];
            smp.wrapMode[1] = translateWrapMode[j];
            DkSamplerDescriptor sd;
            dkSamplerDescriptorInitialize(&sd, &smp);
            memcpy(DataCpu(OffSmpDesc) + (i + j*3) * sizeof(sd), &sd, sizeof(sd));
        }
    }

    // read back of the final image (raw colours, 256x192)
    dkMemBlockMakerDefaults(&mm, Device, AlignUp(ScreenWidth * ScreenHeight * 4, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    ReadbackMem = dkMemBlockCreate(&mm);
    if (!ReadbackMem) return false;
    memset(dkMemBlockGetCpuAddr(ReadbackMem), 0, ScreenWidth * ScreenHeight * 4);

    dkMemBlockMakerDefaults(&mm, Device, CmdSize);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    CmdMem = dkMemBlockCreate(&mm);
    if (!CmdMem) return false;
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, Device);
    Cmd = dkCmdBufCreate(&cm);

    // the presentation queue (see DkGfxQueue)
    Queue = (DkQueue)DkGfxQueue();
    if (!Queue || !Cmd) return false;

    TexArrays.clear();

    return true;
}

DekoRenderer3D::~DekoRenderer3D()
{
    if (Queue) WaitPending("destructor");
    if (Broken)
    {
        // the GPU may still be using this memory: leak it rather than free it under the GPU
        delete[] ClearBitmap[0];
        delete[] ClearBitmap[1];
        return;
    }
    Texcache.Reset();
    for (TexArray& a : TexArrays)
        if (a.Mem) dkMemBlockDestroy(a.Mem);
    if (Cmd) dkCmdBufDestroy(Cmd);
    for (DkMemBlock b : {CmdMem, ReadbackMem, ImageMem, GpuMem, DataMem, CodeMem})
        if (b) dkMemBlockDestroy(b);
    delete[] ClearBitmap[0];
    delete[] ClearBitmap[1];
}

// dkQueueSignalFence(..., flush = true) does NOT start the queue on this firmware (measured on the console with the
// official compute example: such a fence was never signalled, 0 out of 120; with the queue flushed explicitly it
// was signalled 120 out of 120). Signal without the flag, then flush.
void DekoRenderer3D::SignalFence()
{
    dkQueueSignalFence(Queue, &Fence, false);
    dkQueueFlush(Queue);
}

int DekoRenderer3D::DebugSync = 0;

// diagnosis: runs what was recorded up to this pass and waits for it (GPU state persists between command lists).
// The last "submitted" line without its "done" in the log is the pass that hangs (the frontend watchdog then ends
// the process).
bool DekoRenderer3D::Checkpoint(const char* pass)
{
    if (!DebugSync) return true;
    dkQueueSubmitCommands(Queue, dkCmdBufFinishList(Cmd));
    SignalFence();
    Platform::Log(Platform::LogLevel::Warn, "deko3d 3D: pass '%s' submitted\n", pass);
    DkResult r = dkFenceWait(&Fence, 1000000000LL);
    Platform::Log(Platform::LogLevel::Warn, "deko3d 3D: pass '%s' %s\n", pass, r == DkResult_Success ? "done" : "TIMEOUT");
    return true;
}

// bounded wait: a GPU hang disables the 3D instead of freezing the app (the frontend watchdog is the last resort)
bool DekoRenderer3D::WaitPending(const char* where)
{
    if (!Pending) return !Broken;
    if (dkFenceWait(&Fence, 1000000000LL) != DkResult_Success)
    {
        Platform::Log(Platform::LogLevel::Error, "deko3d 3D: fence wait failed (%s), 3D disabled\n", where);
        Broken = true;
        DkGfxMarkBroken();
    }
    Pending = false;
    return !Broken;
}

void DekoRenderer3D::Reset()
{
    if (!Broken) WaitPending("reset");
    if (!Broken) Texcache.Reset();
    ClearBitmapDirty = 0x3;
}

void DekoRenderer3D::WriteDescriptor(int index, const DkImage& image, bool loadStore)
{
    DkImageView view;
    dkImageViewDefaults(&view, &image);
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, loadStore, false);
    memcpy(DataCpu(OffImgDesc) + index * sizeof(DkImageDescriptor), &desc, sizeof(desc));
}

void DekoRenderer3D::BeginCommands()
{
    dkCmdBufClear(Cmd);
    dkCmdBufAddMemory(Cmd, CmdMem, 0, CmdSize);
    StagingUsed = 0;
    Checkpoint("empty list");
    // descriptors may have been (re)written by the CPU, staging data too
    dkCmdBufBarrier(Cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    Checkpoint("barrier image/descriptors");
    dkCmdBufBarrier(Cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    Checkpoint("barrier L2 cache");
    dkCmdBufBindImageDescriptorSet(Cmd, DataGpu(OffImgDesc), MaxDescriptors);
    dkCmdBufBindSamplerDescriptorSet(Cmd, DataGpu(OffSmpDesc), SamplerCount);
    Checkpoint("descriptor sets");
}

// runs what was recorded so far and waits for it (staging area full), then goes on recording
void DekoRenderer3D::SubmitAndWait()
{
    dkQueueSubmitCommands(Queue, dkCmdBufFinishList(Cmd));
    SignalFence();
    Pending = true;
    WaitPending("staging area full");
    BeginCommands();
}

void* DekoRenderer3D::StagingAlloc(u32 size, DkGpuAddr& gpuAddr)
{
    u32 off = AlignUp(StagingUsed, 0x100);
    if (off + size > StagingSize)
    {
        SubmitAndWait();
        off = 0;
        if (size > StagingSize) return nullptr;
    }
    StagingUsed = off + size;
    gpuAddr = DataGpu(OffStaging + off);
    return DataCpu(OffStaging + off);
}

u32 DekoRenderer3D::AllocTextureArray(u32 width, u32 height, u32 layers)
{
    size_t idx = 0;
    while (idx < TexArrays.size() && TexArrays[idx].Used) idx++;
    if (idx + descTexcacheStart >= (size_t)MaxDescriptors)
    {
        printf("deko3d: out of texture descriptors\n");
        abort();
    }
    if (idx == TexArrays.size()) TexArrays.emplace_back();
    TexArray& a = TexArrays[idx];

    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, Device);
    lm.type = DkImageType_2DArray;
    lm.format = DkImageFormat_RGBA8_Uint;
    lm.dimensions[0] = width;
    lm.dimensions[1] = height;
    lm.dimensions[2] = layers;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, Device, AlignUp((u32)dkImageLayoutGetSize(&layout), DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    a.Mem = dkMemBlockCreate(&mm);
    if (!a.Mem)
    {
        printf("deko3d: out of memory for textures\n");
        abort();
    }
    dkImageInitialize(&a.Image, &layout, a.Mem, 0);
    a.Used = true;
    WriteDescriptor(descTexcacheStart + (int)idx, a.Image, false);
    // seen by the GPU after the next descriptor invalidation (BeginCommands, or right here when recording)
    dkCmdBufBarrier(Cmd, DkBarrier_Full, DkInvalidateFlags_Descriptors);
    return descTexcacheStart + (u32)idx;
}

void DekoRenderer3D::UploadTextureLayer(u32 handle, u32 width, u32 height, u32 layer, const void* data)
{
    TexArray& a = TexArrays[handle - descTexcacheStart];
    DkGpuAddr src;
    void* dst = StagingAlloc(width * height * 4, src);
    if (!dst) return;
    memcpy(dst, data, width * height * 4);
    DkCopyBuf copySrc {src, 0, 0};
    DkImageView view;
    dkImageViewDefaults(&view, &a.Image);
    DkImageRect rect {0, 0, layer, width, height, 1};
    dkCmdBufCopyBufferToImage(Cmd, &copySrc, &view, &rect, 0);
}

void DekoRenderer3D::FreeTextureArray(u32 handle)
{
    TexArray& a = TexArrays[handle - descTexcacheStart];
    // only called from Texcache.Reset(), when no frame is in flight (Reset waits, the destructor waits)
    if (a.Mem) dkMemBlockDestroy(a.Mem);
    a = TexArray();
}

void DekoRenderer3D::SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void DekoRenderer3D::SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2])
{
    s32 x0 = positions[vertex][0];
    if (side)
    {
        span->DxInitial = -0x40000;
        x0--;
    }
    else
    {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;

    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;

    span->XCovIncr = 0;

    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void DekoRenderer3D::SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2])
{
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    SetupAttrs(span, poly, from, to);

    s32 minXY, maxXY;
    bool negative = false;
    if (span->X1 > span->X0)
    {
        span->XMin = span->X0;
        span->XMax = span->X1-1;

        minXY = span->Y0;
        maxXY = span->Y1;
    }
    else if (span->X1 < span->X0)
    {
        span->XMin = span->X1;
        span->XMax = span->X0-1;
        negative = true;

        minXY = span->Y1;
        maxXY = span->Y0;
    }
    else
    {
        span->XMin = span->X0;
        if (side) span->XMin--;
        span->XMax = span->XMin;

        // doesn't matter for completely vertical slope
        minXY = span->Y0;
        maxXY = span->Y0;
    }

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = minXY;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = maxXY;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax+1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    // TODO: this is still not perfect (see for example x=169 y=33)
    if (ylen == 0)
    {
        span->Increment = 0;
    }
    else if (ylen == xlen)
    {
        span->Increment = 0x40000;
    }
    else
    {
        s32 yrecip = (1<<18) / ylen;
        span->Increment = (span->X1-span->X0) * yrecip;
        if (span->Increment < 0) span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side)
    {
        // right

        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    }
    else
    {
        // left

        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor)
    {
        if (side)
        {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        }
        else
        {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }

        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    }
    else
    {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    if (span->I0 != span->I1)
        span->IRecip = (1<<30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1))
    {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    }
    else
    {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

namespace
{

struct Variant
{
    u32 Texture, Sampler;
    u16 Width, Height;
    u8 BlendMode;

    bool operator==(const Variant& other)
    {
        return Texture == other.Texture && Sampler == other.Sampler && BlendMode == other.BlendMode;
    }
};

}

void DekoRenderer3D::RenderFrame()
{
    // the previous frame must be done: its memory is reused (normally already read back by GetLine)
    if (!WaitPending("render") || Broken) return;

    BeginCommands();
    if (!Checkpoint("start")) return;

    u8 clrBitmapDirty;
    if (!Texcache.Update(clrBitmapDirty) && GPU3D.RenderFrameIdentical)
    {
        // same frame: the read back image is still valid
        return;
    }

    // if we're using a clear bitmap, set that up
    ClearBitmapDirty |= clrBitmapDirty;
    if (GPU3D.RenderDispCnt & (1<<14))
    {
        for (int k = 0; k < 2; k++)
        {
            if (!(ClearBitmapDirty & (1<<k))) continue;
            if (k == 0)
            {
                u16* vram = (u16*)&GPU.VRAMFlat_Texture[0x40000];
                for (int i = 0; i < 256*256; i++)
                {
                    u16 color = vram[i];
                    u32 r = (color << 1) & 0x3E; if (r) r++;
                    u32 g = (color >> 4) & 0x3E; if (g) g++;
                    u32 b = (color >> 9) & 0x3E; if (b) b++;
                    u32 a = (color & 0x8000) ? 31 : 0;

                    ClearBitmap[0][i] = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
            else
            {
                u16* vram = (u16*)&GPU.VRAMFlat_Texture[0x60000];
                for (int i = 0; i < 256*256; i++)
                {
                    u16 val = vram[i];
                    u32 depth = ((val & 0x7FFF) * 0x200) + 0x1FF;
                    u32 fog = (val & 0x8000) << 9;

                    ClearBitmap[1][i] = depth | fog;
                }
            }
            DkGpuAddr src;
            void* dst = StagingAlloc(256*256*4, src);
            if (!dst) continue;
            memcpy(dst, ClearBitmap[k], 256*256*4);
            DkCopyBuf copySrc {src, 0, 0};
            DkImageView view;
            dkImageViewDefaults(&view, &ClearBitmapImage[k]);
            DkImageRect rect {0, 0, 0, 256, 256, 1};
            dkCmdBufCopyBufferToImage(Cmd, &copySrc, &view, &rect, 0);
        }

        ClearBitmapDirty = 0;
    }
    if (!Checkpoint("clear bitmaps")) return;

    int numYSpans = 0;
    int numSetupIndices = 0;

    // see ComputeRenderer3D::RenderFrame: textures of the same size share array textures, polygons are
    // grouped into variants (shader + texture + sampler). Display captures need no special case here:
    // the software 2D renderer writes them to VRAM, which the texture cache reads.
    u32 numVariants = 0, prevVariant = 0, prevTexLayer = 0;
    Variant variants[MaxVariants];

    bool enableTextureMaps = GPU3D.RenderDispCnt & (1<<0);

    for (int i = 0; i < GPU3D.RenderNumPolygons; i++)
    {
        Polygon* polygon = GPU3D.RenderPolygonRAM[i];

        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;

        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;

        RenderPolygons[i].FirstXSpan = numSetupIndices;
        RenderPolygons[i].Attr = polygon->Attr;

        bool foundVariant = false;
        if (i > 0)
        {
            // if the whole texture attribute matches
            // the texture layer will also match
            Polygon* prevPolygon = GPU3D.RenderPolygonRAM[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam
                && prevPolygon->TexPalette == polygon->TexPalette
                && (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30)
                && prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }

        if (!foundVariant)
        {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            u32* textureLastVariant = nullptr;
            // we always need to look up the texture to get the layer of the array texture
            u32 textype = (polygon->TexParam >> 26) & 0x7;
            if (enableTextureMaps && textype)
            {
                Texcache.GetTexture(polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer, textureLastVariant);

                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = (wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3;

                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant)
                {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }

            if (!foundVariant)
            {
                for (int j = numVariants - 1; j >= 0; j--)
                {
                    if (variants[j] == variant)
                    {
                        foundVariant = true;
                        prevVariant = j;
                        goto foundVariant;
                    }
                }

                prevVariant = numVariants;
                variants[numVariants] = variant;
                variants[numVariants].Width = TextureWidth(polygon->TexParam);
                variants[numVariants].Height = TextureHeight(polygon->TexParam);
                numVariants++;
                assert(numVariants <= MaxVariants);
            foundVariant:;

                if (textureLastVariant)
                    *textureLastVariant = prevVariant;
            }
        }
        RenderPolygons[i].Variant = prevVariant;
        RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView)
        {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        }
        else
        {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }

        // 1x, native coordinates
        s32 scaledPositions[10][2];
        s32 ytop = ScreenHeight, ybot = 0;
        for (int i = 0; i < polygon->NumVertices; i++)
        {
            scaledPositions[i][0] = polygon->Vertices[i]->FinalPosition[0];
            scaledPositions[i][1] = polygon->Vertices[i]->FinalPosition[1];
            ytop = std::min(scaledPositions[i][1], ytop);
            ybot = std::max(scaledPositions[i][1], ybot);
        }
        RenderPolygons[i].YTop = ytop;
        RenderPolygons[i].YBot = ybot;
        RenderPolygons[i].XMin = ScreenWidth;
        RenderPolygons[i].XMax = 0;

        if (ybot == ytop)
        {
            vtop = 0; vbot = 0;

            RenderPolygons[i].YBot++;

            int j = 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            j = nverts - 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanL = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vtop, 0, scaledPositions);
            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanR = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vbot, 1, scaledPositions);

            YSpanIndices[numSetupIndices].PolyIdx = i;
            YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
            YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
            YSpanIndices[numSetupIndices].Y = ytop;
            numSetupIndices++;
        }
        else
        {
            u32 curSpanL = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);

            for (u32 y = ytop; y < ybot; y++)
            {
                if (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                    {
                        curVL = nextVL;
                        if (polygon->FacingView)
                        {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts)
                                nextVL = 0;
                        }
                        else
                        {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0)
                                nextVL = nverts - 1;
                        }
                    }


                    assert(numYSpans < MaxYSpanSetups);
                    curSpanL = numYSpans;
                    SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
                }
                if (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                    {
                        curVR = nextVR;
                        if (polygon->FacingView)
                        {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0)
                                nextVR = nverts - 1;
                        }
                        else
                        {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts)
                                nextVR = 0;
                        }
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanR = numYSpans;
                    SetupYSpan(&RenderPolygons[i] ,&YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);
                }

                YSpanIndices[numSetupIndices].PolyIdx = i;
                YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                YSpanIndices[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }
    }

    if (!Checkpoint("texture cache")) return;
    if (numYSpans > 0)
    {
        memcpy(DataCpu(OffYSpanSetups), YSpanSetups, sizeof(SpanSetupY)*numYSpans);
        memcpy(DataCpu(OffRenderPolygons), RenderPolygons, GPU3D.RenderNumPolygons*sizeof(RenderPolygon));

        DkGpuAddr src;
        void* dst = StagingAlloc(numSetupIndices*4*2, src);
        if (dst)
        {
            memcpy(dst, YSpanIndices.data(), numSetupIndices*4*2);
            dkCmdBufCopyBuffer(Cmd, src, dkMemBlockGetGpuAddr(ImageMem) + 0 /* YSpanIndicesImage at offset 0 */, numSetupIndices*4*2);
        }
    }
    if (!Checkpoint("setup indices copy")) return;
    // texture uploads, clear bitmaps and setup indices are complete before the compute passes read them
    dkCmdBufBarrier(Cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    if (!Checkpoint("uploads")) return;

    // bind everything (same slots as the GL version)
    DkBufExtents sbufs[8] =
    {
        {DataGpu(OffRenderPolygons), (u32)(sizeof(RenderPolygon) * 2048)},  // 0
        {GpuAddr(OffXSpanSetups), SizeXSpanSetups},                         // 1
        {DataGpu(OffYSpanSetups), (u32)(sizeof(SpanSetupY) * MaxYSpanSetups)}, // 2 (tiles colour while rasterising)
        {GpuAddr(OffTiles[1]), SizeTiles},                                  // 3
        {GpuAddr(OffTiles[2]), SizeTiles},                                  // 4
        {GpuAddr(OffFinalTiles), SizeFinalTiles},                           // 5
        {GpuAddr(OffBinResult), SizeBinResult},                             // 6
        {GpuAddr(OffWorkDesc), SizeWorkDesc},                               // 7
    };
    dkCmdBufBindStorageBuffers(Cmd, DkStage_Compute, 0, sbufs, 8);

    MetaUniform meta;
    meta.DispCnt = GPU3D.RenderDispCnt;
    meta.NumPolygons = GPU3D.RenderNumPolygons;
    meta.NumVariants = numVariants;
    meta.AlphaRef = GPU3D.RenderAlphaRef;
    {
        u32 r = (GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = GPU3D.RenderClearAttr1 & 0x3F008000;

        u8 xoff = (GPU3D.RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (GPU3D.RenderClearAttr2 >> 24) & 0xFF;
        meta.ClearBitmapOffset[0] = (float)xoff / 256.0;
        meta.ClearBitmapOffset[1] = (float)yoff / 256.0;
    }
    for (u32 i = 0; i < 32; i++)
    {
        u32 color = GPU3D.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++)
    {
        meta.ToonTable[i*4+1] = GPU3D.RenderFogDensityTable[i];
    }
    for (u32 i = 0; i < 8; i++)
    {
        u32 color = GPU3D.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = GPU3D.RenderFogOffset;
    meta.FogShift = GPU3D.RenderFogShift;
    {
        u32 fogR = (GPU3D.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (GPU3D.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (GPU3D.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }

    const u32 metaSize = AlignUp(sizeof(MetaUniform), DK_UNIFORM_BUF_ALIGNMENT);
    const u32 rastSize = AlignUp(sizeof(RasteriseUniform), DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindUniformBuffer(Cmd, DkStage_Compute, 0, DataGpu(OffMeta), metaSize);
    dkCmdBufBindUniformBuffer(Cmd, DkStage_Compute, 1, DataGpu(OffRasterise), rastSize);
    dkCmdBufPushConstants(Cmd, DataGpu(OffMeta), metaSize, 0, sizeof(MetaUniform), &meta);

    auto useShader = [this](int idx) {
        DkShader const* sh = &Shaders[idx];
        dkCmdBufBindShaders(Cmd, DkStageFlag_Compute, &sh, 1);
    };

    useShader(shClearCoarseBinMask);
    dkCmdBufDispatchCompute(Cmd, TilesPerLine*TileLines/ClearCoarseBinMaskLocalSize, 1, 1);
    if (!Checkpoint("ClearCoarseBinMask")) return;

    bool wbuffer = false;
    if (numYSpans > 0)
    {
        wbuffer = GPU3D.RenderPolygonRAM[0]->WBuffer;
        const DkGpuAddr binResult = GpuAddr(OffBinResult);

        useShader(shClearIndirectWorkCount);
        dkCmdBufDispatchCompute(Cmd, (numVariants+31)/32, 1, 1);
        if (!Checkpoint("ClearIndirectWorkCount")) return;

        // calculate x-spans
        dkCmdBufBindImage(Cmd, DkStage_Compute, 0, dkMakeImageHandle(descYSpanIndices));
        useShader(wbuffer ? shInterpXSpansW : shInterpXSpansZ);
        dkCmdBufDispatchCompute(Cmd, (numSetupIndices + 31) / 32, 1, 1);
        dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);
        if (!Checkpoint("InterpXSpans")) return;

        // bin polygons
        useShader(shBinCombined);
        dkCmdBufDispatchCompute(Cmd, ((GPU3D.RenderNumPolygons + 31) / 32), ScreenWidth/CoarseTileW, ScreenHeight/CoarseTileH);
        dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);
        if (!Checkpoint("BinCombined")) return;

        // calculate list offsets
        useShader(shCalculateWorkOffsets);
        dkCmdBufDispatchCompute(Cmd, (numVariants + 31) / 32, 1, 1);
        dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);
        if (!Checkpoint("CalculateWorkOffsets")) return;

        // sort shader work
        useShader(shSortWork);
        dkCmdBufDispatchComputeIndirect(Cmd, binResult + offsetof(BinResultHeader, SortWorkWorkCount));
        dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);
        if (!Checkpoint("SortWork")) return;

        // tile buffers in slots 2-4 while rasterising
        DkBufExtents tiles[3] =
        {
            {GpuAddr(OffTiles[0]), SizeTiles},
            {GpuAddr(OffTiles[1]), SizeTiles},
            {GpuAddr(OffTiles[2]), SizeTiles},
        };
        dkCmdBufBindStorageBuffers(Cmd, DkStage_Compute, 2, tiles, 3);

        // rasterise
        {
            bool highLightMode = GPU3D.RenderDispCnt & (1<<1);
            const int w = wbuffer ? 1 : 0;

            int shadersNoTexture[] =
            {
                shRasteriseNoTextureZ + w,
                shRasteriseNoTextureZ + w,
                (highLightMode ? shRasteriseNoTextureHighlightZ : shRasteriseNoTextureToonZ) + w,
                shRasteriseNoTextureZ + w,
                shRasteriseShadowMaskZ + w
            };
            int shadersUseTexture[] =
            {
                shRasteriseUseTextureModulateZ + w,
                shRasteriseUseTextureDecalZ + w,
                (highLightMode ? shRasteriseUseTextureHighlightZ : shRasteriseUseTextureToonZ) + w,
                shRasteriseUseTextureDecalZ + w,
                shRasteriseShadowMaskZ + w
            };

            int prevShader = -1;
            u32 prevTexture = 0, prevSampler = ~0U;
            for (u32 i = 0; i < numVariants; i++)
            {
                int shader;
                RasteriseUniform ru {};
                ru.CurVariant = i;
                if (variants[i].Texture == 0)
                {
                    shader = shadersNoTexture[variants[i].BlendMode];
                }
                else
                {
                    shader = shadersUseTexture[variants[i].BlendMode];
                    if (variants[i].Texture != prevTexture || variants[i].Sampler != prevSampler)
                    {
                        dkCmdBufBindTexture(Cmd, DkStage_Compute, 0, dkMakeTextureHandle(variants[i].Texture, variants[i].Sampler));
                        prevTexture = variants[i].Texture;
                        prevSampler = variants[i].Sampler;
                    }
                }
                if (shader != prevShader)
                {
                    useShader(shader);
                    prevShader = shader;
                }

                ru.InvTextureSize[0] = 1.f / variants[i].Width;
                ru.InvTextureSize[1] = 1.f / variants[i].Height;
                ru.TexIsCapture = 0;
                ru.CaptureYOffset = 0.f;
                dkCmdBufPushConstants(Cmd, DataGpu(OffRasterise), rastSize, 0, sizeof(RasteriseUniform), &ru);
                dkCmdBufDispatchComputeIndirect(Cmd, binResult + offsetof(BinResultHeader, VariantWorkCount) + i*4*4);
                if (DebugSync && !Checkpoint(i == 0 ? "Rasterise (variant 0)" : "Rasterise")) return;
            }
        }
    }
    dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);

    // compose final image
    {
        DkResHandle clear[2] = {dkMakeTextureHandle(descClearBitmap0, SamplerRepeat), dkMakeTextureHandle(descClearBitmap1, SamplerRepeat)};
        dkCmdBufBindTextures(Cmd, DkStage_Compute, 0, clear, 2);
    }
    useShader(wbuffer ? shDepthBlendW : shDepthBlendZ);
    dkCmdBufDispatchCompute(Cmd, ScreenWidth/TileSize, ScreenHeight/TileSize, 1);
    dkCmdBufBarrier(Cmd, DkBarrier_Primitives, 0);
    if (!Checkpoint("DepthBlend")) return;

    dkCmdBufBindImage(Cmd, DkStage_Compute, 0, dkMakeImageHandle(descFinalFB));
    u32 finalPassShader = 0;
    if (GPU3D.RenderDispCnt & (1<<4))
        finalPassShader |= 0x4;
    if (GPU3D.RenderDispCnt & (1<<7))
        finalPassShader |= 0x2;
    if (GPU3D.RenderDispCnt & (1<<5))
        finalPassShader |= 0x1;
    useShader(shFinalPass + finalPassShader);
    dkCmdBufDispatchCompute(Cmd, ScreenWidth/32, ScreenHeight, 1);
    dkCmdBufBarrier(Cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    if (!Checkpoint("FinalPass")) return;

    // read back (GetLine)
    {
        DkImageView view;
        dkImageViewDefaults(&view, &FinalFBImage);
        DkImageRect rect {0, 0, 0, ScreenWidth, ScreenHeight, 1};
        DkCopyBuf dst {dkMemBlockGetGpuAddr(ReadbackMem), 0, 0};
        dkCmdBufCopyImageToBuffer(Cmd, &view, &rect, &dst, 0);
    }

    dkQueueSubmitCommands(Queue, dkCmdBufFinishList(Cmd));
    SignalFence();
    Pending = true;
    if (DebugSync > 0) DebugSync--;   // checkpoints on the first N frames only
}

u32* DekoRenderer3D::GetLine(int line)
{
    if (GPU3D.AbortFrame)
    {
        memset(Line, 0, sizeof(Line));
        return Line;
    }

    if (!WaitPending("read back"))
    {
        memset(Line, 0, sizeof(Line));
        return Line;
    }

    const u32* rawline = (const u32*)dkMemBlockGetCpuAddr(ReadbackMem) + line * ScreenWidth;
    u16 xpos = GPU3D.RenderXPos;
    if (xpos == 0)
    {
        memcpy(Line, rawline, sizeof(Line));
        return Line;
    }

    // apply X scroll (as SoftRenderer3D::GetLine, the image is 256 pixels wide here)
    if (xpos & 0x100)
    {
        int i = 0, j = xpos;
        for (; j < 512; i++, j++)
            Line[i] = 0;
        for (j = 0; i < 256; i++, j++)
            Line[i] = rawline[j];
    }
    else
    {
        int i = 0, j = xpos;
        for (; j < 256; i++, j++)
            Line[i] = rawline[j];
        for (; i < 256; i++)
            Line[i] = 0;
    }
    return Line;
}

DekoSoftRenderer::DekoSoftRenderer(melonDS::NDS& nds, DkDevice device)
    : SoftRenderer(nds)
{
    Rend3D = std::make_unique<DekoRenderer3D>(GPU.GPU3D, device);
}

}
