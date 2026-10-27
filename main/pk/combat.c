// Shared move selection and effects for wild and trainer battles. No PP economy.
#include "combat.h"
#include "battle.h"
#include "combat_moves.h"
#include "combat_auto_moves.h"
#include "combat_gen2_stats.h"
#include <stddef.h>
#include "combat_selected_moves.h"
static const combat_move_data_t *move_data(unsigned id){
 if(id>=1&&id<=165)return &COMBAT_DATA[id-1];
 for(unsigned i=0;i<sizeof(COMBAT_EXTRA)/sizeof(COMBAT_EXTRA[0]);i++)if(COMBAT_EXTRA[i].move.id==id)return &COMBAT_EXTRA[i];
 return NULL;
}
static bool stored_move(unsigned id){return !id||move_data(id)!=NULL;}
#include <string.h>
static uint32_t roll(uint32_t *rng,unsigned n){uint32_t x=*rng?*rng:1;x^=x<<13;x^=x>>17;x^=x<<5;*rng=x;return n?x%n:0;}
static unsigned minimum(unsigned a,unsigned b){return a<b?a:b;}
static void hurt(combat_mon_t *m,unsigned n){m->hp-=minimum(n,m->hp);}
#define SAFETY_MARKER_V12 0x5347
#define SAFETY_MARKER 0x5348
int8_t combat_sp_def_stage(const combat_mon_t *m){return m->safety.marker==SAFETY_MARKER?m->safety.special_defense:m->special;}
static void safety_init(combat_mon_t *m){
 if(m->safety.marker==SAFETY_MARKER)return;
 if(m->safety.marker!=SAFETY_MARKER_V12){memset(m->moves,0,sizeof(m->moves));m->safety.low_hp=m->hp;}
 m->safety.marker=SAFETY_MARKER;m->safety.special_defense=m->special;
}
static unsigned fatigue(const combat_mon_t *m){return m->safety.marker==SAFETY_MARKER?minimum(m->safety.fatigue,10):0;}
static unsigned heal(combat_mon_t *m,unsigned n){n=n*(10-fatigue(m))/10;n=minimum(n,m->max_hp-m->hp);m->hp+=n;return n;}
// Called once after both sides act, never for a failed-capture retaliation.
void combat_finish_round(combat_mon_t *a,combat_mon_t *d,struct battle_round *r){
 safety_init(a);safety_init(d);
 if(!a->hp||!d->hp)return; // A decisive attack keeps its normal outcome.
 bool progress=a->hp<a->safety.low_hp||d->hp<d->safety.low_hp;
 a->safety.low_hp=minimum(a->safety.low_hp,a->hp);d->safety.low_hp=minimum(d->safety.low_hp,d->hp);
 unsigned stalled=progress?0:minimum(a->safety.stalled,d->safety.stalled)+1;
 a->safety.stalled=d->safety.stalled=minimum(stalled,8);
 unsigned level=fatigue(a)>fatigue(d)?fatigue(a):fatigue(d);
 if(level||stalled>=8){
  level=minimum(level+1,10);a->safety.fatigue=d->safety.fatigue=level;
  hurt(a,(a->max_hp*level/20)?a->max_hp*level/20:1);
  hurt(d,(d->max_hp*level/20)?d->max_hp*level/20:1);
  r->fatigue=level;
 }
}
static bool stage(int8_t *s,int n){int old=*s,value=old+n;*s=value>6?6:value<-6?-6:value;return *s!=old;}
static uint8_t identity(const combat_mon_t *m){return m->transform_species?m->transform_species:m->species;}
static bool species_info(const combat_mon_t *m,species_t *out){
 if(!assets_species(identity(m),out))return false;
 if(identity(m)==81||identity(m)==82)out->type2=TY_STEEL;
 if(m->converted){out->type1=m->converted_type;out->type2=TY_NONE;}
 return true;
}
static uint8_t learn_level(const combat_mon_t *m){return m->transform_species?m->transform_level:m->level;}
bool combat_move(uint16_t id,move_t *out){if(!out)return false;if(!move_data(id)){memset(out,0,sizeof(*out));return false;}*out=move_data(id)->move;return true;}
uint8_t combat_learn_level(uint16_t species,uint16_t move){
 unsigned level=255;
 if(!move_data(move))return 255;
 if(species&&species<=151){const uint16_t *r=COMBAT_AUTO_RANGES[species-1];for(unsigned i=0;i<r[1];i++){const uint8_t *v=COMBAT_AUTO_LEARN[r[0]+i];if(v[1]==move&&v[0]<level)level=v[0];}}
 for(unsigned guard=0;species&&species<=151&&guard<4;guard++,species=COMBAT_PARENTS[species-1]){
  const uint16_t *range=COMBAT_LEARN_RANGES[species-1];
  for(unsigned i=0;i<range[1];i++){const uint8_t *r=COMBAT_LEARN[range[0]+i];if(r[1]==move&&r[0]<level)level=r[0];}
 }
 return level;
}
int combat_known_moves(uint16_t species,uint8_t level,uint16_t *out,int capacity){
 if(!out||capacity<=0||!species||species>151)return 0;
 int count=0;for(unsigned id=1;id<=COMBAT_MAX_MOVE_ID&&count<capacity;id++)if(combat_learn_level(species,id)<=level)out[count++]=id;
 return count;
}
uint16_t combat_stat(uint8_t species,uint8_t level,unsigned stat){
 if(!species||species>151||!level||level>100||stat>COMBAT_SP_DEFENSE)return 0;
 unsigned v=2u*(COMBAT_GEN2_STATS[species-1][stat]+15u)*level/100u;
 return v+(stat==COMBAT_HP?level+10u:5u);
}
uint16_t combat_max_hp(uint8_t species,uint8_t level){return combat_stat(species,level,COMBAT_HP);}
static uint16_t scaled(const combat_mon_t *m,unsigned stat,int st){
 unsigned v=combat_stat(identity(m),learn_level(m),stat);
 v=st>=0?v*(2+st)/2:v*2/(2-st);
 return v<1?1:minimum(v,999);
}
uint16_t combat_speed(const combat_mon_t *m){unsigned v=scaled(m,COMBAT_SPEED,m->speed);v=m->status==3?v/4:v;return v?v:1;}
void combat_init(combat_mon_t *m,uint8_t species,uint8_t level,uint16_t hp){memset(m,0,sizeof(*m));m->species=species;m->level=level;m->hp=m->max_hp=hp;safety_init(m);}
void combat_migrate_gen2(combat_mon_t *m){
 if(!m->max_hp)return;
 safety_init(m);
 unsigned old=m->max_hp,next=combat_max_hp(m->species,m->level);
 // Round surviving HP upward: a save upgrade must never faint a living partner.
 m->hp=m->hp?((uint32_t)m->hp*next+old-1)/old:0;
 m->substitute=(uint32_t)m->substitute*next/old;
 m->last_damage=(uint32_t)m->last_damage*next/old;
 m->bide_damage=(uint32_t)m->bide_damage*next/old;
 m->safety.low_hp=(uint32_t)m->safety.low_hp*next/old;
 m->max_hp=next;
}
void combat_reset_volatile(combat_mon_t *m){uint16_t hp=m->hp,max=m->max_hp;uint8_t sp=m->species,lv=m->level,status=m->status,sleep=m->sleep;combat_init(m,sp,lv,max);m->hp=hp;m->safety.low_hp=hp;m->status=status;m->sleep=sleep;}
bool combat_valid(const combat_mon_t *m){
 if(!m->species||m->species>151||!m->level||m->level>100||!m->max_hp||m->hp>m->max_hp||m->status>5||m->sleep>4||m->seeded>1)return false;
 const int8_t stages[]={m->attack,m->defense,m->special,combat_sp_def_stage(m),m->speed,m->accuracy,m->evasion};for(unsigned i=0;i<7;i++)if(stages[i]<-6||stages[i]>6)return false;
 return m->converted<=1&&m->converted_type<BATTLE_TYPE_COUNT&&stored_move(m->mimic_move)&&stored_move(m->last_move)&&stored_move(m->charge_move)&&stored_move(m->disabled_move)&&m->transform_species<=151&&m->transform_level<=100&&m->confusion<=4&&m->toxic<=15&&m->trap<=5&&m->recharge<=1&&m->charge<=1&&m->bide<=3&&m->flinch<=1&&m->reflect<=5&&m->light_screen<=5&&m->disable_turns<=5&&m->substitute<=m->max_hp/4;
}
static bool self_effect(unsigned e){switch(e){
 case EFFECT_ATTACK_UP:case EFFECT_ATTACK_UP_2:case EFFECT_SP_ATK_UP:case EFFECT_SP_DEF_UP_2:case EFFECT_DEFENSE_UP:case EFFECT_DEFENSE_UP_2:case EFFECT_SPEED_UP_2:case EFFECT_EVASION_UP:case EFFECT_DEFENSE_CURL:case EFFECT_HEAL:case EFFECT_MIST:case EFFECT_LIGHT_SCREEN:case EFFECT_REFLECT:case EFFECT_FOCUS_ENERGY:case EFFECT_SUBSTITUTE:case EFFECT_TRANSFORM:case EFFECT_MIMIC:case EFFECT_CONVERSION:case EFFECT_BIDE:case EFFECT_SPLASH:return true;default:return false;}}
