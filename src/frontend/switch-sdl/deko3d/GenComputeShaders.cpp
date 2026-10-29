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

// Host tool (built and run at build time): writes the 33 compute shader variants of the compute renderer
// (src/GPU3D_Compute_shaders.h, the single source) as GLSL 4.60 files for uam, the deko3d shader compiler.
// Same assembly as ComputeRenderer3D::CompileShader, at 1x, with three adaptations for deko3d:
//  - the Rasterise uniforms without a block (layout(location=N) uniform) go into a uniform block (binding 1),
//  - the final pass writes the raw colour (soft renderer format, read back for the software 2D) into an r32ui image,
//  - int64 needs its extension.
// Usage: GenComputeShaders <output directory>

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "../../../GPU3D_Compute_shaders.h"

using namespace melonDS::ComputeRendererShaders;

static std::string OutDir;
static int Failures = 0;

static void Replace(std::string& s, const std::string& from, const std::string& to, const char* name)
{
    size_t pos = s.find(from);
    if (pos == std::string::npos)
    {
        fprintf(stderr, "GenComputeShaders: %s: pattern not found: %.60s\n", name, from.c_str());
        Failures++;
        return;
    }
    s.replace(pos, from.size(), to);
}

static void Emit(const char* name, const std::string& source, std::initializer_list<const char*> defines)
{
    // 1x: same values as ComputeRenderer3D::SetRenderSettings(1, ...)
    const int screenWidth = 256, screenHeight = 192, tileSize = 8;
    const int coarseTileCountY = 4, coarseTileArea = 8 * 4, clearCoarseBinMaskLocalSize = 64;
    const int maxWorkTiles = (screenWidth / tileSize) * (screenHeight / tileSize) * 16;

    std::string s = "#version 460\n#extension GL_ARB_gpu_shader_int64 : require\n";
    for (const char* d : defines)
        s += std::string("#define ") + d + "\n";
    s += "#define ScreenWidth " + std::to_string(screenWidth);
    s += "\n#define ScreenHeight " + std::to_string(screenHeight);
    s += "\n#define MaxWorkTiles " + std::to_string(maxWorkTiles);
    s += "\n#define TileSize " + std::to_string(tileSize);
    s += "\nconst int CoarseTileCountY = " + std::to_string(coarseTileCountY) + ";";
    s += "\n#define CoarseTileArea " + std::to_string(coarseTileArea);
    s += "\n#define ClearCoarseBinMaskLocalSize " + std::to_string(clearCoarseBinMaskLocalSize);
    s += Common;
    s += source;

    if (&source == &Rasterise)
        Replace(s,
            "layout (location = 0) uniform uint CurVariant;\n"
            "layout (location = 1) uniform vec2 InvTextureSize;\n"
            "layout (location = 2) uniform int TexIsCapture;\n"
            "layout (location = 3) uniform float CaptureYOffset;\n",
            "layout (std140, binding = 1) uniform RasteriseUniforms\n"
            "{\n"
            "    uint CurVariant;\n"
            "    vec2 InvTextureSize;\n"
            "    int TexIsCapture;\n"
            "    float CaptureYOffset;\n"
            "};\n", name);
    if (&source == &FinalPass)
    {
        Replace(s, "layout (binding = 0, rgba8) writeonly uniform image2D FinalFB;",
            "layout (binding = 0, r32ui) writeonly uniform uimage2D FinalFB;", name);
        Replace(s,
            "    vec4 result = vec4(color.x & 0x3FU, bitfieldExtract(color.x, 8, 8), bitfieldExtract(color.x, 16, 8), bitfieldExtract(color.x, 24, 8));\n"
            "    result /= vec4(63.0, 63.0, 63.0, 31.0);\n"
            "    imageStore(FinalFB, ivec2(gl_GlobalInvocationID.xy), result);\n",
            "    imageStore(FinalFB, ivec2(gl_GlobalInvocationID.xy), uvec4(color.x, 0U, 0U, 0U));\n", name);
    }

    std::string path = OutDir + "/" + name + ".glsl";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "GenComputeShaders: cannot write %s\n", path.c_str()); Failures++; return; }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: GenComputeShaders <output directory>\n"); return 1; }
    OutDir = argv[1];

    // same list and order as ComputeRenderer3D::ShaderCompileStep
    Emit("InterpXSpansZ", InterpSpans, {"InterpSpans", "ZBuffer"});
    Emit("InterpXSpansW", InterpSpans, {"InterpSpans", "WBuffer"});
    Emit("BinCombined", BinCombined, {"BinCombined"});
    Emit("DepthBlendZ", DepthBlend, {"DepthBlend", "ZBuffer"});
    Emit("DepthBlendW", DepthBlend, {"DepthBlend", "WBuffer"});
    Emit("RasteriseNoTextureZ", Rasterise, {"Rasterise", "ZBuffer", "NoTexture"});
    Emit("RasteriseNoTextureW", Rasterise, {"Rasterise", "WBuffer", "NoTexture"});
    Emit("RasteriseNoTextureToonZ", Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Toon"});
    Emit("RasteriseNoTextureToonW", Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Toon"});
    Emit("RasteriseNoTextureHighlightZ", Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Highlight"});
    Emit("RasteriseNoTextureHighlightW", Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Highlight"});
    Emit("RasteriseUseTextureDecalZ", Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Decal"});
    Emit("RasteriseUseTextureDecalW", Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Decal"});
    Emit("RasteriseUseTextureModulateZ", Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Modulate"});
    Emit("RasteriseUseTextureModulateW", Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Modulate"});
    Emit("RasteriseUseTextureToonZ", Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Toon"});
    Emit("RasteriseUseTextureToonW", Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Toon"});
    Emit("RasteriseUseTextureHighlightZ", Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Highlight"});
    Emit("RasteriseUseTextureHighlightW", Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Highlight"});
    Emit("RasteriseShadowMaskZ", Rasterise, {"Rasterise", "ZBuffer", "ShadowMask"});
    Emit("RasteriseShadowMaskW", Rasterise, {"Rasterise", "WBuffer", "ShadowMask"});
    Emit("ClearCoarseBinMask", ClearCoarseBinMask, {"ClearCoarseBinMask"});
    Emit("ClearIndirectWorkCount", ClearIndirectWorkCount, {"ClearIndirectWorkCount"});
    Emit("CalculateWorkOffsets", CalcOffsets, {"CalculateWorkOffsets"});
    Emit("SortWork", SortWork, {"SortWork"});
    Emit("FinalPass", FinalPass, {"FinalPass"});
    Emit("FinalPassEdge", FinalPass, {"FinalPass", "EdgeMarking"});
    Emit("FinalPassFog", FinalPass, {"FinalPass", "Fog"});
    Emit("FinalPassEdgeFog", FinalPass, {"FinalPass", "EdgeMarking", "Fog"});
    Emit("FinalPassAA", FinalPass, {"FinalPass", "AntiAliasing"});
    Emit("FinalPassAAEdge", FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking"});
    Emit("FinalPassAAFog", FinalPass, {"FinalPass", "AntiAliasing", "Fog"});
    Emit("FinalPassAAEdgeFog", FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking", "Fog"});
    return Failures ? 1 : 0;
}
