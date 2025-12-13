#pragma once
static const char* gs_material_shader=R"metal(
float4 gs_material_color(Varying v,constant State& state,
    device const uchar* bytes,device const uchar* known,
    constant GsTextureRegistersMetal& regs,device atomic_uint* invalid) {
    float4 color=v.color;
    if(state.mode.z) {
        const float4 sample=gs_sample_texture(bytes,known,regs,v.uv,invalid);
        if(any(!isfinite(sample)))return float4(1,0,1,1); // completion rejects; never presented
        color=floor(sample*color/128.0f);
    }
    color=clamp(floor(color),0.0f,255.0f);
    if(state.mode.w==3) {if(color.a<=0.0f)discard_fragment();}
    else if(state.mode.w==1?color.a<=96.0f:color.a>96.0f)discard_fragment();
    const float alpha=color.a/128.0f;
    return float4((color.rgb/255.0f)*alpha,alpha);
}
fragment FieldOutput field_gs_material_fragment(FieldVarying v [[stage_in]],
    constant State& state [[buffer(0)]],device atomic_uint* invalid [[buffer(1)]],
    device const uchar* bytes [[buffer(2)]],device const uchar* known [[buffer(3)]],
    constant GsTextureRegistersMetal& regs [[buffer(4)]]) {
    return {gs_material_color(Varying{v.position,v.color,v.uv.xy/v.uv.z},state,bytes,known,regs,invalid),
        v.field>0?1.0f-floor(v.z)/16777216.0f:v.position.z};
}
)metal";
