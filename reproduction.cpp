#include "stdafx.h"
#include "reproduction.h"
#include "collective.h"
#include "collective_config.h"
#include "monster_ai.h"
#include "creature.h"
#include "creature_attributes.h"
#include "territory.h"
#include "game.h"
#include "furniture_factory.h"
#include "construction_map.h"
#include "collective_teams.h"
#include "furniture.h"
#include "item_index.h"
#include "technology.h"
#include "level.h"
#include "game.h"
#include "collective_name.h"
#include "clock.h"
#include "tutorial.h"
#include "container_range.h"
#include "creature_factory.h"
#include "resource_info.h"
#include "equipment.h"
#include "player_control.h"
#include "view_object.h"
#include "content_factory.h"
#include "immigrant_info.h"
#include "special_trait.h"
#include "item.h"
#include "effect_type.h"
#include "storage_id.h"


Reproduction::Reproduction(Collective* collective, const vector<ImmigrantInfo>* immigrants)
	: collective(collective), immigrants(immigrants)
{
	vector<CreatureId> definedCreatures;
	for (auto elem : Iter(*immigrants)){
		if(elem->getTraits().contains(MinionTrait::WORKER)){
			continue;
		}
		auto creatureId = elem->getNonRandomId(0);
		if(definedCreatures.size() <= 2){
			initialCreatures.push_back(creatureId);
			std::cout<<"Reproduction " << creatureId << " Initial " << std::endl;
		} else {
			auto crossing = GetNewCrossing(definedCreatures, creatureId);
			if(crossing.has_value()){
				crossings.push_back(crossing.value());
				std::cout<<"Reproduction " << creatureId << " Crossed from " << crossing->parent1 << " + " << crossing->parent2 << std::endl;
			} else {
				initialCreatures.push_back(creatureId);
			}
		}
		definedCreatures.push_back(creatureId);
	}
}

optional<Reproduction::Crossing> Reproduction::GetNewCrossing(const vector<CreatureId>& definedCreatures, CreatureId child){
	for(int i = 0; i < 100; i++){
		auto parent1 = Random.choose(definedCreatures);
		auto parent2 = Random.choose(definedCreatures);
		if(parent1 == parent2){
			continue;
		}
		if(tryGetCrossing(parent1, parent2).has_value()){
			continue;
		}
		Reproduction::Crossing c;
		c.parent1 = parent1;
		c.parent2 = parent2;
		c.child = child;
		return c;
	}
	return {};
}

optional<Reproduction::Crossing> Reproduction::tryGetCrossing(CreatureId parent1, CreatureId parent2){
	for (auto elem : Iter(crossings)){
		if((elem->parent1 == parent1 && elem->parent2 == parent2) 
		|| (elem->parent1 == parent2 && elem->parent2 == parent1) ){
			Reproduction::Crossing crossing = *elem;
			return crossing;
		}
	}	
	return {};
}

Reproduction::~Reproduction(){
}

const vector<CreatureId>& Reproduction::getInitialCreatureIds() const{
	return initialCreatures;
}

PCreature Reproduction::reproduce(const Creature* parent1, const Creature* parent2){		
	std::cout<<"reproduce of " << parent1->getName().a() << std::endl;	
	CreatureId parent1Id = parent1->getAttributes().getCreatureId().value();
	if(parent2){
		if(parent1Id == parent2->getAttributes().getCreatureId()){
			std::cout<<"Same specy reproduction " << std::endl;	
			return generateChild(parent1, parent2);
		} 	
		CreatureId parent2Id = parent2->getAttributes().getCreatureId().value();
		std::cout<<"Crossing with " << parent2->getName().a() << std::endl;	
		auto crossing = tryGetCrossing(parent1Id, parent2Id);
		if(crossing.has_value()){
			std::cout<<"Has crossing" << std::endl;	
			return generateFirstGeneration(crossing->child);
		}	
		std::cout<<"No crossing found" << std::endl;	
	}	
	return generateFirstGeneration(initialCreatures.front());
}

