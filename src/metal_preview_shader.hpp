// Shared provisional unlit material path; see docs/native-rendering.md.
#pragma once
static const char* shader=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position, color, uv; };
struct Varying { float4 position [[position]]; float4 color; float2 uv; };
struct State { uint4 region; uint4 mode; };
vertex Varying preview_vertex(uint id [[vertex_id]], device const Vertex* v [[buffer(0)]]) {
    return {v[id].position, v[id].color, v[id].uv.xy};
}
fragment float4 preview_fragment(Varying v [[stage_in]], texture2d<float> image [[texture(0)]],
                                constant State& state [[buffer(0)]]) {
    float4 color=v.color;
    if (state.mode.z) {
        int2 p=int2(floor(v.uv*1024.0f));
        p.x=state.mode.x==2 ? min(int(state.region.y),max(int(state.region.x),p.x)) :
                             ((p.x & int(state.region.x)) | int(state.region.y));
        p.y=state.mode.y==2 ? min(int(state.region.w),max(int(state.region.z),p.y)) :
                             ((p.y & int(state.region.z)) | int(state.region.w));
        if (any(p<0) || p.x>=int(image.get_width()) || p.y>=int(image.get_height()))
            return float4(1,0,1,1);
        color*=image.read(uint2(p));
    }
    if (color.a<0.5f) discard_fragment();
    return float4(color.rgb,1);
}
fragment float4 effect_fragment(Varying v [[stage_in]], texture2d<float> image [[texture(0)]],
                               constant State& state [[buffer(0)]]) {
    int2 p=int2(floor(v.uv*1024.0f));
    p=clamp(p,int2(state.region.x,state.region.z),int2(state.region.y,state.region.w));
    p=clamp(p,int2(0),int2(image.get_width()-1,image.get_height()-1));
    float4 color=image.read(uint2(p))*v.color*2.0f;
    if(color.a<=0)discard_fragment();
    return clamp(color,0.0f,1.0f);
}
)metal";
