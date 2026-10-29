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

#ifndef DEKORENDERER_H
#define DEKORENDERER_H

// 3D on the Switch GPU through deko3d: the compute renderer of melonDS (GPU3D_Compute.cpp, same algorithm and
// shaders, generated at 1x by GenComputeShaders) with deko3d resources instead of OpenGL. Its output (raw colours,
// software renderer format) is read back for the software 2D renderer: DekoSoftRenderer = SoftRenderer with this 3D.
// Structure of RenderFrame and of the span setup taken from ComputeRenderer3D; deko3d usage modelled on
// RSDuck's GPU3D_Deko (melonDS switch-new branch, GPL-3.0).

#include <deko3d.h>

#include <vector>

#include "GPU3D.h"
#include "GPU3D_Texcache.h"
#include "GPU_Soft.h"

namespace melonDS
{

class DekoRenderer3D;

class TexcacheDekoLoader
{
public:
    explicit TexcacheDekoLoader(DekoRenderer3D* owner) : Owner(owner) {}
    // handle = image descriptor index (0 = no texture)
    u32 GenerateTexture(u32 width, u32 height, u32 layers);
    void UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data);
    void DeleteTexture(u32 handle);

private:
    DekoRenderer3D* Owner;
};

using TexcacheDeko = Texcache<TexcacheDekoLoader, u32>;

class DekoRenderer3D : public Renderer3D
{
public:
    DekoRenderer3D(melonDS::GPU3D& gpu3D, DkDevice device);
    ~DekoRenderer3D() override;
    bool Init() override;
    void Reset() override;

    void RenderFrame() override;
    void RestartFrame() override {}
    u32* GetLine(int line) override;

    // texture cache back end (TexcacheDekoLoader)
    u32 AllocTextureArray(u32 width, u32 height, u32 layers);
    void UploadTextureLayer(u32 handle, u32 width, u32 height, u32 layer, const void* data);
    void FreeTextureArray(u32 handle);

private:
    struct SpanSetupY
    {
        // Attributes
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        // Interpolator
        s32 I0, I1;
        s32 Linear;
        s32 IRecip;
        s32 W0n, W0d, W1d;

        // Slope
        s32 Increment;

        s32 X0, X1, Y0, Y1;
        s32 XMin, XMax;
        s32 DxInitial;

        s32 XCovIncr;
        u32 IsDummy;
    };
    struct SpanSetupX
    {
        s32 X0, X1;

        s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;

        s32 XRecip;

        u32 Flags;

        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        s32 CovLInitial, CovRInitial;
    };
    struct SetupIndices
    {
        u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
    };
    struct RenderPolygon
    {
        u32 FirstXSpan;
        s32 YTop, YBot;

        s32 XMin, XMax;
        s32 XMinY, XMaxY;

        u32 Variant;
        u32 Attr;

        float TextureLayer;
    };

    // 1x only: these match GenComputeShaders
    static constexpr int ScreenWidth = 256, ScreenHeight = 192;
    static constexpr int TileSize = 8;
    static constexpr int CoarseTileCountX = 8, CoarseTileCountY = 4;
    static constexpr int CoarseTileW = CoarseTileCountX * TileSize, CoarseTileH = CoarseTileCountY * TileSize;
    static constexpr int ClearCoarseBinMaskLocalSize = 64;
    static constexpr int TilesPerLine = ScreenWidth / TileSize, TileLines = ScreenHeight / TileSize;
    static constexpr int MaxWorkTiles = TilesPerLine * TileLines * 16;

    static constexpr int BinStride = 2048/32;
    static constexpr int CoarseBinStride = BinStride/32;
    static constexpr int MaxVariants = 256;

    struct BinResultHeader
    {
        u32 VariantWorkCount[MaxVariants*4];
        u32 SortedWorkOffset[MaxVariants];

        u32 SortWorkWorkCount[4];
    };

    static const int MaxYSpanSetups = 6144*2;
    static const int MaxYSpanIndices = 64*2048;
    std::vector<SetupIndices> YSpanIndices;
    SpanSetupY YSpanSetups[MaxYSpanSetups];
    RenderPolygon RenderPolygons[2048];

    struct MetaUniform
    {
        u32 NumPolygons;
        u32 NumVariants;

        u32 AlphaRef;
        u32 DispCnt;

        u32 ToonTable[4*34];

        u32 ClearColor, ClearDepth, ClearAttr;

        u32 FogOffset, FogShift, FogColor;

        float ClearBitmapOffset[2];
    };
    struct RasteriseUniform
    {
        u32 CurVariant;
        u32 Pad;
        float InvTextureSize[2];
        s32 TexIsCapture;
        float CaptureYOffset;
    };

