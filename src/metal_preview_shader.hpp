// Shared native preview/material shaders; see docs/native-rendering.md.
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
float4 preview_color(Varying v, texture2d<float> image,
                                constant State& state) {
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
float4 material_color(Varying v, texture2d<float> image,
                                 constant State& state) {
    float4 color=v.color;
    if(state.mode.z) {
        int2 p=int2(floor(v.uv*1024.0f));
        p.x=state.mode.x==2 ? clamp(p.x,int(state.region.x),int(state.region.y)) :
                             ((p.x & int(state.region.x)) | int(state.region.y));
        p.y=state.mode.y==2 ? clamp(p.y,int(state.region.z),int(state.region.w)) :
                             ((p.y & int(state.region.z)) | int(state.region.w));
        if(any(p<0)||p.x>=int(image.get_width())||p.y>=int(image.get_height()))
            return float4(1,0,1,1);
        float4 t=image.read(uint2(p));
        float a=floor(t.a*127.5f+0.5f);
        if(state.mode.z&2u)a=all(t.rgb==float3(0))?0.0f:128.0f;
        t=float4(t.rgb*255.0f,a);
        color=floor(t*color/128.0f);
    }
    color=clamp(floor(color),0.0f,255.0f);
    // 0x185fb0: TEST 0x5060d (>96, depth write), then 0x50607
    // (<=96, ZBUF masked). The split applies to final modulated alpha.
    // Ordinary opaque-list TEST0x5000d uses one alpha>0, depth-writing pass.
    if(state.mode.w==3) {if(color.a<=0.0f)discard_fragment();}
    else if(state.mode.w==1 ? color.a<=96.0f : color.a>96.0f)discard_fragment();
    float alpha=color.a/128.0f;
    return float4((color.rgb/255.0f)*alpha,alpha);
}
float4 effect_color(Varying v, texture2d<float> image,
                               constant State& state) {
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
float4 map_effect_color(MapEffectVarying v,texture2d<float> image,
                                   constant State& state) {
    // TEX1=0x60 uses linear filtering. Apply region clamp/repeat to each tap.
    float4 modulated;
    if(!state.mode.z)modulated=clamp(v.color,0.0f,255.0f);
    else {
    float2 texel=v.uv*1024.0f-0.5f;int2 base=int2(floor(texel));float2 f=fract(texel);
    float4 texels[4];
    for(uint i=0;i<4;++i){
        int2 p=base+int2(i&1,i>>1);
        p.x=state.mode.x==2?clamp(p.x,int(state.region.x),int(state.region.y)):
            ((p.x&int(state.region.x))|int(state.region.y));
        p.y=state.mode.y==2?clamp(p.y,int(state.region.z),int(state.region.w)):
            ((p.y&int(state.region.z))|int(state.region.w));
        float4 t=image.read(uint2(p));
        texels[i]=float4(t.rgb*255.0f,floor(t.a*127.5f+0.5f));
    }
    float4 t=mix(mix(texels[0],texels[1],f.x),mix(texels[2],texels[3],f.x),f.y);
    modulated=clamp(floor(t*v.color/128.0f),0.0f,255.0f);
    }
    // GS ALPHA0x48=(Cs-0)*As/128+Cd. Premultiply explicitly so As>128
    // keeps its additive contribution instead of clipping the blend factor.
    if(state.mode.w && modulated.a<=0)discard_fragment(); // TEST0x5000d: alpha>0.
    // Dust ALPHA0x44 adds the destination scaled by1-As/128.
    return float4((modulated.rgb/255.0f)*(modulated.a/128.0f),state.mode.w?modulated.a/128.0f:0);
}

struct FieldVarying {float4 position [[position]]; float4 color [[center_no_perspective]];
    float3 uv [[center_no_perspective]]; float z [[center_no_perspective]]; float field [[flat]];};
struct FieldOutput {float4 color [[color(0)]];float depth [[depth(any)]];};
vertex FieldVarying field_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    const auto a=v[id];
    const bool field=a.uv.w>0;
    const float q=field?a.position.z:1.0f/a.position.w;
    return {field?float4(a.position.xy,0,1):a.position,a.color,
        float3(field?a.uv.xy:a.uv.xy*q,q),a.uv.z,a.uv.w};
}

fragment float4 preview_fragment(Varying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {return preview_color(v,image,state);}
fragment FieldOutput field_preview_fragment(FieldVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {
    return {preview_color(Varying{v.position,v.color,v.uv.xy/v.uv.z},image,state),v.field>0?1.0f-floor(v.z)/16777216.0f:v.position.z};
}

fragment float4 material_fragment(Varying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {return material_color(v,image,state);}
fragment FieldOutput field_material_fragment(FieldVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {
    return {material_color(Varying{v.position,v.color,v.uv.xy/v.uv.z},image,state),v.field>0?1.0f-floor(v.z)/16777216.0f:v.position.z};
}

fragment float4 effect_fragment(Varying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {return effect_color(v,image,state);}
fragment FieldOutput field_effect_fragment(FieldVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {
    return {effect_color(Varying{v.position,v.color,v.uv.xy/v.uv.z},image,state),v.field>0?1.0f-floor(v.z)/16777216.0f:v.position.z};
}

fragment float4 map_effect_fragment(MapEffectVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {return map_effect_color(v,image,state);}
fragment FieldOutput field_map_effect_fragment(FieldVarying v [[stage_in]],texture2d<float> image [[texture(0)]],
    constant State& state [[buffer(0)]]) {
    return {map_effect_color(MapEffectVarying{v.position,v.color,v.uv.xy/v.uv.z},image,state),v.field>0?1.0f-floor(v.z)/16777216.0f:v.position.z};
}
)metal";