static bool has_type(const combat_mon_t *m,unsigned type){species_t s;return species_info(m,&s)&&(s.type1==type||s.type2==type);}
static bool immune_status(const combat_mon_t *d,unsigned status){return d->status||d->substitute||(status==1&&(has_type(d,TY_POISON)||has_type(d,TY_STEEL)))||(status==2&&has_type(d,TY_FIRE))||(status==5&&has_type(d,TY_ICE));}
static bool fixed(unsigned effect){return effect==EFFECT_STATIC_DAMAGE||effect==EFFECT_LEVEL_DAMAGE||effect==EFFECT_PSYWAVE||effect==EFFECT_SUPER_FANG;}
static unsigned effectiveness(const combat_mon_t *d,const move_t *m){species_t sp;species_info(d,&sp);return m->id==165?100:battle_effectiveness(m->type,sp.type1,sp.type2);}
static unsigned estimate(const combat_mon_t *a,const combat_mon_t *d,const combat_move_data_t *data,unsigned ability,unsigned divisor,bool critical){
 const move_t *m=&data->move;unsigned mult=effectiveness(d,m);if(!mult)return 0;
 switch(data->effect){case EFFECT_STATIC_DAMAGE:return m->power;case EFFECT_LEVEL_DAMAGE:return a->level;case EFFECT_SUPER_FANG:return d->hp/2?d->hp/2:1;case EFFECT_PSYWAVE:return a->level*3/4+1;case EFFECT_OHKO:return a->level>=d->level?d->hp:0;case EFFECT_COUNTER:return a->last_damage*2;case EFFECT_BIDE:return a->bide_damage*2;default:break;}
 species_t as;species_info(a,&as);
 int astage=m->special?a->special:a->attack,dstage=m->special?combat_sp_def_stage(d):d->defense;
 bool ignore=critical&&astage<=dstage;
 unsigned A=scaled(a,m->special?COMBAT_SP_ATTACK:COMBAT_ATTACK,ignore?0:astage),D=scaled(d,m->special?COMBAT_SP_DEFENSE:COMBAT_DEFENSE,ignore?0:dstage);
 if(!ignore&&a->status==2&&!m->special)A/=2;
 A=A*ability/1024;if(!A)A=1;
 if(!ignore&&((m->special&&d->light_screen)||(!m->special&&d->reflect)))D*=2;
 // Original damage operands are reduced together when either exceeds a byte.
 // Keep wide intermediates rather than reproducing the Game Boy overflow bugs.
 while(A>255||D>255){A=A/4?A/4:1;D=D/4?D/4:1;}
 if(!A)A=1;
 if(data->effect==EFFECT_SELFDESTRUCT)D/=2;
 if(!D)D=1;
 unsigned value=(uint64_t)(2u*a->level/5+2)*A*m->power/D/divisor;
 value=minimum(value*(critical?2:1),997)+2;
 if(m->id!=165&&(as.type1==m->type||as.type2==m->type))value=value*3/2;
 value=value*mult/100;

 return value?value:1;
}
static unsigned utility(const combat_mon_t *a,const combat_mon_t *d,const combat_move_data_t *m){
 unsigned e=m->effect;
 if(e==EFFECT_FORCE_SWITCH||e==EFFECT_TELEPORT||e==EFFECT_SPLASH)return 0; // No overworld escape/forced switching during auto duels.
 if(e==EFFECT_HEAL){if(a->hp*100u>=a->max_hp*60u)return 0;unsigned score=minimum(a->max_hp-a->hp,a->max_hp/2);return a->last_move==m->move.id?score/4:score*2;}
 if(e==EFFECT_LEECH_SEED)return !d->seeded&&!d->substitute&&!has_type(d,TY_GRASS)?d->max_hp/5+10:0;
 if(e==EFFECT_SLEEP)return immune_status(d,4)?0:40;
 if(e==EFFECT_POISON||e==EFFECT_TOXIC)return immune_status(d,1)?0:35;
 if(e==EFFECT_PARALYZE)return immune_status(d,3)||(m->move.id==86&&has_type(d,TY_GROUND))?0:30;
 if(e==EFFECT_CONFUSE)return d->confusion||d->substitute?0:30;
 if(e==EFFECT_DISABLE)return !d->last_move||d->disable_turns||d->substitute?0:20;
 if(e==EFFECT_ATTACK_UP||e==EFFECT_ATTACK_UP_2)return a->attack>=2?0:30;
 if(e==EFFECT_DEFENSE_UP||e==EFFECT_DEFENSE_UP_2||e==EFFECT_DEFENSE_CURL)return a->defense>=2?0:25;
 if(e==EFFECT_SP_ATK_UP)return a->special>=2?0:30;
 if(e==EFFECT_SP_DEF_UP_2)return combat_sp_def_stage(a)>=2?0:30;
 if(e==EFFECT_SPEED_UP_2)return combat_speed(a)>=combat_speed(d)||a->speed>=2?0:25;
 if(e==EFFECT_EVASION_UP)return a->evasion>=2?0:25;
 if(e==EFFECT_ACCURACY_DOWN)return d->accuracy<=-2||d->mist||d->substitute?0:25;
 if(e==EFFECT_DEFENSE_DOWN||e==EFFECT_DEFENSE_DOWN_2)return d->defense<=-2||d->mist||d->substitute?0:25;
 if(e==EFFECT_ATTACK_DOWN||e==EFFECT_ATTACK_DOWN_2)return d->attack<=-2||d->mist||d->substitute?0:25;
 if(e==EFFECT_SPEED_DOWN)return combat_speed(a)>=combat_speed(d)||d->speed<=-2||d->mist||d->substitute?0:20;
 if(e==EFFECT_REFLECT)return a->reflect?0:25;
 if(e==EFFECT_LIGHT_SCREEN)return a->light_screen?0:25;
 if(e==EFFECT_MIST)return a->mist?0:10;
 if(e==EFFECT_FOCUS_ENERGY)return a->focus?0:20;
 if(e==EFFECT_TRANSFORM)return a->transform_species?0:80;
 if(e==EFFECT_CONVERSION)return a->converted?0:15;
 if(e==EFFECT_MIMIC)return a->mimic_move||!d->last_move?0:20;
 if(e==EFFECT_MIRROR_MOVE)return d->last_move&&d->last_move!=m->move.id?20:0;
 if(e==EFFECT_METRONOME)return 35;
 if(e==EFFECT_RESET_STATS)return d->attack+d->defense+d->special+combat_sp_def_stage(d)+d->speed>0||a->attack+a->defense+a->special+combat_sp_def_stage(a)+a->speed<0?35:0;
 if(e==EFFECT_SUBSTITUTE)return a->substitute||a->hp<=a->max_hp/2?0:25;
 if(e==EFFECT_DREAM_EATER&&d->status!=4)return 0;
 if(e==EFFECT_COUNTER){move_t last;if(!a->last_damage||!combat_move(d->last_move,&last)||last.special)return 0;}
 if(e==EFFECT_BIDE)return a->hp>a->max_hp/2?10:0;
 unsigned score=estimate(a,d,m,1024,50,false);if(e==EFFECT_FALSE_SWIPE&&!d->substitute)score=minimum(score,d->hp?d->hp-1:0);if(!score)return 0;
 if(e==EFFECT_MULTI_HIT)score*=3;
 if(e==EFFECT_DOUBLE_HIT||e==EFFECT_POISON_MULTI_HIT)score*=2;
 if(e==EFFECT_FLY||e==EFFECT_SOLARBEAM||e==EFFECT_SKY_ATTACK||e==EFFECT_SKULL_BASH||e==EFFECT_RAZOR_WIND||e==EFFECT_HYPER_BEAM)score=score*2/3;
 if(e==EFFECT_SELFDESTRUCT)score=a->hp*3>a->max_hp?score/10:score/2;
 if(e==EFFECT_RECOIL_HIT&&a->hp<a->max_hp/4)score/=2;
 if(score>=d->hp)score+=40;
 score=score*(m->move.accuracy==255?100:m->move.accuracy)/100;
 return score;
}
static bool direct_attack(const combat_mon_t *a,const combat_mon_t *d,const combat_move_data_t *m){
 if(!(m->move.power||fixed(m->effect)||m->effect==EFFECT_OHKO||m->effect==EFFECT_COUNTER))return false;
 return utility(a,d,m)>0;
}
uint16_t combat_choose(const combat_mon_t *a,const combat_mon_t *d,uint32_t *rng){
 if(a->charge)return a->charge_move;
 if(a->bide)return 117;
 uint16_t ids[COMBAT_MOVE_CAP],selected=165;int count=combat_known_moves(identity(a),learn_level(a),ids,COMBAT_MOVE_CAP);
 bool can_attack=false,can_transform=false;
 for(int i=0;i<count;i++){
  unsigned id=ids[i]==102&&a->mimic_move?a->mimic_move:ids[i];
  if(a->disable_turns&&a->disabled_move==id)continue;
  can_attack|=direct_attack(a,d,move_data(id));
  can_transform|=id==144&&!a->transform_species;
 }
 if(!can_attack)return can_transform?144:165;
 bool must_attack=a->safety.marker==SAFETY_MARKER&&a->safety.status_streak>=2;
 uint32_t total=0;
 for(int i=0;i<count;i++){
  unsigned id=ids[i]==102&&a->mimic_move?a->mimic_move:ids[i];if(a->disable_turns&&a->disabled_move==id)continue;
  if(must_attack&&!direct_attack(a,d,move_data(id)))continue;
  unsigned score=utility(a,d,move_data(id));if(!score)continue;
  if(a->last_move==id)score=score*3/4+1;
  if(score>500)score=500;
  // Effective attacks get 3x the lottery weight; utility still distinguishes tactics.
  unsigned weight=score*score*(direct_attack(a,d,move_data(id))?3u:1u);total+=weight;
  if(roll(rng,total)<weight)selected=id;
 }
 // Retain truly non-damaging moves in the catalogue without trapping gameplay forever.
 return selected;
}
static void apply_status(combat_mon_t *d,unsigned status,uint32_t *rng){if(immune_status(d,status))return;d->status=status;d->toxic=0;if(status==4)d->sleep=2+roll(rng,3);}
static bool status_effect(combat_mon_t *a,combat_mon_t *d,const combat_move_data_t *m,uint32_t *rng){
 unsigned e=m->effect;if(!self_effect(e)&&d->substitute)return false;
 switch(e){
 case EFFECT_HEAL:if(a->hp==a->max_hp&&!(m->move.id==156&&a->status))return false;if(m->move.id==156){heal(a,a->max_hp);a->status=4;a->sleep=2;a->toxic=0;}else heal(a,a->max_hp/2);return true;
 case EFFECT_SLEEP:if(immune_status(d,4))return false;apply_status(d,4,rng);return true;
 case EFFECT_POISON:case EFFECT_TOXIC:if(immune_status(d,1))return false;apply_status(d,1,rng);d->toxic=e==EFFECT_TOXIC?1:0;return true;
 case EFFECT_PARALYZE:if(immune_status(d,3)||(m->move.id==86&&has_type(d,TY_GROUND)))return false;apply_status(d,3,rng);return true;
 case EFFECT_CONFUSE:if(d->confusion)return false;d->confusion=2+roll(rng,3);return true;
 case EFFECT_LEECH_SEED:if(d->seeded||has_type(d,TY_GRASS))return false;d->seeded=1;return true;
 case EFFECT_DISABLE:if(!d->last_move||d->disable_turns)return false;d->disabled_move=d->last_move;d->disable_turns=3+roll(rng,3);return true;
 case EFFECT_ATTACK_UP:return stage(&a->attack,1);case EFFECT_ATTACK_UP_2:return stage(&a->attack,2);
 case EFFECT_DEFENSE_UP:case EFFECT_DEFENSE_CURL:return stage(&a->defense,1);case EFFECT_DEFENSE_UP_2:return stage(&a->defense,2);
 case EFFECT_SP_ATK_UP:return stage(&a->special,1);case EFFECT_SP_DEF_UP_2:return stage(&a->safety.special_defense,2);
 case EFFECT_SPEED_UP_2:return stage(&a->speed,2);case EFFECT_EVASION_UP:return stage(&a->evasion,1);
 case EFFECT_ACCURACY_DOWN:return !d->mist&&stage(&d->accuracy,-1);case EFFECT_ATTACK_DOWN:return !d->mist&&stage(&d->attack,-1);
 case EFFECT_DEFENSE_DOWN:return !d->mist&&stage(&d->defense,-1);case EFFECT_DEFENSE_DOWN_2:return !d->mist&&stage(&d->defense,-2);
 case EFFECT_ATTACK_DOWN_2:return !d->mist&&stage(&d->attack,-2);
 case EFFECT_SPEED_DOWN:return !d->mist&&stage(&d->speed,-1);
 case EFFECT_REFLECT:if(a->reflect)return false;a->reflect=5;return true;
 case EFFECT_LIGHT_SCREEN:if(a->light_screen)return false;a->light_screen=5;return true;
 case EFFECT_MIST:if(a->mist)return false;a->mist=1;return true;
 case EFFECT_FOCUS_ENERGY:if(a->focus)return false;a->focus=1;return true;
 case EFFECT_RESET_STATS:a->safety.special_defense=d->safety.special_defense=0;a->attack=a->defense=a->special=a->speed=a->accuracy=a->evasion=0;d->attack=d->defense=d->special=d->speed=d->accuracy=d->evasion=0;return true;
 case EFFECT_SUBSTITUTE:if(a->substitute||a->hp<=a->max_hp/4)return false;a->substitute=a->max_hp/4;hurt(a,a->substitute);return true;
 case EFFECT_MIMIC:if(!d->last_move||d->last_move==102||d->last_move==165)return false;a->mimic_move=d->last_move;return true;
 case EFFECT_CONVERSION:{uint16_t ids[COMBAT_MOVE_CAP];int n=combat_known_moves(identity(a),learn_level(a),ids,COMBAT_MOVE_CAP);for(int i=0;i<n;i++){unsigned type=move_data(ids[i])->move.type;if(!has_type(a,type)){a->converted=1;a->converted_type=type;return true;}}return false;}
 case EFFECT_TRANSFORM:if(a->transform_species||d->transform_species)return false;a->transform_species=d->species;a->transform_level=d->level;a->attack=d->attack;a->defense=d->defense;a->special=d->special;a->safety.special_defense=combat_sp_def_stage(d);a->speed=d->speed;return true;
 default:return false;
 }
}
static void residual(combat_mon_t *a,combat_mon_t *d){
 if(a->hp&&(a->status==1||a->status==2)){unsigned n=a->max_hp/8;if(a->toxic){n=(a->max_hp/16?a->max_hp/16:1)*a->toxic;if(a->toxic<15)a->toxic++;}hurt(a,n?n:1);}
 if(a->hp&&a->seeded){unsigned n=minimum(a->hp,a->max_hp/8?a->max_hp/8:1);hurt(a,n);if(d->hp)heal(d,n);}
 if(a->hp&&a->trap){hurt(a,a->max_hp/16?a->max_hp/16:1);a->trap--;}
 if(a->reflect)a->reflect--;
 if(a->light_screen)a->light_screen--;
 if(a->disable_turns)a->disable_turns--;
}
void combat_turn(combat_mon_t *a,combat_mon_t *d,uint16_t ability,uint32_t *rng,unsigned divisor,uint16_t forced_move,struct battle_round *r){
 safety_init(a);safety_init(d);
 r->mult=100;unsigned before=a->hp;bool skip=false;
 d->last_damage=0;
 if(a->recharge){a->recharge=0;skip=true;r->skipped=6;}
 else if(a->flinch){a->flinch=0;skip=true;r->skipped=4;}
 else if(a->status==4){if(a->sleep){a->sleep--;skip=true;r->skipped=1;}else a->status=0;}
 else if(a->status==5){if(forced_move==172||(!forced_move&&combat_learn_level(identity(a),172)<=learn_level(a)))forced_move=172;if(forced_move==172||roll(rng,100)<20)a->status=0;else{skip=true;r->skipped=2;}}
 else if(a->status==3&&roll(rng,100)<25){skip=true;r->skipped=3;}
 if(!skip&&a->confusion){a->confusion--;if(roll(rng,2)==0){hurt(a,a->max_hp/8?a->max_hp/8:1);skip=true;r->skipped=5;}}
 if(skip){a->charge=0;residual(a,d);return;}
 unsigned id=forced_move?forced_move:combat_choose(a,d,rng);if(!move_data(id))id=165;
 unsigned effect=move_data(id)->effect;
 if(effect==EFFECT_METRONOME){do{id=1+roll(rng,COMBAT_MOVE_CAP);if(id>165)id=COMBAT_EXTRA[id-166].move.id;effect=move_data(id)->effect;}while(effect==EFFECT_METRONOME||effect==EFFECT_MIMIC||effect==EFFECT_MIRROR_MOVE||effect==EFFECT_FORCE_SWITCH||effect==EFFECT_TELEPORT||effect==EFFECT_CONVERSION);}
 else if(effect==EFFECT_MIRROR_MOVE){if(d->last_move&&d->last_move!=102&&d->last_move!=119&&d->last_move!=118)id=d->last_move;else r->no_effect=1;}
 const combat_move_data_t *data=move_data(id);const move_t *m=&data->move;effect=data->effect;
 bool is_attack=m->power||fixed(effect)||effect==EFFECT_COUNTER||effect==EFFECT_BIDE||effect==EFFECT_OHKO;
 a->safety.status_streak=is_attack?0:minimum(a->safety.status_streak+1,2);
 r->move_id=id;r->move_type=m->type;r->move_zh=m->name_zh;r->move_zh_len=m->name_zh_len;r->self_target=self_effect(effect);a->last_move=id;
 if(r->no_effect){residual(a,d);return;}
 bool charging=effect==EFFECT_FLY||effect==EFFECT_SOLARBEAM||effect==EFFECT_SKY_ATTACK||effect==EFFECT_SKULL_BASH||effect==EFFECT_RAZOR_WIND;
 if(charging&&!a->charge){a->charge=1;a->charge_move=id;r->charging=1;r->self_target=1;if(effect==EFFECT_SKULL_BASH)stage(&a->defense,1);residual(a,d);return;}
 if(charging){a->charge=0;a->charge_move=0;}
 if(effect==EFFECT_BIDE){if(!a->bide){a->bide=2;a->bide_damage=0;r->charging=1;r->self_target=1;residual(a,d);return;}if(--a->bide){r->charging=1;r->self_target=1;residual(a,d);return;}r->self_target=0;}
 unsigned accuracy=m->accuracy;int acc=a->accuracy-d->evasion;if(acc>6)acc=6;if(acc<-6)acc=-6;
 if(accuracy!=255&&!r->self_target)accuracy=acc>=0?accuracy*(3+acc)/3:accuracy*3/(3-acc);
 if(effect==EFFECT_OHKO)accuracy=a->level>=d->level?minimum(100,30+a->level-d->level):0;
 bool avoid=d->charge&&(d->charge_move==19||d->charge_move==91);
 if(avoid&&((d->charge_move==19&&(id==16||id==87||id==239))||(d->charge_move==91&&id==89)))avoid=false;
 r->missed=!r->self_target&&(avoid||(m->accuracy!=255&&roll(rng,100)>=accuracy));
 if(r->missed){if(effect==EFFECT_SELFDESTRUCT)a->hp=0;if(effect==EFFECT_JUMP_KICK)hurt(a,a->max_hp/8?a->max_hp/8:1);residual(a,d);return;}
 bool damaging=m->power>0||effect==EFFECT_COUNTER||effect==EFFECT_BIDE||effect==EFFECT_OHKO||effect==EFFECT_PSYWAVE;
 if(!damaging){r->no_effect=!status_effect(a,d,data,rng);r->healed=a->hp>before?a->hp-before:0;residual(a,d);return;}
 r->mult=effectiveness(d,m);
 if(effect==EFFECT_DREAM_EATER&&d->status!=4)r->no_effect=1;
 if(effect==EFFECT_COUNTER){move_t last;if(!a->last_damage||!combat_move(d->last_move,&last)||last.special)r->no_effect=1;}
 if(!r->mult||r->no_effect){if(effect==EFFECT_SELFDESTRUCT)a->hp=0;r->no_effect=1;residual(a,d);return;}
 bool variable=!fixed(effect)&&effect!=EFFECT_OHKO&&effect!=EFFECT_COUNTER&&effect!=EFFECT_BIDE;
 if(variable){
  static const uint8_t crit_chance[]={17,32,64,85,128};
  unsigned crit=(a->focus?1:0)+((id==2||id==75||id==152||id==163||id==238)?2:0);
  r->critical=roll(rng,256)<crit_chance[crit];
 }
 unsigned value=estimate(a,d,data,ability,divisor,r->critical);
 if(!value){r->no_effect=1;residual(a,d);return;}
 if(effect==EFFECT_PSYWAVE)value=1+roll(rng,a->level*3/2?a->level*3/2:1);
 if(variable){
  value=value*(217+roll(rng,39))/255;if(!value)value=1;
 }else r->mult=100;
 if(d->charge&&((d->charge_move==19&&(id==16||id==239))||(d->charge_move==91&&id==89)))value*=2;
 unsigned hits=1;
 if(effect==EFFECT_DOUBLE_HIT||effect==EFFECT_POISON_MULTI_HIT)hits=2;
 if(effect==EFFECT_MULTI_HIT){static const uint8_t distribution[]={2,2,2,3,3,3,4,5};hits=distribution[roll(rng,8)];}
 bool shielded=d->substitute>0;unsigned actual=0;
 for(unsigned i=0;i<hits&&d->hp;i++){r->hits++;if(d->substitute){unsigned n=minimum(value,d->substitute);d->substitute-=n;actual+=n;}else{unsigned n=minimum(value,d->hp);if(effect==EFFECT_FALSE_SWIPE&&n>=d->hp)n=d->hp-1;hurt(d,n);r->damage+=n;actual+=n;}}
 d->last_damage=r->damage;if(d->bide)d->bide_damage+=r->damage;
 if(d->rage&&r->damage)stage(&d->attack,1);
 if(effect==EFFECT_RAGE)a->rage=1;
 if(effect==EFFECT_BIDE){a->bide=0;a->bide_damage=0;}
 if(effect==EFFECT_HYPER_BEAM)a->recharge=1;
 if(effect==EFFECT_LEECH_HIT||effect==EFFECT_DREAM_EATER)r->healed=heal(a,actual/2?actual/2:1);
 if(effect==EFFECT_RECOIL_HIT&&id!=165)hurt(a,actual/4?actual/4:1);
 if(id==165)hurt(a,a->max_hp/4?a->max_hp/4:1);
 if(effect==EFFECT_SELFDESTRUCT)a->hp=0;
 if(effect==EFFECT_RAMPAGE){if(!a->charge_move){a->charge_move=id;a->charge=1;}else{a->charge=0;a->charge_move=0;a->confusion=2+roll(rng,3);}}
 if(actual&&(effect==EFFECT_DEFENSE_UP_HIT||effect==EFFECT_ALL_UP_HIT)&&roll(rng,100)<data->chance){stage(&a->defense,1);if(effect==EFFECT_ALL_UP_HIT){stage(&a->attack,1);stage(&a->special,1);stage(&a->safety.special_defense,1);stage(&a->speed,1);}}
 if(d->hp&&!shielded){
  if(effect==EFFECT_TRAP_TARGET)d->trap=2+roll(rng,4);
  if(data->chance&&roll(rng,100)<data->chance){switch(effect){
   case EFFECT_POISON_HIT:case EFFECT_POISON_MULTI_HIT:apply_status(d,1,rng);break;
   case EFFECT_FLAME_WHEEL:case EFFECT_BURN_HIT:apply_status(d,2,rng);break;case EFFECT_PARALYZE_HIT:case EFFECT_THUNDER:apply_status(d,3,rng);break;case EFFECT_FREEZE_HIT:apply_status(d,5,rng);break;
   case EFFECT_CONFUSE_HIT:if(!d->confusion)d->confusion=2+roll(rng,3);break;
   case EFFECT_TWISTER:case EFFECT_FLINCH_HIT:case EFFECT_STOMP:d->flinch=1;break;
   case EFFECT_ACCURACY_DOWN_HIT:if(!d->mist)stage(&d->accuracy,-1);break;
   case EFFECT_ATTACK_DOWN_HIT:if(!d->mist)stage(&d->attack,-1);break;
   case EFFECT_DEFENSE_DOWN_HIT:if(!d->mist)stage(&d->defense,-1);break;
   case EFFECT_SPEED_DOWN_HIT:if(!d->mist)stage(&d->speed,-1);break;
   case EFFECT_SP_DEF_DOWN_HIT:if(!d->mist)stage(&d->safety.special_defense,-1);break;
   case EFFECT_TRI_ATTACK:apply_status(d,(unsigned[]){2,3,5}[roll(rng,3)],rng);break;
   default:break;
  }}
 }
 residual(a,d);
}

