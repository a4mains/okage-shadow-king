#include "title_session.hpp"
#include "image_metal.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <stdexcept>

namespace okage {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto n=std::filesystem::file_size(path);
    check(n<=64*1024*1024,"Title asset extent");
    std::vector<std::uint8_t> data(n);std::ifstream in(path,std::ios::binary);
    check(bool(in.read(reinterpret_cast<char*>(data.data()),n)),"Title asset read");return data;
}
Float4 floats(const TitleVector& words){Float4 result;for(unsigned i=0;i<4;++i)result[i]=std::bit_cast<float>(words[i]);return result;}
TitleVector words(const Float4& value){TitleVector result;for(unsigned i=0;i<4;++i)result[i]=std::bit_cast<std::uint32_t>(value[i]);return result;}
Texture image(Bytes bytes) {
    auto result=decode_xim(bytes);
    check(result.pixel_format==19&&result.palette_format==0,"Title image source format");
    constexpr unsigned pixels=16+16+256*4+16;
    check(bytes.size()==pixels+std::size_t(result.width)*result.height,"Title palette/pixel extent");
    for(auto index:bytes.subspan(pixels))check(bytes[32+unsigned(index)*4+3]<=128,"Title image alpha source exceeds128");
    return result;
}
constexpr ImageMetalBaseline baseline{0x6c00,0x7900,0,639,0,223,0,1,0,1};
constexpr std::array<const char*,8> names{"title_sample4.xim","push.xim","new.xim","restore.xim","maru_w.xim","maru_b.xim","sp.xim","hanten.xim"};
}
struct TitleSession::Impl {
    TitleTaskState task{};
    bool live=true,ready=false,first_water_color=false;
    StageSettings settings;
    std::array<Texture,8> images;
    std::array<id<MTLTexture>,8> image_textures;
    id<MTLTexture> fade_texture;
    std::vector<std::pair<unsigned,ImageQuad>> image_queue;
    std::unique_ptr<FieldActor> model;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<ImageMetal> image_renderer;
    MapEffects effects;
    EffectSubmissions submissions;
    EffectDefinition water_definition;
    ShadowScreenParameters ui_projection=initial_ui_projection();
    Texture output;
    Impl(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,GameOverShared& shared,TitleConstruction construction) {
        clear.rgb={0,0,0};fade.color=0;initialize_scene_ui(ui);fade.immediate(0);
        check(bool(construction.stop_music)&&bool(construction.start_theme),"Title construction audio services");
        construction.stop_music();
        auto device=MTLCreateSystemDefaultDevice();check(device,"Title Metal device");
        image_renderer=std::make_unique<ImageMetal>(device);
        fade_texture=upload_image_gs_texture(device,Texture{1,1,0,0,{128,128,128,255}},ImageMetalPreviewAlpha::SourceAtMost128);
        for(unsigned i=0;i<names.size();++i){
            images[i]=image(read(construction.common_root/"2D/SCR.XPF"/names[i]));
            image_textures[i]=upload_image_gs_texture(device,images[i],ImageMetalPreviewAlpha::SourceAtMost128);
        }
        renderer=std::make_unique<SceneRenderer>();
        if(construction.verified_effect_textures)renderer->load_effect_textures(*construction.verified_effect_textures);
        else renderer->load_effect_textures(construction.effect_textures);
        task.texture=renderer->add_effect_texture(images[7]);
        const auto model_path=construction.common_root/"2D/NIGHT_V.XPF/a.xmd";
        model=std::make_unique<FieldActor>(read_xmd(read(model_path)));
        renderer->add_actor({model.get(),model_path,0,SceneActorDescriptor{0,false}});
        renderer->set_stage_environment(settings.lighting,settings.shadows,settings.sky);
        renderer->set_sort_depth_shift(settings.bucket_base);renderer->set_clear_rgb(clear.rgb);
        renderer->bind_effect_submissions(&submissions);
        auto definitions=resolve_effect_references(read_efd(construction.effect_data));
        auto found=std::find_if(definitions.begin(),definitions.end(),[](const auto& d){return d.id==106;});
        check(found!=definitions.end(),"Missing title water template106");water_definition=*found;
        task.common.submode=1;shared.initialize_mode();construction.start_theme();
        fade.request(1);fade.immediate(0);
    }
    void submit(const GameOverImageCall& call) {
        check(call.a0<images.size(),"Title image identity");
        if(image_queue.size()==512)return;
        const auto& source=images[call.a0];
        image_queue.emplace_back(call.a0,axis_aligned_image_quad(source.width,source.height,call.fp_words));
    }
};
TitleSession::TitleSession(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,GameOverShared& shared,TitleConstruction construction)
    :impl_(std::make_unique<Impl>(clear,fade,ui,shared,std::move(construction))){}
