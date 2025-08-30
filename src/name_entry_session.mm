#include "okage/input_log.hpp"
#include "name_entry_session.hpp"
#include "results_renderer.hpp"
#include "okage/ui_geometry.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <stdexcept>
namespace okage {
namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path){const auto n=std::filesystem::file_size(path);if(n>64*1024*1024)throw std::runtime_error("Name asset extent");std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);if(!in.read(reinterpret_cast<char*>(b.data()),n))throw std::runtime_error("Name asset read");record_input_read(path,b);return b;}
}
struct NameEntrySession::Impl {
    bool live=true;unsigned phase=0;
    BattleUiShared& ui;NameEntryShared& shared;
    std::array<std::uint32_t,2> name_words{};
    StageSettings settings;
    ShadowScreenParameters ui_projection=initial_ui_projection();
    std::shared_ptr<const std::vector<Font>> fonts;
    std::unique_ptr<NameEntryData> data;
    std::shared_ptr<RuwResource> ruw;
    std::shared_ptr<const Texture> atlas;
    std::unique_ptr<NameEntryInterface> manager;
    std::unique_ptr<ResultsRenderer> renderer;
    const Texture* output=nullptr;
    NameEntrySession::FrameObservation frame;
    int window=-1;
    Impl(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,NameEntryShared& shared,NameEntryConstruction c):ui(ui),shared(shared),fonts(std::move(c.fonts)) {
        clear.rgb={0,0,0};fade.color=0;initialize_scene_ui(ui);fade.immediate(0);
        if(!c.stop_music)throw std::runtime_error("Name entry music service");c.stop_music();
        data=std::make_unique<NameEntryData>(read(c.ui_root/"NAME_ENTRY.bin"));
        atlas=std::make_shared<const Texture>(decode_xim(c.atlas_bytes?Bytes(*c.atlas_bytes):Bytes(read(c.common_root/"2D/MAIN.XPF/bm_if.xim"))));
        ruw=std::make_shared<RuwResource>(read(c.ui_root/"FLD.RUW"));
        renderer=std::make_unique<ResultsRenderer>(fonts,atlas);
        manager=std::make_unique<NameEntryInterface>(ui,shared,*data,ruw,atlas,0);window=int(manager->handle());
        fade.immediate(1);
    }
};
NameEntrySession::NameEntrySession(SceneClearColor& c,SceneFade& f,BattleUiShared& u,NameEntryShared& n,NameEntryConstruction setup):impl_(std::make_unique<Impl>(c,f,u,n,std::move(setup))){}
NameEntrySession::~NameEntrySession()=default;
void NameEntrySession::tick(BattleUiPad& pad,BattleUiPadInput input,std::span<std::uint8_t> variables,const GameDataTables& tables,
    SceneFade& fade,Animation& camera,FieldProjection& projection,RetiringSceneEffects* retiring,BattleUiShared& ui,
    unsigned& image_counter,const SineTable& sine,GameRandom& random,const std::function<void(unsigned)>& sound,const std::function<void()>& finish) {
    auto& p=*impl_;if(!p.live)throw std::runtime_error("Dead name entry resource");
    (void)image_counter; //100ef8 leaves resource+0x24 clear and no 1009e0 load runs: 100f90 skips 100800/1b3450.
    p.frame={};
    //124630 parent precedes resource and manager children.
    const bool unlink=p.manager&&p.manager->dead();
    p.frame.unlinked=unlink;
    if(p.phase==0){if(!p.manager||unlink)p.phase=1;}
    else if(p.phase==1){fade.immediate(0);finish();p.frame.finished=true;}
    else throw std::runtime_error("Name entry parent phase");
    camera.tick_channels();
    projection.submit_empty_resource_frame(true,p.settings.cached_depth_min,p.settings.cached_depth_max);
    if(retiring)retiring->tick();
    std::vector<UiResourceSlotBinding> bindings;
    if(p.manager&&!p.manager->dead())bindings.push_back({p.manager->handle(),0});
    const auto draw=ui.resource.advance_resource(ui.registry,bindings,ui.resource_accumulators,p.fonts,sine,random,p.ui_projection);
    p.output=&p.renderer->render(draw,fade);
    if(unlink)p.manager.reset();
    else if(p.manager){
        const auto word=[&](unsigned at){return std::bit_cast<std::int32_t>(p.manager->words()[at/4]);};
        p.frame.manager_ran=true;p.frame.phase_in=word(0xe4c);p.frame.pressed=input.pressed;p.frame.repeat=input.repeat;
        p.manager->first_update(pad,input,variables,sound);
        p.manager->second_update(variables,tables);
        p.frame.phase_out=word(0xe4c);p.frame.length_out=word(0xa8);p.frame.selection_out=word(0xcc);p.frame.yes_out=word(0xd8);
        p.frame.died=p.manager->dead();
        if(p.manager->dead())p.manager->destroy_windows();
    }
    if(variables.size()<0x854)throw std::runtime_error("Name observation variable extent");
    for(unsigned i=0;i<2;++i){p.name_words[i]=0;for(unsigned j=0;j<4;++j)p.name_words[i]|=unsigned(variables[0x844+i*4+j])<<(j*8);}
    std::copy_n(variables.begin()+0x844,16,p.frame.name.begin());
}
const NameEntrySession::FrameObservation& NameEntrySession::last_frame() const{return impl_->frame;}
int NameEntrySession::ui_window() const{return impl_->window;}
void NameEntrySession::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* retiring,BattleUiShared& ui) {
    auto& p=*impl_;if(!p.live)throw std::runtime_error("Name resource retired twice");
    //Resource child precedes UI sibling in the descendant deletion traversal.
    if(retiring)retiring->mark();for(auto& window:ui.registry)window.reset();
    projection.reset_scene();reset_scene_camera(camera);p.manager.reset();p.renderer.reset();p.ruw.reset();p.atlas.reset();p.output=nullptr;p.live=false;
}
bool NameEntrySession::resource_live() const{return impl_->live;}
const Texture* NameEntrySession::rendered_image() const{return impl_->live?impl_->output:nullptr;}
std::vector<std::uint32_t> NameEntrySession::observation() const {
    const auto& p=*impl_;const auto* window=p.manager&&!p.manager->dead()?p.ui.registry[p.manager->handle()].get():nullptr;
    std::vector<std::uint32_t> words{p.phase,unsigned(bool(p.manager)),p.manager?unsigned(p.manager->dead()):0,
        unsigned(bool(window)),window?window->word(0x270):0,window?window->word(0x27c):0,p.name_words[0],p.name_words[1],
        unsigned(std::uint8_t(p.shared.selected_character[0]))|(unsigned(std::uint8_t(p.shared.selected_character[1]))<<8),p.shared.flag};
    if(p.manager)words.insert(words.end(),p.manager->words().begin(),p.manager->words().end());else words.resize(10+0xef0/4);
    return words;
}
}