    TexcacheDeko Texcache;

    u32* ClearBitmap[2];
    u8 ClearBitmapDirty;

    // deko3d
    DkDevice Device;
    DkQueue Queue = nullptr;
    DkCmdBuf Cmd = nullptr;
    DkFence Fence;
    bool Pending = false;           // a frame was submitted and its result not read back yet
    bool Broken = false;            // the GPU did not finish a frame in time: 3D disabled (empty), the app stays usable

    DkMemBlock CodeMem = nullptr;   // the 33 shaders
    DkMemBlock CmdMem = nullptr;
    DkMemBlock DataMem = nullptr;   // CPU-written: polygons, Y spans, setup indices, uniforms, descriptors, staging
    DkMemBlock GpuMem = nullptr;    // GPU-only buffers
    DkMemBlock ImageMem = nullptr;  // clear bitmaps, final image
    DkMemBlock ReadbackMem = nullptr;

    enum
    {
        shInterpXSpansZ, shInterpXSpansW, shBinCombined, shDepthBlendZ, shDepthBlendW,
        shRasteriseNoTextureZ, shRasteriseNoTextureW, shRasteriseNoTextureToonZ, shRasteriseNoTextureToonW,
        shRasteriseNoTextureHighlightZ, shRasteriseNoTextureHighlightW, shRasteriseUseTextureDecalZ, shRasteriseUseTextureDecalW,
        shRasteriseUseTextureModulateZ, shRasteriseUseTextureModulateW, shRasteriseUseTextureToonZ, shRasteriseUseTextureToonW,
        shRasteriseUseTextureHighlightZ, shRasteriseUseTextureHighlightW, shRasteriseShadowMaskZ, shRasteriseShadowMaskW,
        shClearCoarseBinMask, shClearIndirectWorkCount, shCalculateWorkOffsets, shSortWork,
        shFinalPass, // 8 variants: | 1 edge marking, | 2 fog, | 4 antialiasing
        shCount = shFinalPass + 8
    };
    DkShader Shaders[shCount];

    // image descriptors: fixed ones, then the texture cache arrays
    enum
    {
        descYSpanIndices,
        descClearBitmap0,
        descClearBitmap1,
        descFinalFB,
        descTexcacheStart,
    };
    static constexpr int MaxDescriptors = 1024;
    DkImage YSpanIndicesImage, ClearBitmapImage[2], FinalFBImage;

    struct TexArray
    {
        bool Used = false;
        DkMemBlock Mem = nullptr;
        DkImage Image;
    };
    std::vector<TexArray> TexArrays;   // index + descTexcacheStart = descriptor

    // DataMem layout
    u32 OffYSpanSetups, OffRenderPolygons, OffYSpanIndices, OffMeta, OffRasterise, OffImgDesc, OffSmpDesc, OffStaging;
    u32 StagingSize, StagingUsed = 0;
    // GpuMem layout
    u32 OffXSpanSetups, OffBinResult, OffWorkDesc, OffTiles[3], OffFinalTiles;
    u32 SizeXSpanSetups, SizeBinResult, SizeWorkDesc, SizeTiles, SizeFinalTiles;
    u32 SizeGpuMem;

    alignas(16) u32 Line[256];

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2]);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2]);

    bool WaitPending(const char* where);
    bool Checkpoint(const char* pass);
public:
    // diagnosis (config dk3dsync=N): submit and wait after each pass of the first N frames, log which one hangs
    static int DebugSync;
private:
    void BeginCommands();
    void SubmitAndWait();
    void* StagingAlloc(u32 size, DkGpuAddr& gpuAddr);
    void WriteDescriptor(int index, const DkImage& image, bool loadStore);
    DkGpuAddr DataGpu(u32 off) const { return dkMemBlockGetGpuAddr(DataMem) + off; }
    u8* DataCpu(u32 off) const { return (u8*)dkMemBlockGetCpuAddr(DataMem) + off; }
    DkGpuAddr GpuAddr(u32 off) const { return dkMemBlockGetGpuAddr(GpuMem) + off; }
};

// software renderer (2D, captures, master brightness) with the 3D rendered by DekoRenderer3D
class DekoSoftRenderer : public SoftRenderer
{
public:
    DekoSoftRenderer(melonDS::NDS& nds, DkDevice device);
    bool Init() override { return Rend3D->Init(); }

    // SoftRenderer's versions assume a SoftRenderer3D (render thread)
    void PreSavestate() override {}
    void PostSavestate() override {}
    void SetRenderSettings(RendererSettings& settings) override {}
};

}

#endif