PCreature Reproduction::generateFirstGeneration(CreatureId creatureId){
	std::cout<<"generateFirstGeneration of " <<  creatureId << std::endl;	
	return generateFirstGeneration(creatureId, Random.choose(Gender::MALE, Gender::FEMALE));
}

PCreature Reproduction::generateFirstGeneration(CreatureId creatureId, Gender gender) {
	std::cout<<"generateFirstGeneration of " <<  creatureId << ", gender: " << gender << std::endl;	
	auto contentFactory = collective->getGame()->getContentFactory();
	PCreature creature = contentFactory->getCreatures().fromId(creatureId, collective->getTribeId(), MonsterAIFactory::collective(collective));
	creature->getEquipment().removeAllItems(creature.get());
	creature->getAttributes().setGender(gender);
	creature->getAttributes().addPermanentEffect(LastingEffect::COPULATION_SKILL, 1);
	creature->getAttributes().addGeneticTalent(Random.choose(LastingEffect::MAGIC_VULNERABILITY, LastingEffect::BAD_BREATH, LastingEffect::MELEE_VULNERABILITY));
	creature->getAttributes().addPermanentEffect(LastingEffect::FIRST_GENERATION, 1);
	
	std::cout << "generateFirstGeneration done" << std::endl;	
	return creature;
}

PCreature Reproduction::generateChild(const Creature* parent1, const Creature* parent2) {
	CreatureId creatureId = parent1->getAttributes().getCreatureId().value();
	Gender gender = Random.choose(parent1->getAttributes().getGender(), parent2->getAttributes().getGender());
	std::cout<<"generateChild  " <<  creatureId << ", gender: " << gender << std::endl;	
	auto contentFactory = collective->getGame()->getContentFactory();
	PCreature creature = contentFactory->getCreatures().fromId(creatureId, collective->getTribeId(), MonsterAIFactory::collective(collective));
	creature->getEquipment().removeAllItems(creature.get());
	creature->getAttributes().setGender(gender);
	creature->getAttributes().addPermanentEffect(LastingEffect::COPULATION_SKILL, 1);
	set<LastingEffect> uniqueTalent;

	vector<const Creature*> bothParents;
	bothParents.push_back(parent1);
	bothParents.push_back(parent2);
	for(const Creature* parent : bothParents){
		for(LastingEffect talent : parent->getAttributes().getGeneticTalents()){
			if(uniqueTalent.find(talent) == uniqueTalent.end() && Random.roll(2)){
				creature->getAttributes().addGeneticTalent(talent);
				uniqueTalent.insert(talent);
			}
		}
	}
  if(creature->getAttributes().getGeneticTalents().size() < 3){
		LastingEffect talent = Random.choose(
			LastingEffect::MAGIC_VULNERABILITY,
			LastingEffect::MELEE_VULNERABILITY,
			LastingEffect::BAD_BREATH,
			LastingEffect::INSANITY,
			LastingEffect::HATE_DWARVES,
			LastingEffect::HATE_ELVES,
			LastingEffect::HATE_GREENSKINS,
			LastingEffect::HATE_UNDEAD,

			LastingEffect::FAST_TRAINING,
			LastingEffect::MELEE_RESISTANCE,
			LastingEffect::MAGIC_RESISTANCE,
			LastingEffect::NIGHT_VISION,
			LastingEffect::ELF_VISION,
			LastingEffect::POISON_RESISTANT,
			LastingEffect::FIRE_RESISTANT,
			LastingEffect::COLD_RESISTANT,
			LastingEffect::PLAGUE_RESISTANT,
			LastingEffect::PLAGUE_RESISTANT
		);
		if(uniqueTalent.find(talent) == uniqueTalent.end()){
			creature->getAttributes().addGeneticTalent(talent);
			uniqueTalent.insert(talent);
		}
	}
	
	std::cout << "generateChild done" << std::endl;	
	return creature;
}