#version 460
layout (std140, binding = 0) uniform Quad
{
    vec4 uRect;
    vec4 uUV;
    vec4 uColor;
    ivec4 uMode;
};
layout (binding = 0) uniform sampler2D uTex;
layout (location = 0) in vec2 vUV;
layout (location = 0) out vec4 oColor;
void main()
{
    if (uMode.x == 0) oColor = uColor;                                // flat colour
    else if (uMode.x == 1) oColor = vec4(texture(uTex, vUV).bgr, 1.0); // DS software frame (BGRA bytes)
    else oColor = texture(uTex, vUV);                                 // text (RGBA)
}
