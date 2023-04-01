#include "okage/battle_interface.hpp"
#include "okage/battle_actor.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding() {
        if(old<0||std::fesetround(FE_TOWARDZERO))
            throw std::runtime_error("Battle interface rounding unavailable");
    }
    ~Rounding(){std::fesetround(old);}
};
float checked(float f) {
    if(!std::isfinite(f)||std::fpclassify(f)==FP_SUBNORMAL)
        throw std::runtime_error("Unrecovered battle interface float class");
    return f;
}
float rotate(float f) {
    checked(f);
    f=checked(f+std::bit_cast<float>(0x3d567451u));
    const float lower=std::bit_cast<float>(0xc0490d0cu),
                upper=std::bit_cast<float>(0x40490d0cu),
                period=std::bit_cast<float>(0x40c90d0cu);
    while(f<lower) {
        const float next=checked(f+period);
        if(next==f)throw std::runtime_error("Battle interface angle wrap cannot advance");
        f=next;
    }
    while(upper<f) {
        const float next=checked(f-period);
        if(next==f)throw std::runtime_error("Battle interface angle wrap cannot advance");
        f=next;
    }
    return f;
}
}
BattleUiPadMasks BattleInterface::first_update(std::shared_ptr<RuwResource> resource,
    BattleUiPad& pad,const BattleUiPadInput& input,unsigned display_rate,
    const BattleUiFirstContext* context) {
    if(!resource)throw std::runtime_error("Battle interface requires owned RUW slot0");
    Rounding rounding;
    BattleUiPadMasks masks{};
    words_[0xe8c/4]=0;
    for(unsigned layer=0;layer<words_[0xe40/4];++layer) {
        if(layer>=handles_.size())throw std::runtime_error("Battle interface layer extent");
        const unsigned base=(0x40+layer*0xe0)/4;
        if(!(words_[base+0x50/4]&0x40))continue;
        words_[0xe48/4]=layer;
        words_[base+0xd4/4]=1;
        auto& cursor=words_[0xe8c/4];
        if(cursor>=16)throw std::runtime_error("Battle interface state cursor outside owner");
        auto& state=words_[0xe4c/4+cursor];
        const int handle=handles_.at(layer);
        if(handle<0||unsigned(handle)>=shared_.registry.size()||!shared_.registry[unsigned(handle)])
            throw std::runtime_error("Battle interface callback window lifetime");
        auto& w=*shared_.registry[unsigned(handle)];
        const auto bind=[&](std::span<const std::size_t> groups) {
            shared_.resource_accumulators[0]=0;
            w.bind_ruw(resource,groups);
        };
        const auto transform=[&] {
            std::array<std::uint32_t,3> position,rotation;
            std::copy_n(words_.begin()+base+0x10/4,3,position.begin());
            std::copy_n(words_.begin()+base+0x20/4,3,rotation.begin());
            w.set_transform_xyz(position,rotation);
        };
        switch(words_[base+0x60/4]) {
        case 2: {
            // 165b50 always executes this prefix, including states 0/-100.
            masks=pad.normalize(input);
            if(shared_.hud.selection_state>50)masks={};
            switch(std::bit_cast<std::int32_t>(state)) {
            case 0: {
                pad.set_enabled(0,true);pad.set_repeat(0,30,15,display_rate);
                words_[base+0x64/4]=0xffffffff;
                words_[base+0x64/4]=0;
                words_[base+0x64/4]=19;
                w.set_opacity_tween(-128.f,128.f,0,128);
                constexpr std::array<std::size_t,3> groups{2,3,4};bind(groups);
                ++state;state=std::bit_cast<std::uint32_t>(std::int32_t(-100));
                break; // Unlike type3, no same-call fallthrough.
            }
            case -100:
                words_[base+0x64/4]=0;words_[base+0x68/4]=0;
                words_[base+0x64/4]=0;state=2;break;
            case -1:w.set_opacity_tween(0.f,0.f,0,128);break;
            case 1: {
                if(!context)throw std::runtime_error("Battle type2 opening requires actual selection ownership");
                words_[base+0x24/4]=std::bit_cast<std::uint32_t>(rotate(std::bit_cast<float>(words_[base+0x24/4])));
                words_[base+0x24/4]=0;
                words_[base+0x64/4]=0;words_[base+0x68/4]=0;words_[base+0x64/4]=0;
                ++state;
                if(context->game_mode==4)words_[base+0xa8/4]=0;
                else {
                    if(context->selected_entity<0||context->selected_entity>=3)
                        throw std::runtime_error("Battle type2 selected party entity outside source slots");
                    const auto ready=battle_ui_ready_party(context->entities);
                    for(unsigned i=0;i<3;++i)if(ready[i]==context->selected_entity)words_[base+0xa8/4]=i;
                    const auto selected=unsigned(context->selected_entity)*64;
                    words_[base+0x8c/4]=shared_.hud.type2_work[selected];
                    for(unsigned i=0;i<7;++i) {
                        const auto category=std::bit_cast<std::int32_t>(words_[base+0x8c/4]);
                        const auto at=std::int64_t(selected)+category+i*8;
                        if(at<0||std::uint64_t(at)>=shared_.hud.type2_work.size())
                            throw std::runtime_error("Battle type2 remembered selection outside persistent cache");
                        if(shared_.hud.type2_work[std::size_t(at)]!=0xffffffffu)words_[base+0x8c/4+i]=0;
                    }
                }
                transform();break;
            }
            case 2: {
                words_[base+0x64/4]=0xffffffff;words_[base+0x68/4]=0;
                if(w.opacity()==0.f)break;
                if(!context||!context->party)
                    throw std::runtime_error("Battle type2 category selection requires live party ownership");
                const auto sound=[&](unsigned id){
                    if(!context->sound)throw std::runtime_error("Battle type2 input requires source sound owner");
                    context->sound(id);
                };
                const auto signed_word=[](auto value){return std::bit_cast<std::int32_t>(value);};
                for(unsigned i=1;i<7;++i)words_[base+0x8c/4+i]=0;
                for(unsigned i=0;i<8;++i)words_[base+0x6c/4+i]=0xffffffffu;
                auto& category=words_[base+0x8c/4];
                auto& ordinal=words_[base+0xa8/4];
                if(masks.repeat&0x1000) {
                    --category;if(signed_word(category)<0)category=3;
                    words_[base+0x6c/4]=category;sound(0);
                }
                const auto available=battle_ui_available_party(context->entities);
                if(available>1&&(masks.pressed&8)) {
                    ++ordinal;ordinal=std::uint32_t(signed_word(ordinal)%std::int32_t(available));
                    words_[base+0x88/4]=ordinal;sound(0);
                }
                if(available>1&&(masks.pressed&4)) {
                    --ordinal;if(signed_word(ordinal)<0)ordinal+=available;
                    words_[base+0x88/4]=ordinal;sound(0);
                }
                if(masks.repeat&0x4000) {
                    ++category;if(signed_word(category)>3)category=0;
                    words_[base+0x6c/4]=category;sound(0);
                }
                const auto party_count=context->party->counts()[3];
                if(shared_.formation.count==0)shared_.hud.type2_enabled=0;
                if(party_count==0)shared_.hud.type2_enabled=0;
                bool disabled=shared_.formation.count==0||party_count==0;
                if(context->selected_entity<0||context->selected_entity>=8)
                    throw std::runtime_error("Battle type2 selected party ordinal extent");
                const int slot=context->party->at(3,unsigned(context->selected_entity));
                if(slot>=0&&slot<8) {
                    if(!context->tables)throw std::runtime_error("Battle type2 character refresh requires owned game tables");
                    refresh_game_character(*context->tables,context->variables,unsigned(slot));
                    const auto at=0x5c4+unsigned(slot)*0x50+8;
                    const auto hp=std::uint32_t(context->variables[at])|(std::uint32_t(context->variables[at+1])<<8)|
                                  (std::uint32_t(context->variables[at+2])<<16)|(std::uint32_t(context->variables[at+3])<<24);
                    if(hp==0){disabled=true;if(slot==0)shared_.hud.type2_enabled=0;}
                }
                if(disabled)category=3;
                if(masks.pressed&0x40) {
                    switch(signed_word(category)) {
                    case 0:if(disabled)break;words_[base+0x64/4]=1;break;
                    case 1:case 2:
                        if(disabled)break;
                        throw std::runtime_error("Battle type2 spell/item list preparation is unrecovered");
                    case 3:words_[base+0x64/4]=16;break;
                    default:throw std::runtime_error("Battle type2 category outside recovered input scope");
                    }
                    if(disabled&&category!=3)break;
                    if(context->selected_entity>=3)throw std::runtime_error("Battle type2 remembered party selection extent");
                    shared_.hud.type2_work[unsigned(context->selected_entity)*64]=category;
                    words_[base+0x68/4]=words_[base+0x64/4];++state;sound(3);
                }
                break;
            }
            case 3: {
                if(!context)throw std::runtime_error("Battle type2 submenu requires live selection ownership");
                const auto as_signed=[](auto v){return std::bit_cast<std::int32_t>(v);};
                const auto sound=[&](unsigned id){
                    if(!context->sound)throw std::runtime_error("Battle type2 input requires source sound owner");
                    context->sound(id);
                };
                bool selected_action=false;
                if(context->game_mode!=4) {
                    if(context->selected_entity<0||std::size_t(context->selected_entity)>=context->entities.size())
                        throw std::runtime_error("Battle type2 submenu selected entity extent");
                    if(const auto* actor=context->entities[unsigned(context->selected_entity)].actor) {
                        //154330 reads body13c, hence task17c; no stat refresh.
                        const auto& b=actor->storage;
                        selected_action=(unsigned(b[0x17c])|(unsigned(b[0x17d])<<8)|
                            (unsigned(b[0x17e])<<16)|(unsigned(b[0x17f])<<24))!=0;
                    }
                }
                words_[base+0x64/4]=0xffffffff;
                for(unsigned i=0;i<8;++i)words_[base+0x6c/4+i]=0xffffffff;
                unsigned column=0,count=1,next=0,previous=0;
                const auto available=battle_ui_available_party(context->entities);
                const auto character_slot=[&]() {
                    if(!context->party)throw std::runtime_error("Battle action requires live party ownership");
                    const int entity=context->game_mode==4?0:context->selected_entity;
                    if(entity<0||entity>=8)throw std::runtime_error("Battle action selected party extent");
                    return context->party->at(3,unsigned(entity));
                };
                const auto refresh_character=[&](unsigned slot) {
                    if(!context->tables)throw std::runtime_error("Battle action requires owned stat tables");
                    refresh_game_character(*context->tables,context->variables,slot);
                    return std::size_t(0x5c4+slot*0x50);
                };
                switch(as_signed(words_[base+0x68/4])) {
                case 1:
                    shared_.hud.type2_flag_b=0;shared_.hud.type2_flag_a=0;
                    if(available<2&&!selected_action) {
                        words_[base+0x90/4]=0;words_[base+0x70/4]=0;
                        resource->select_class_color(3,1,0);resource->select_class_color(3,2,0);
                        words_[base+0x64/4]=2;words_[base+0x68/4]=2;
                        break;
                    }
                    count=available>=2&&selected_action?3:2;column=1;next=2;
                    goto submenu_input;
                case 2:
                    shared_.hud.type2_flag_a=0;shared_.hud.type2_flag_b=0;
                    if(available<2&&!selected_action&&(masks.pressed&0x20)) {
                        words_[base+0x64/4]=0xffffff9c;words_[base+0x68/4]=0;
                        goto submenu_input;
                    }
                    if(available>=2)shared_.target_count=shared_.formation.count;
                    if((available<2&&selected_action&&words_[base+0x90/4]==1)||
                       (available>=2&&words_[base+0x90/4]==2)) {
                        shared_.hud.type2_flag_b=1;
                        words_[base+0x64/4]=5;words_[base+0x68/4]=5;
                        break;
                    }
                    shared_.target_count=shared_.formation.count;
                    column=2;count=shared_.target_count;next=5;previous=1;
                    goto submenu_input;
                case 5: {
                    const auto option=words_[base+0x90/4];
                    if(masks.pressed&0x20) {
                        words_[base+0x64/4]=0xffffff9c;words_[base+0x68/4]=2;
                        if(selected_action&&((option==1&&available<2)||option==2))
                            words_[base+0x68/4]=1;
                        break;
                    }
                    if(words_[base+0x8c/4]!=0)
                        throw std::runtime_error("Battle target action outside Attack category is unrecovered");
                    if(option==2||(option==1&&available<2&&selected_action&&shared_.command.values[0]==0)) {
                        shared_.pending_action={4,10,0xca};
                    } else if(option<=1) {
                        auto target=as_signed(words_[base+0x94/4]);
                        if(target!=10) {
                            const int slot=character_slot();
                            if(slot>=0&&slot<8) {
                                const auto at=refresh_character(unsigned(slot));
                                if(context->variables[at+1]==1) {
                                    if(!context->random)throw std::runtime_error("Battle random targeting requires live RNG");
                                    const auto draw=as_signed(context->random());
                                    if(as_signed(shared_.target_count)<=0||shared_.target_count>8)
                                        throw std::runtime_error("Battle random targeting divisor outside owned formation");
                                    target=draw%as_signed(shared_.target_count);
                                }
                            }
                        }
                        if(target<0||std::size_t(target)>=shared_.formation.groups.size())
                            throw std::runtime_error("Battle action target outside owned formation group table");
                        shared_.pending_action={option,std::uint32_t(shared_.formation.groups[unsigned(target)]),0};
                    } else throw std::runtime_error("Battle Attack option outside recovered selection");
                    column=3;previous=2;next=20;goto submenu_input;
                }
                default:
                    throw std::runtime_error("Battle type2 submenu argument unrecovered: "+
                        std::to_string(as_signed(words_[base+0x68/4])));
                }
                break;
submenu_input:
                auto& selected=words_[base+0x8c/4+column];
                auto& changed=words_[base+0x6c/4+column];
                if(count>8)throw std::runtime_error("Battle type2 submenu count exceeds owned formation");
                if(count)selected=std::uint32_t(as_signed(selected)%std::int32_t(count));
                if(count&&(masks.pressed&0x40)) {
                    if(next==20) {
                        const int slot=character_slot();
                        if(slot<0||slot>=8)break; //Source bypasses the rest of the tail.
                        const auto at=refresh_character(unsigned(slot));
                        context->variables[at+0x26]=101;context->variables[at+0x27]=0;
                        if(shared_.pending_action[0]==1) {
                            for(unsigned i=0;i<context->party->counts()[5];++i) {
                                const int member=context->party->at(5,i);
                                if(member<0||member>=8)throw std::runtime_error("Battle combined action party record extent");
                                const auto record=refresh_character(unsigned(member));
                                const auto charge=std::bit_cast<std::int16_t>(std::uint16_t(
                                    unsigned(context->variables[record+0x26])|(unsigned(context->variables[record+0x27])<<8)));
                                if(charge<101){context->variables[record+0x26]=100;context->variables[record+0x27]=0;}
                            }
                        }
                        state=5;words_[base+0xa8/4]=0;
                    } else if(words_[base+0x68/4]==next)words_[base+0x64/4]=0xffffffff;
                    else {
                        const int selected_entity=context->game_mode==4?0:context->selected_entity;
                        const auto cache=std::int64_t(selected_entity)*64+column*8+
                            as_signed(words_[base+0x8c/4]);
                        if(cache<0||std::uint64_t(cache)>=shared_.hud.type2_work.size())
                            throw std::runtime_error("Battle type2 submenu cache extent");
                        shared_.hud.type2_work[std::size_t(cache)]=selected;
                        words_[base+0x64/4]=next;changed=selected;
                    }
                    if(next!=20)words_[base+0x68/4]=next;
                    sound(3);
                }
                if(count>=2) {
                    if(masks.repeat&0x1000) {
                        --selected;if(as_signed(selected)<0)selected+=count;
                        changed=selected;sound(0);
                    }
                    if(masks.repeat&0x4000) {
                        ++selected;selected=std::uint32_t(as_signed(selected)%std::int32_t(count));
                        changed=selected;sound(0);
                    }
                }
                if(masks.pressed&0x20) {
                    words_[base+0x64/4]=0xffffff9c;words_[base+0x68/4]=previous;
                    selected=0;changed=0;
                    if(previous==0)state=1;
                    sound(4);
                }
                break;
            }
            case 5:
                if(w.opacity()==0.f) {
                    words_[base+0x64/4]=4;words_[base+0x68/4]=4;state=6;
                }
                if(w.opacity()==128.f)w.set_opacity_tween(-16.f,128.f,0,128);
                break;
            case 6:words_[base+0x64/4]=0xffffffff;break;
            default:
                throw std::runtime_error("Battle type2 state unrecovered: "+
                    std::to_string(std::bit_cast<std::int32_t>(state)));
            }
            ++cursor;break;
        }
        case 3:
            if(state==0) {
                words_[base+0x64/4]=0xffffffff;words_[base+0x64/4]=0;
                constexpr std::array<std::size_t,1> groups{5};bind(groups);
                w.set_opacity_tween(-128.f,128.f,0,128);
                words_[base+0x64/4]=0;++state;
            }
            if(state==1) {
                float angle=rotate(std::bit_cast<float>(words_[base+0x24/4]));
                words_[base+0x24/4]=std::bit_cast<std::uint32_t>(angle);
                if(angle>0.f) {
                    words_[base+0x24/4]=0;
                    words_[base+0x64/4]=0;words_[base+0x68/4]=0;
                    words_[base+0x64/4]=0;++state;
                }
                transform();
            } else if(state==0xffffffffu)w.set_opacity_tween(-4.f,128.f,0,128);
            ++cursor;break;
        default:throw std::runtime_error("Battle interface first callback type unrecovered");
        }
        if(std::bit_cast<std::int32_t>(cursor)>15)cursor=0;
    }
    return masks;
}
}
