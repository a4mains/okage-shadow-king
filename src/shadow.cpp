#include "okage/shadow.hpp"
#include <bit>
#include <algorithm>
#include <limits>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct ProjectionRounding {
    int old=std::fegetround();
    ProjectionRounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set shadow projection rounding");}
    ~ProjectionRounding(){std::fesetround(old);}
};
Float4 shadow_transform(const Matrix4& m,const Float4& v) {
    Float4 result{};
    for(unsigned row=0;row<4;++row) {
        float f=m[0][row]*v[0];f=f+m[1][row]*v[1];f=f+m[2][row]*v[2];
        result[row]=f+m[3][row]*v[3];
    }
    return result;
}
Matrix4 shadow_multiply(const Matrix4& a,const Matrix4& b) {
    Matrix4 result;
    for(unsigned col=0;col<4;++col)result[col]=shadow_transform(a,b[col]);
    return result;
}
Float4 shadow_normalize(const Float4& v) {
    float square=v[0]*v[0];square=square+v[1]*v[1];square=square+v[2]*v[2];
    if(!(square>0)||!std::isfinite(square))throw std::runtime_error("Invalid shadow projection direction");
    const float inverse=1.f/std::sqrt(square);
    return {v[0]*inverse,v[1]*inverse,v[2]*inverse,0};
}
//0x1e2b20's directional branch. Keep even zero products/signs and the scalar
// operation order: normalized and unnormalized directions differ in rounding.
Matrix4 shadow_plane(float y,const Float4& d) {
    constexpr float x=0,z=0;
    const float a=x*d[0],b=y*d[1],c=z*d[2];
    float dot=a+b;dot=dot+c;
    if(!std::isfinite(dot)||dot==0)throw std::runtime_error("Degenerate shadow projection plane");
    const float inv=-1.f/dot;
    return Matrix4{{{inv*(a-dot),inv*(x*d[1]),inv*(x*d[2]),0},
                    {inv*(y*d[0]),inv*(b-dot),inv*(y*d[2]),0},
                    {inv*(z*d[0]),inv*(z*d[1]),inv*(c-dot),0},
                    {inv*-d[0],inv*-d[1],inv*-d[2],inv*-dot}}};
}
Matrix4 shadow_translate(Matrix4 m,const Float4& p) {
    for(unsigned i=0;i<3;++i)m[3][i]=m[3][i]+p[i];
    return m;
}
}
ShadowProjectionContext::ShadowProjectionContext(const Float4& light,const Matrix4& camera):view(camera) {
    for(float f:light)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow light direction");
    for(const auto& col:view)for(float f:col)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow view");
    if(view[0][3]!=0||view[1][3]!=0||view[2][3]!=0||view[3][3]!=1)
        throw std::runtime_error("Shadow view must be affine");
    ProjectionRounding rounding;
    slope=light[0];span=slope+slope;
    world_direction=shadow_normalize(light);
    planes={shadow_plane(-4,world_direction),shadow_plane(4,world_direction)};
    //0x1e2350 uses the transposed basis and a three-term translation product.
    // This intentionally is not a general inverse for scaled camera matrices.
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)inverse[col][row]=view[row][col];
    inverse[3][3]=1;
    for(unsigned row=0;row<3;++row) {
        float f=inverse[0][row]*view[3][0];f=f+inverse[1][row]*view[3][1];
        f=f+inverse[2][row]*view[3][2];inverse[3][row]=0.f-f;
    }
    view_direction=shadow_normalize(shadow_transform(view,world_direction));
}
ShadowProjection ShadowProjectionContext::base() const {
    ProjectionRounding rounding;
    ShadowProjection result;result.has_matrices=true;result.direction=view_direction;
    result.matrices={shadow_multiply(view,shadow_multiply(planes[0],inverse)),
                     shadow_multiply(view,shadow_multiply(planes[1],inverse))};
    return result;
}
ShadowProjection ShadowProjectionContext::project(unsigned mode,float height,float factor,
        const std::array<std::uint32_t,3>& tint) const {
    if(mode>7||!std::isfinite(height)||!std::isfinite(factor))throw std::runtime_error("Invalid shadow projection request");
    for(auto c:tint)if(c>255)throw std::runtime_error("Shadow tint byte overflow");
    ProjectionRounding rounding;
    ShadowProjection out;
    if(mode==2||mode==3||mode==6||mode==7)out.color=tint;
    if(mode<4)return out;
    out.has_matrices=true;
    Matrix4 first,second;float rate=slope,range=span,offset_z=0;
    if(mode==4) {
        rate=slope*factor;
        second=planes[1];
        if(factor!=1) {
            const Float4 direction{rate,1,0,0};
            out.direction=shadow_normalize(shadow_transform(view,direction));
            //0x1b8ec4 normalizes into the later translation temporary. Only
            // X/Y are replaced at0x1b8f58/0x1b8fd0; retain its direction Z.
            offset_z=(*out.direction)[2];
            second=shadow_plane(4,direction);
        }
        height=height-8;range=span+8;
        first=Matrix4{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    } else {
        first=shadow_translate(planes[0],{rate*height,height,0,0});
        second=planes[1];
    }
    const float upper=height+range;
    second=shadow_translate(second,{rate*upper,upper,offset_z,0});
    out.matrices={shadow_multiply(view,shadow_multiply(first,inverse)),
                  shadow_multiply(view,shadow_multiply(second,inverse))};
    return out;
}
Model bind_shadow_model(const Model& main,const Model& shadow) {
    const auto count=main.hierarchy.size();
    if(!count||count>32768||shadow.hierarchy.size()!=count||main.names.size()!=count||shadow.names.size()!=count)
        throw std::runtime_error("Shadow model hierarchy/name counts differ");
    Model result=shadow;result.bones=main.bones;
    for(std::size_t i=0;i<count;++i) {
        auto& node=result.hierarchy[i];
        if(node.parent!= -1) {
            const auto found=std::find(main.names.begin(),main.names.end(),shadow.names[i]);
            if(found==main.names.end())throw std::runtime_error("Shadow model bone name is missing");
            node.bone=static_cast<std::uint16_t>(found-main.names.begin());
        }
        if(node.bone>=result.bones.size())throw std::runtime_error("Shadow model mapped bone outside main channels");
    }
    return result;
}
std::vector<ShadowBatch> shadow_batches(const RenderBlock& block,bool positive_material) {
    std::vector<ShadowBatch> result;ShadowBatch batch;
    for(const auto& polygon:block.polygons) {
        if(polygon.material>=0) {
            if(std::size_t(polygon.material)>=block.materials.size())throw std::runtime_error("Shadow material index");
            const float value=__builtin_bit_cast(float, (block.materials[polygon.material].words[5]));
            if(!std::isfinite(value))throw std::runtime_error("Nonfinite shadow material");
            if((value>0)!=positive_material)continue;
        }
        if(polygon.corners.size()!=3&&polygon.corners.size()!=4)throw std::runtime_error("Unsupported shadow polygon");
        auto positions=batch.positions;std::vector<std::uint16_t> corners;
        for(const auto& corner:polygon.corners) {
            if(corner[0]<0||std::uint32_t(corner[0])>=block.attribute_extents[0])
                throw std::runtime_error("Shadow source position index");
            auto found=std::find(positions.begin(),positions.end(),corner[0]);
            if(found==positions.end()) {corners.push_back(positions.size());positions.push_back(corner[0]);}
            else corners.push_back(found-positions.begin());
        }
        if(positions.size()>97||batch.polygons.size()==97) {
            result.push_back(std::move(batch));batch={};positions.clear();corners.clear();
            for(const auto& corner:polygon.corners) {
                auto found=std::find(positions.begin(),positions.end(),corner[0]);
                if(found==positions.end()) {corners.push_back(positions.size());positions.push_back(corner[0]);}
                else corners.push_back(found-positions.begin());
            }
        }
        batch.positions=std::move(positions);batch.polygons.push_back(std::move(corners));
    }
    if(!batch.polygons.empty())result.push_back(std::move(batch));
    return result;
}
std::vector<Float4> shadow_posed_positions(const RenderBlock& block,std::span<const Matrix4> pose) {
    if(block.attribute_extents[0]>32768)throw std::runtime_error("Shadow position extent");
    ProjectionRounding rounding;
    std::vector<Float4> result(block.attribute_extents[0]);
    for(const auto& range:block.attributes) {
        if(range.kind!=5)continue;
        if(range.flags!=1||range.hierarchy_node<0||std::size_t(range.hierarchy_node)>=pose.size()||
           std::size_t(range.first)+range.values.size()>result.size())
            throw std::runtime_error("Invalid shadow position range");
        const auto& matrix=pose[range.hierarchy_node];
        for(const auto& col:matrix)for(float v:col)if(!std::isfinite(v))throw std::runtime_error("Nonfinite shadow pose");
        for(std::size_t i=0;i<range.values.size();++i) {
            const auto& position=range.values[i];
            for(float v:position)if(!std::isfinite(v))throw std::runtime_error("Nonfinite shadow source position");
            auto& dest=result[range.first+i];
            for(unsigned row=0;row<4;++row) {
                float value=dest[row]*1.f;
                for(unsigned col=0;col<4;++col)value=value+matrix[col][row]*position[col];
                if(!std::isfinite(value))throw std::runtime_error("Shadow skinning overflow");
                dest[row]=value;
            }
        }
    }
    return result;
}
ShadowScreenParameters shadow_screen_parameters(const ShadowScreenSettings& s) {
    const std::array<float,15> values{s.focal,s.scale_x,s.scale_y,s.center_x,s.center_y,
        s.depth_min,s.depth_max,s.depth_near,s.depth_far,s.clip_near,s.clip_far,
        s.fog_near,s.fog_far,s.half_width,s.half_height};
    for(float v:values)if(!std::isfinite(v))throw std::runtime_error("Nonfinite shadow screen setting");
    if(s.focal<=0||s.scale_x<=0||s.scale_y<=0||s.half_width<=0||s.half_height<=0||
       s.depth_near<=0||s.depth_far<=s.depth_near||s.clip_near<=0||s.clip_far<=s.clip_near)
        throw std::runtime_error("Invalid shadow screen range");
    ProjectionRounding rounding;
    const float clip_range=s.clip_far-s.clip_near;
    const float zoffset=((s.clip_far*-2.f)*s.clip_near)/clip_range;
    const float zscale=(s.clip_far+s.clip_near)/clip_range;
    const float depth_range=s.depth_far-s.depth_near;
    const float depth_scale=((s.depth_near*s.depth_far)*(s.depth_max-s.depth_min))/depth_range;
    const float depth_base=(s.depth_min*s.depth_far-s.depth_max*s.depth_near)/depth_range;
    const float xscale=(s.focal*s.scale_x)/s.half_width;
    const float yscale=(s.focal*s.scale_y)/s.half_height;
    float fog_base=255,fog_scale=0;
    if(s.fog_near<s.fog_far) {
        const float range=s.fog_far-s.fog_near;
        fog_base=(-s.fog_near/range)*255.f;
        fog_scale=((s.fog_far*s.fog_near)/range)*255.f;
    }
    ShadowScreenParameters result{{
        {xscale,yscale,zscale,1},{0,0,zoffset,0},
        {1.f/xscale,1.f/yscale,1.f/zscale,0},{0,0,-zoffset/zscale,0},
        {s.center_x,s.center_y,depth_base-(depth_scale*zscale)/zoffset,fog_scale/zoffset},
        {s.half_width,s.half_height,depth_scale/zoffset,fog_base-(fog_scale*zscale)/zoffset},
        {-s.half_width+s.center_x,-s.half_height+s.center_y,-s.clip_near,1},
        {s.half_width+s.center_x,s.half_height+s.center_y,s.clip_far,-depth_base}}};
    for(const auto& v:result)for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Shadow screen arithmetic overflow");
    return result;
}
ShadowVertexCache shadow_vertex_cache(std::span<const Float4> positions,
        const std::array<Matrix4,2>& projections,const ShadowScreenParameters& params) {
    if(positions.empty()||positions.size()>97)throw std::runtime_error("Shadow vertex cache extent");
    for(const auto& m:projections)for(const auto& v:m)for(float f:v)
        if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow cache projection");
    for(const auto& v:params)for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow screen parameter");
    for(const auto& v:positions)for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow cache position");
    ProjectionRounding rounding;
    const Matrix4 perspective{{{params[0][0],0,0,0},{0,params[0][1],0,0},
        {0,0,params[0][2],params[0][3]},params[1]}};
    ShadowVertexCache out;std::uint32_t clip_union=0,clip_intersection=63;
    for(const auto& projection:projections)for(const auto& position:positions) {
        auto camera=shadow_transform(projection,position);
        const auto clip=shadow_transform(perspective,camera);
        for(float f:clip)if(!std::isfinite(f))throw std::runtime_error("Shadow cache arithmetic overflow");
        const float w=std::abs(clip[3]);std::uint32_t flags=0;
        for(unsigned i=0;i<3;++i) {
            if(clip[i]>w)flags|=1u<<(2*i);
            if(clip[i]< -w)flags|=2u<<(2*i);
        }
        clip_union|=flags;clip_intersection&=flags;
        const float reciprocal=clip[3]==0?std::copysign(std::numeric_limits<float>::max(),clip[3]):1.f/clip[3];
        Float4 screen;
        for(unsigned i=0;i<4;++i) {
            float v=params[4][i]*(i==3?clip[2]:clip[3]);
            v=v+clip[i]*params[5][i];v=v*reciprocal;
            if(!std::isfinite(v))throw std::runtime_error("Shadow cache screen overflow");
            screen[i]=std::max(v,0.f);
        }
        screen[3]=std::min(screen[3],255.f);
        camera[3]=0;
        out.camera.push_back(camera);out.screen.push_back(screen);out.clip.push_back(flags);
    }
    out.clip_status=clip_union?(clip_intersection?-1:1):0;
    return out;
}
std::vector<ModelBlockBound> model_block_bounds(const RenderBlock& block,bool shadow) {
    ProjectionRounding rounding;
    if(!std::isfinite(block.bounds[3])||block.bounds[3]<0)
        throw std::runtime_error("Invalid model block radius");
    std::vector<ModelBlockBound> result;
    std::int32_t selected=-1;
    auto positions=[&](const ModelCommand& command)->const std::vector<Float4>& {
        if(command.attribute<0||std::size_t(command.attribute)>=block.attributes.size())
            throw std::runtime_error("Model bound command has no attribute");
        const auto& range=block.attributes[command.attribute];
        if(range.kind!=5||range.hierarchy_node!=selected||range.values.empty())
            throw std::runtime_error("Invalid model bound position range");
        return range.values;
    };
    auto point=[](const Float4& v) {
        for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite model bound position");
        if(v[3]==0)throw std::runtime_error("Zero model bound position weight");
        Float4 p{v[0]/v[3],v[1]/v[3],v[2]/v[3],1};
        for(float f:p)if(!std::isfinite(f))throw std::runtime_error("Model bound position overflow");
        return p;
    };
    for(std::size_t at=0;at<block.commands.size()&&block.commands[at].opcode!=1;++at) {
        const auto& command=block.commands[at];
        if(command.opcode==4)selected=command.hierarchy_node;
        else if(command.opcode==5&&selected>=0) {
            const auto node=selected;
            auto minimum=point(positions(command).front()),maximum=minimum;
            while(at<block.commands.size()&&block.commands[at].opcode!=1&&selected==node) {
                const auto& current=block.commands[at];
                if(current.opcode==4)selected=current.hierarchy_node;
                else if(current.opcode==5)for(const auto& v:positions(current)) {
                    const auto p=point(v);
                    for(unsigned axis=0;axis<3;++axis) {
                        // EE MIN/MAX select their second operand on equal values.
                        minimum[axis]=minimum[axis]<p[axis]?minimum[axis]:p[axis];
                        maximum[axis]=p[axis]<maximum[axis]?maximum[axis]:p[axis];
                    }
                }
                ++at;
            }
            ModelBlockBound bound{{0,0,0,1},0,static_cast<std::uint32_t>(node)};
            Float4 half{};
            for(unsigned axis=0;axis<3;++axis) {
                half[axis]=(maximum[axis]-minimum[axis])*.5f;
                bound.center[axis]=(maximum[axis]+minimum[axis])*.5f;
            }
            float square=half[0]*half[0];square=square+half[1]*half[1];square=square+half[2]*half[2];
            float radius=std::sqrt(square);radius=radius+radius;
            const float minimum_radius=block.bounds[3]+block.bounds[3];
            radius=minimum_radius<radius?radius:minimum_radius;
            bound.radius=shadow?radius+radius:radius;
            if(!std::isfinite(bound.radius))throw std::runtime_error("Model bound radius overflow");
            result.push_back(bound);
            if(at==block.commands.size()||block.commands[at].opcode==1)break;
            // 0x182000 advances after the changed selection, then0x1820c4
            // advances once more. Preserve that skipped command, not a merge
            // of all position ranges with the same hierarchy index.
        }
    }
    return result;
}
std::vector<ModelBlockSelection> visible_model_blocks(
        std::span<const std::vector<ModelBlockBound>> blocks,std::span<const Matrix4> pose,
        const ShadowScreenSettings& settings,bool culling) {
    (void)shadow_screen_parameters(settings); // Validate the shared screen contract.
    ProjectionRounding rounding;
    for(const auto& matrix:pose)for(const auto& col:matrix)for(float f:col)
        if(!std::isfinite(f))throw std::runtime_error("Nonfinite model visibility pose");
    std::vector<ModelBlockSelection> result;
    for(unsigned index=0;index<blocks.size();++index) {
        const auto& bounds=blocks[index];
        if(bounds.empty())throw std::runtime_error("Model visibility requires block bounds");
        bool visible=!culling;float sum_z=0;
        for(const auto& bound:bounds) {
            if(bound.hierarchy_node>=pose.size()||!std::isfinite(bound.radius)||bound.radius<0)
                throw std::runtime_error("Invalid model visibility bound");
            for(float f:bound.center)if(!std::isfinite(f))throw std::runtime_error("Nonfinite model bound center");
            const auto& matrix=pose[bound.hierarchy_node];
            const auto center=shadow_transform(matrix,bound.center);
            sum_z=sum_z+center[2];
            if(!culling)continue;
            Float4 radius{};const float square=bound.radius*bound.radius;
            for(unsigned axis=0;axis<3;++axis) {
                const float a=matrix[0][axis]*matrix[0][axis],b=matrix[1][axis]*matrix[1][axis],c=matrix[2][axis]*matrix[2][axis];
                float extent=a*square;extent=extent+b*square;extent=extent+c*square;
                radius[axis]=std::sqrt(extent);
            }
            if(center[2]+radius[2]<settings.clip_near||settings.clip_far<center[2]-radius[2])continue;
            const float z=center[2]<settings.depth_near?settings.depth_near:center[2];
            if(std::abs(center[0])-radius[0]>((settings.half_width/settings.scale_x)/settings.focal)*z)continue;
            if(std::abs(center[1])-radius[1]>((settings.half_height/settings.scale_y)/settings.focal)*z)continue;
            visible=true;
        }
        const float depth=sum_z/static_cast<float>(bounds.size());
        if(!std::isfinite(depth))throw std::runtime_error("Model visibility depth overflow");
        if(visible)result.push_back({index,depth});
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.depth>b.depth;});
    return result;
}
std::vector<ShadowDraw> shadow_draws(const Model& model,std::span<const Matrix4> pose,
        const ShadowProjection& projection,const ShadowScreenParameters& screen,
        const Float4& default_direction,bool positive_material) {
    std::vector<ModelBlockSelection> blocks;
    for(unsigned i=0;i<model.blocks.size();++i)blocks.push_back({i,0});
    return shadow_draw_blocks(model,pose,projection,screen,default_direction,blocks,positive_material);
}
std::vector<ShadowDraw> shadow_draw_blocks(const Model& model,std::span<const Matrix4> pose,
        const ShadowProjection& projection,const ShadowScreenParameters& screen,
        const Float4& default_direction,std::span<const ModelBlockSelection> blocks,bool positive_material) {
    if(!projection.has_matrices||pose.size()!=model.hierarchy.size())
        throw std::runtime_error("Shadow draw requires projection and complete camera pose");
    std::vector<ShadowDraw> result;
    std::vector<bool> seen(model.blocks.size());
    for(const auto& selected:blocks) {
        const auto block_index=selected.block;
        if(block_index>=model.blocks.size()||seen[block_index])throw std::runtime_error("Invalid shadow block selection");
        seen[block_index]=true;
        const auto& block=model.blocks[block_index];
        const auto positions=shadow_posed_positions(block,pose);
        const auto batches=shadow_batches(block,positive_material);
        for(unsigned batch_index=0;batch_index<batches.size();++batch_index) {
            const auto& batch=batches[batch_index];std::vector<Float4> selected;
            for(auto index:batch.positions)selected.push_back(positions.at(index));
            const auto cache=shadow_vertex_cache(selected,projection.matrices,screen);
            unsigned triangle_index=0;
            for(const auto& polygon:batch.polygons) {
                const std::array<std::array<unsigned,3>,2> triangles{{{0,1,2},{2,3,0}}};
                for(unsigned triangle=0;triangle<(polygon.size()==4?2u:1u);++triangle) {
                    ShadowPrismInput input;input.clip_status=cache.clip_status;
                    input.direction=projection.direction.value_or(default_direction);
                    for(unsigned upper=0;upper<2;++upper)for(unsigned corner=0;corner<3;++corner) {
                        const auto at=upper*selected.size()+polygon[triangles[triangle][corner]];
                        const auto dest=upper*3+corner;
                        input.camera[dest]=cache.camera[at];input.screen[dest]=cache.screen[at];input.clip[dest]=cache.clip[at];
                    }
                    result.push_back({block_index,batch_index,triangle_index++,shadow_prism(input)});
                }
            }
        }
    }
    return result;
}
namespace {
Float4 shadow_cross(const Float4& a,const Float4& b) {
    return {a[1]*b[2]-b[1]*a[2],a[2]*b[0]-b[2]*a[0],a[0]*b[1]-b[0]*a[1],0};
}
float shadow_dot(const Float4& a,const Float4& b) {
    float v=a[0]*b[0];v=v+a[1]*b[1];v=v+a[2]*b[2];
    if(!std::isfinite(v))throw std::runtime_error("Shadow prism arithmetic overflow");
    return v;
}
std::uint32_t shadow_fixed(float value) {
    const float scaled=value*16;
    if(!std::isfinite(scaled)||scaled>=2147483648.f||scaled< -2147483648.f)
        throw std::runtime_error("Shadow vertex exceeds fixed-point range");
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));
}
}
ShadowPrism shadow_prism(const ShadowPrismInput& input) {
    if(input.clip_status< -1||input.clip_status>1)throw std::runtime_error("Invalid shadow clip status");
    for(const auto& values:{input.camera,input.screen})for(const auto& v:values)for(float f:v)
        if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow prism vertex");
    for(float f:input.direction)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow prism direction");
    std::uint32_t clip_union=0,clip_intersection=63;
    for(auto f:input.clip) {
        if(f>63)throw std::runtime_error("Invalid shadow prism clip flags");
        clip_union|=f;clip_intersection&=f;
    }
    ShadowPrism out;
    if(input.clip_status<0)return out;
    ProjectionRounding rounding;
    if(clip_union&&(clip_intersection||(clip_union&0x30)))return out;
    const auto& p=input.camera;
    Float4 edge_a{},edge_b{};
    for(unsigned i=0;i<3;++i){edge_a[i]=p[0][i]-p[1][i];edge_b[i]=p[1][i]-p[2][i];}
    if(shadow_dot(input.direction,shadow_cross(edge_b,edge_a))<=0)return out;
    if(clip_union) {
        for(const auto& v:input.screen)for(unsigned i=0;i<2;++i)
            if(v[i]<=16||v[i]>=4080)return out;
    }
    constexpr std::array<unsigned,12> order{0,1,2,5,1,4,0,3,2,5,4,3};
    for(unsigned pass=0;pass<3;++pass)for(unsigned i=0;i<12;++i) {
        auto v=input.screen[order[i]];
        v[2]=v[2]+(pass==1?200.f:-200.f);
        if(pass==1||i==3||i==10)v[3]=v[3]+2048.f;
        for(unsigned j=0;j<4;++j)out.passes[pass][i][j]=shadow_fixed(v[j]);
    }
    auto face=[&](unsigned point,unsigned a,unsigned b,std::initializer_list<unsigned> vertices) {
        const auto normal=shadow_cross(p[a],p[b]);
        const float distance=std::sqrt(shadow_dot(p[point],p[point]));
        const float size=std::sqrt(shadow_dot(normal,normal));
        const float bias=__builtin_bit_cast(float, (0xb851b717u))*distance;
        const float value=shadow_dot(p[point],normal)+bias*size;
        if(value<=0)return;
        for(auto i:vertices) {
            const float w=input.screen[order[i]][3];
            out.passes[0][i][3]=out.passes[2][i][3]=shadow_fixed(w+2048.f);
            out.passes[1][i][3]=shadow_fixed(w);
        }
    };
    face(0,2,1,{2});face(2,5,1,{4,5});face(1,4,0,{6,7});
    face(0,3,2,{8,9});face(5,3,4,{11});
    out.emitted=true;
    return out;
}
void ShadowTransition::tick(const Float4& hero_rotation,const Float4& hero_position) {
    if(phase>5||!std::isfinite(factor)||!std::isfinite(projection_factor))
        throw std::runtime_error("Unrecovered shadow transition state");
    for(const auto& v:{hero_rotation,hero_position,stun_rotation,stun_position,stun_scale})
        for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow transform");
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set shadow rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    if(!follow_disabled) {
        stun_rotation=hero_rotation;stun_position=hero_position;
        stun_position[1]-=.01f;
    }
    const auto shrink=[&] {if(factor>0){factor-=.05f;return false;}factor=0;return true;};
    const auto grow=[&] {if(factor<1){factor+=.05f;return false;}factor=1;return true;};
    switch(phase) {
    case 0:case 3:break;
    case 1:
        if(shrink()){phase=2;hero_shadow=0xffff;}
        projection_factor=factor;break;
    case 2:
        if(grow())phase=3;
        stun_scale={factor,factor,factor,1};stun_visible=true;break;
    case 4:
        if(shrink()){phase=5;stun_visible=false;}
        stun_scale={factor,factor,factor,1};break;
    case 5:
        if(grow())phase=0;
        projection_factor=factor;hero_shadow=4;break;
    }
}
}