const char *combat_description(uint16_t id){
 if(!move_data(id))return "未知招式";
 switch(move_data(id)->effect){
 case EFFECT_FLAME_WHEEL:return "解除自身冰冻 可能灼伤";
 case EFFECT_ATTACK_DOWN_2:return "大幅降低对手攻击";
 case EFFECT_ACCURACY_DOWN_HIT:return "攻击并降低对手命中";
 case EFFECT_DEFENSE_UP_HIT:return "攻击时可能提升自身防御";
 case EFFECT_ALL_UP_HIT:return "可能提升五项战斗能力";
 case EFFECT_FALSE_SWIPE:return "至少给对手保留一点体力";
 case EFFECT_TWISTER:return "可能畏缩 对飞翔伤害翻倍";
 case EFFECT_SLEEP:return "使对手睡眠";case EFFECT_PARALYZE:case EFFECT_PARALYZE_HIT:case EFFECT_THUNDER:return "可能使对手麻痹";
 case EFFECT_POISON:case EFFECT_POISON_HIT:case EFFECT_POISON_MULTI_HIT:return "可能使对手中毒";case EFFECT_TOXIC:return "剧毒伤害逐次增加";
 case EFFECT_BURN_HIT:return "可能灼伤 降低物攻";case EFFECT_FREEZE_HIT:return "可能使对手冰冻";
 case EFFECT_CONFUSE:case EFFECT_CONFUSE_HIT:return "使对手混乱 可能自伤";
 case EFFECT_LEECH_SEED:return "每回合吸取对手体力";
 case EFFECT_LEECH_HIT:case EFFECT_DREAM_EATER:return id==138?"睡眠时吸取对手体力":"恢复实际伤害的一半";
 case EFFECT_HEAL:return id==156?"完全回复 并睡眠两回合":"恢复一半最大体力";
 case EFFECT_MULTI_HIT:return "连续攻击二至五次";case EFFECT_DOUBLE_HIT:return "连续攻击两次";
 case EFFECT_RECOIL_HIT:return "强力攻击 自身受反伤";case EFFECT_SELFDESTRUCT:return "大爆发后自身倒下";
 case EFFECT_FLY:case EFFECT_SOLARBEAM:case EFFECT_SKY_ATTACK:case EFFECT_SKULL_BASH:case EFFECT_RAZOR_WIND:return "蓄力一回合后攻击";
 case EFFECT_HYPER_BEAM:return "强力攻击后休息一回合";
 case EFFECT_STATIC_DAMAGE:case EFFECT_LEVEL_DAMAGE:case EFFECT_PSYWAVE:return "固定伤害 无视攻防";case EFFECT_SUPER_FANG:return "削减对手当前一半体力";
 case EFFECT_OHKO:return "等级不低时概率一击倒下";
 case EFFECT_COUNTER:return "双倍返还受到的物理伤害";case EFFECT_BIDE:return "忍耐后返还累计伤害";
 case EFFECT_ATTACK_UP:case EFFECT_ATTACK_UP_2:return "提升自身攻击";case EFFECT_DEFENSE_UP:case EFFECT_DEFENSE_UP_2:case EFFECT_DEFENSE_CURL:return "提升自身防御";
 case EFFECT_SP_ATK_UP:return "提升自身特攻";case EFFECT_SP_DEF_UP_2:return "大幅提升自身特防";case EFFECT_SPEED_UP_2:return "提升速度 改变出手顺序";
 case EFFECT_DEFENSE_DOWN:case EFFECT_DEFENSE_DOWN_2:case EFFECT_DEFENSE_DOWN_HIT:return "降低对手防御";
 case EFFECT_ATTACK_DOWN:case EFFECT_ATTACK_DOWN_HIT:return "降低对手攻击";
 case EFFECT_SPEED_DOWN:case EFFECT_SPEED_DOWN_HIT:return "降低对手速度";case EFFECT_SP_DEF_DOWN_HIT:return "可能降低对手特防";
 case EFFECT_ACCURACY_DOWN:return "降低对手命中";case EFFECT_EVASION_UP:return "提高自身闪避";
 case EFFECT_REFLECT:return "暂时减半物理伤害";case EFFECT_LIGHT_SCREEN:return "暂时减半特殊伤害";
 case EFFECT_MIST:return "防止能力被对手降低";case EFFECT_RESET_STATS:return "清除双方能力变化";case EFFECT_FOCUS_ENERGY:return "提高击中要害概率";
 case EFFECT_SUBSTITUTE:return "消耗体力制造替身";case EFFECT_TRANSFORM:return "复制对方形态与技能";
 case EFFECT_DISABLE:return "暂时封住对手上一招";
 case EFFECT_METRONOME:return "随机使出一种招式";case EFFECT_MIMIC:return "学会对手上一招直到换人";case EFFECT_MIRROR_MOVE:return "使用对手上一招";
 case EFFECT_CONVERSION:return "变为已学技能的属性";
 case EFFECT_TRAP_TARGET:return "束缚对手 持续造成伤害";
 case EFFECT_FLINCH_HIT:case EFFECT_STOMP:return "先手命中可能使对手畏缩";
 case EFFECT_RAMPAGE:return "连续攻击后陷入混乱";
 case EFFECT_RAGE:return "受到攻击时提升物攻";
 case EFFECT_ALWAYS_HIT:return "忽略命中与闪避变化";
 case EFFECT_FORCE_SWITCH:case EFFECT_TELEPORT:return "此对战模式不自动使用";
 case EFFECT_SPLASH:return "只是跳跃 不产生伤害";
 default:return "依据属性与攻防造成伤害";
 }
}
const char *combat_feedback(const struct battle_round *r){
 if(r->fatigue)return r->fatigue==1?"陷入苦战 体力开始消耗":"苦战加剧 回复效果降低";
 if(r->skipped){static const char *messages[]={"","正在睡眠","身体被冻住了","麻痹 无法行动","畏缩 无法行动","混乱中伤到了自己","正在恢复行动"};return r->skipped<=6?messages[r->skipped]:"无法行动";}
 if(r->charging)return "正在积蓄力量";
 if(r->missed)return "攻击没有命中";
 if(r->no_effect||!r->mult)return "没有产生效果";
 if(r->healed)return "体力得到回复";
 if(r->self_target)return "自身状态发生变化";
 if(r->hits>1)return "连续攻击命中";
 if(!r->damage)return "对手状态发生变化";
 return r->critical?"击中了要害！":battle_eff_label(r->mult);
}

int combat_priority(uint16_t move){return move==98||move==183||move==245?1:move==68?-1:0;}
