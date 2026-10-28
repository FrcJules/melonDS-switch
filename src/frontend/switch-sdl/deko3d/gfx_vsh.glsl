#version 460
// screen-space quad (triangle strip, 4 vertices), 1280x720 pixels from the top left (NDC y = +1 is the top row)
layout (std140, binding = 0) uniform Quad
{
    vec4 uRect;   // x, y, w, h in pixels
    vec4 uUV;     // u0, v0, u1, v1
    vec4 uColor;
    ivec4 uMode;  // x: 0 flat colour, 1 DS software frame, 2 RGBA texture
};
layout (location = 0) out vec2 vUV;
void main()
{
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUV = mix(uUV.xy, uUV.zw, p);
    vec2 px = uRect.xy + p * uRect.zw;
    gl_Position = vec4(px.x / 640.0 - 1.0, 1.0 - px.y / 360.0, 0.0, 1.0);
}