TitleSession::~TitleSession()=default;
void TitleSession::tick(GameOverPadSample pad,std::uint32_t confirm_mask,GameOverShared& shared,SceneFade& fade,Animation& camera,
    FieldProjection& projection,RetiringSceneEffects* retiring,BattleUiShared& ui,
    std::shared_ptr<const std::vector<Font>> fonts,const SineTable& sine,GameRandom& random,const TitleActions& actions) {
    auto& p=*impl_;check(p.live,"Dead title resource callback");p.image_queue.clear();
    const TitleScratch scratch{0,{0,0,0},{0,0,0}};
    TitleUpdateServices services;
    services.common={[&](float v){fade.immediate(v);},actions.request,[&](const auto& call){p.submit(call);}};
    services.set_camera=[&](unsigned channel,const TitleVector& value){camera.set_channel(channel,floats(value));};
    services.model_channel_base=[] {return 0u;};
    services.read_model_channel=[&](unsigned channel){check(channel<4,"Title model channel");return words(p.model->animation().value(channel));};
    services.set_model_channel=[&](unsigned channel,const TitleVector& value){check(channel<4,"Title model channel");p.model->animation().set_channel(channel,floats(value));};
    services.create_water=[&](const TitleWaterCall& call){
        check(call.definition==106,"Title water identity");unsigned slot=0;while(slot<p.effects.size()&&p.effects[slot])++slot;
        check(slot<p.effects.size(),"Title effect pool exhausted");
        p.effects[slot]=std::make_unique<MapEffect>(p.water_definition,floats(call.position),floats(call.rotation),floats(call.scale),
            p.renderer->effect_texture_sizes(),[&]{return random.next();},EffectStart::FirstFrame);
        p.effects[slot]->source_position_w_available=false;p.first_water_color=true;return slot;
    };
    services.rebind_water=[&](unsigned slot,unsigned element,unsigned texture){
        check(slot<p.effects.size()&&p.effects[slot]&&texture==p.task.texture,"Title water texture owner");
        p.effects[slot]->rebind_water_texture(element,texture,p.images[7].width,p.images[7].height);
    };
    services.set_water_color=[&](unsigned slot,const TitleVector& value){
        if(slot>=p.effects.size()||!p.effects[slot])return;
        auto& effect=*p.effects[slot];effect.color=floats(value);
        effect.source_color_rgb_available=!p.first_water_color;p.first_water_color=false;
    };
    services.set_water_clamp=[&](unsigned slot,unsigned mode){if(slot<p.effects.size()&&p.effects[slot])p.effects[slot]->clamp_mode=mode;};
    services.stop_water=[&](unsigned slot){if(slot<p.effects.size()&&p.effects[slot])p.effects[slot]->request_removal();};
    services.play_sound=actions.sound;services.reset_game=actions.reset_game;services.reset_progress=actions.reset_progress;
    services.select_saved_field=actions.select_saved_field;services.read_saved_field=actions.read_saved_field;
    title_update(p.task,shared.input,pad,confirm_mask,scratch,services);
    //Attached resource100f90 follows the title callback in the same traversal.
    ++shared.image_frame_counter;camera.tick_channels();
    auto position=camera.value(0),rotation=camera.value(1);position[3]=1;rotation[3]=0;
    p.model->evaluate_world();projection.begin_resource(p.settings.cached_depth_min,p.settings.cached_depth_max);
    const Float4 inverse_rotation{-rotation[0],-rotation[1],-rotation[2],0};
    const auto view=camera_matrix({-position[0],-position[1],-position[2],1},inverse_rotation,{1,1,1,0},sine);
    p.submissions.clear();projection.begin_effects();if(retiring)retiring->tick();
    std::array<unsigned,84> deferred{};unsigned count=0;
    for(unsigned slot=0;slot<p.effects.size();++slot)if(auto& effect=p.effects[slot]) {
        if(effect->flags&0x200){effect.reset();continue;}
        if(effect->flags&0x100){effect->flags|=0x200;continue;}
        check((effect->flags&0x400)&&!(effect->flags&0x4000),"Unexpected ordinary/disabled title effect");
        deferred[count++]=slot;
    }
    projection.begin_deferred();
    for(unsigned i=0;i<count;++i) {
        auto& effect=*p.effects[deferred[i]];
        const EffectProjectionContext context{view,shadow_screen_parameters(projection.current()),projection.current().focal};
        effect.tick([&]{return random.next();},true,&position,&context);
        if(!(effect.flags&0x100)){projection.select_effect_near(effect.near_clip);p.submissions.push_back({deferred[i],effect});}
    }
    projection.end_effects();
    check(std::none_of(ui.registry.begin(),ui.registry.end(),[](const auto& w){return bool(w);}),"Title unexpectedly owns UI windows");
    check(!ui.resource.advance_resource(ui.registry,{},ui.resource_accumulators,std::move(fonts),sine,random,p.ui_projection),
        "Title unexpectedly queued UI geometry");
    const auto& screen=projection.frame().ordinary;
    auto prepared=p.renderer->prepare(view,field_clip_projection(screen),nullptr,inverse_rotation,screen.clip_far,position,&projection.frame(),1,false);
    SceneGpuPass image_pass{[&](id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth){
        auto validations=std::make_shared<std::vector<ImageMetalValidation>>();
        for(const auto& [index,quad]:p.image_queue)validations->push_back(p.image_renderer->encode(command,color,depth,quad,baseline,{p.image_textures[index],{}}));
        if(fade.enabled) {
            check(fade.alpha()<=128,"Title fade per-grid colors unsupported");
            auto quad=axis_aligned_image_quad(640,448,{0,0,0x3f800000,0x3f800000,0,0x3f800000});
            quad.st={.5f/1024,.5f/1024,.5f/1024,.5f/1024};
            quad.rgba=fade.published_color()|(fade.alpha()<<24);
            validations->push_back(p.image_renderer->encode(command,color,depth,quad,baseline,{p.fade_texture,{}}));
        }
        return [validations,command]{for(const auto& validation:*validations)validation.validate_completed(command);};
    },true};
    p.output=p.renderer->render_prepared(prepared,nullptr,&image_pass,std::array<std::uint8_t,4>{0,0,0,0});p.ready=true;
}
RetiringSceneEffects TitleSession::retire(Animation& camera,FieldProjection& projection,BattleUiShared& ui) {
    auto& p=*impl_;check(p.live,"Title retired twice");p.renderer.reset();p.model.reset();
    RetiringSceneEffects result;result.map=std::move(p.effects);result.mark();
    for(auto& window:ui.registry)window.reset();projection.reset_scene();reset_scene_camera(camera);
    p.submissions.clear();p.image_queue.clear();p.live=false;p.ready=false;
    return result;
}
bool TitleSession::resource_live() const{return impl_->live;}
const Texture* TitleSession::rendered_image() const{return impl_->live&&impl_->ready?&impl_->output:nullptr;}
const TitleTaskState& TitleSession::state() const{return impl_->task;}
}
