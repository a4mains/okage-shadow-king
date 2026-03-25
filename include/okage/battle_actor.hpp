#pragma once
#include "okage/battle_projectiles.hpp"
#include "okage/actor.hpp"
#include "okage/battle_data.hpp"
#include "okage/battle_action_data.hpp"
#include "okage/battle_entry.hpp"
#include "okage/battle_state.hpp"
#include "okage/game_state.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace okage {
struct BattleHitServices;
struct SceneModelAsset;
using BattleModelAssetProvider = std::function<std::shared_ptr<const SceneModelAsset>(
    const std::filesystem::path&, unsigned requested_flags)>;
struct BattleActor {
    //ActionSequence is 156c70, installed by 154b38 state8 (Stan action205).
    enum class FirstCallback {Roster,ApproachAttack,BallisticAttack,RangedAttack,EnemyDeath,PartyDeath,ActionSequence};
    FirstCallback first_callback=FirstCallback::Roster;
    std::optional<FirstCallback> saved_first_callback;
    std::array<std::uint8_t,0x3e0> storage{};
    std::span<std::uint8_t> record;
    std::array<std::span<std::uint8_t>,6> stat_bindings;
    std::unique_ptr<FieldActor> model,bag,box;
    std::optional<Model> bound_shadow;
    std::vector<std::uint8_t> record_before;
    std::vector<std::uint32_t> constructor_random_draws;
    std::filesystem::path model_path,bag_path,box_path;
    std::optional<GameActionRecord> action_record;
    std::optional<std::uint16_t> action_record_id;
    std::optional<BattleActionDescriptor> action_motion,action_secondary;
    unsigned entity_id=0;
    bool dead_callback=false;
    bool enemy=false;
    bool cartesian_motion=false;
    std::shared_ptr<const SceneModelAsset> model_asset,bag_asset,box_asset;
};
struct BattleEntityBinding {
    BattleActor* actor=nullptr;
    std::span<std::uint8_t> record;
    unsigned kind=4;
};
struct BattleStrikeServices {
    std::function<void(int)> sound;
    std::function<unsigned(int,const Float4&,bool source_w_available)> positioned_effect;
    std::function<void(int,int,int)> damage_event;
    std::function<void(BattleActor&)> enemy_reward;
    std::function<float(const BattleActor&)> approach_offset_w;
    //1684c0 writes the live HUD dirty flag; death entry cannot omit it.
    std::function<void()> refresh_hud;
    std::function<const BattleProjectile&(std::uint32_t source_pointer)> projectile;
    std::function<void(unsigned,const Float4&)> effect_position,effect_rotation;
    std::function<void(unsigned)> effect_stop;
    std::function<void(int row_offset)> camera_sequence;
    //156c70 substep0: 168380(action) writes the Waza name into the type3 HUD.
    std::function<void(std::int16_t action)> action_name;
};
using BattleDiagnosticLog=std::function<void(std::string_view)>;
void battle_refresh_entities(BattleState&,std::span<const BattleEntityBinding>);
void battle_register_action(BattleState&,unsigned owner,unsigned target,
                            std::uint32_t registration_flag);
//1588b0: inspect live graph heads and signed target bytes; no mutation.
unsigned battle_action_readiness(const BattleState&,unsigned owner,Bytes target_descriptor);
void battle_clear_action(BattleState&,unsigned owner); //158868, preserves links/counter.
//158b70: fixed 64 passes, indexes0..62 only; slot0 null returns immediately.
void battle_compact_actions(BattleState&,unsigned entity);
void battle_party_first_update(BattleActor&,BattleState&,std::span<const BattleEntityBinding> = {},
                               const BattleHitServices* =nullptr,const BattleDiagnosticLog& = {});  //153a20
void battle_party_second_update(BattleActor&,const BattleState&); //153ce8
void battle_enemy_first_update(BattleActor&,BattleState&,std::span<const BattleEntityBinding> = {},
                               const BattleHitServices* =nullptr,const BattleDiagnosticLog& = {});  //15b9a0
void battle_enemy_second_update(BattleActor&,const BattleState&); //15c138
void battle_actor_first_update(BattleActor&,BattleState&,std::span<const BattleEntityBinding>,
                               const BattleStrikeServices& = {},const BattleHitServices* =nullptr,
                               const BattleDiagnosticLog& = {});
//14b498: keep primary6 unchanged; otherwise reset live primary, then counter.
void battle_actor_reset_after_victory(BattleActor&);
void battle_actor_motion_update(BattleActor&,std::span<const BattleEntityBinding> = {},
                                const BattleState* =nullptr,const BattleDiagnosticLog& = {});
void battle_actor_second_suffix(BattleActor&);
class BattleRoster {
public:
    explicit BattleRoster(BattleState& root):root_(root) {}
    void construct_party(const std::filesystem::path& decoded,std::span<std::uint8_t> variables,
        const GameDataTables&,BattleEntryContext&,const std::function<std::uint32_t()>& random,
        const BattleModelAssetProvider& provider = {});
    void construct_enemies(const std::filesystem::path& decoded,unsigned formation_index,
        std::span<const BattleFormationEntry>,const GameDataTables&,int entry_half32,
        const std::function<std::uint32_t()>& random,const BattleModelAssetProvider& provider = {});
    const auto& actors() const {return actors_;}
    const auto& entities() const {return entities_;}
private:
    BattleState& root_;
    std::vector<std::unique_ptr<BattleActor>> actors_;
    std::vector<BattleEntityBinding> entities_;
    bool party_done_=false,enemies_done_=false;
    unsigned register_entity(BattleActor*,std::span<std::uint8_t>,unsigned kind);
};
}
