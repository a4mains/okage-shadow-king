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
struct MapEffectVarying {float4 position [[position]];float4 color [[center_no_perspective]];float2 uv;};
vertex MapEffectVarying map_effect_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    return {v[id].position,v[id].color,v[id].uv.xy};
}
fragment float4 map_effect_fragment(MapEffectVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
                                   constant State& state [[buffer(0)]]) {
    // TEX1=0x60 uses linear filtering; clamp each tap to the observed region.
    float2 texel=v.uv*1024.0f-0.5f;int2 base=int2(floor(texel));float2 f=fract(texel);
    float4 texels[4];
    for(uint i=0;i<4;++i){
        int2 p=clamp(base+int2(i&1,i>>1),int2(state.region.x,state.region.z),int2(state.region.y,state.region.w));
        float4 t=image.read(uint2(p));
        texels[i]=float4(t.rgb*255.0f,floor(t.a*127.5f+0.5f));
    }
    float4 t=mix(mix(texels[0],texels[1],f.x),mix(texels[2],texels[3],f.x),f.y);
    float4 modulated=clamp(floor(t*v.color/128.0f),0.0f,255.0f);
    // GS ALPHA0x48=(Cs-0)*As/128+Cd. Premultiply explicitly so As>128
    // keeps its additive contribution instead of clipping the blend factor.
    if(state.mode.w && modulated.a<=0)discard_fragment(); // TEST0x5000d: alpha>0.
    // Dust ALPHA0x44 adds the destination scaled by1-As/128.
    return float4((modulated.rgb/255.0f)*(modulated.a/128.0f),state.mode.w?modulated.a/128.0f:0);
}
)metal";
