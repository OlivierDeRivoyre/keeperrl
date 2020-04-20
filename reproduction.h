#pragma once

#include "stdafx.h"
#include "util.h"
#include "entity_map.h"
#include "entity_set.h"
#include "collective_config.h"
#include "immigrant_auto_state.h"
#include "game_time.h"
#include "immigrant_info.h"

class Reproduction {
  public:
  Reproduction(Collective* collective, const vector<ImmigrantInfo>* immigrants);
  ~Reproduction();
 
  PCreature reproduce(const Creature* parent1, const Creature* parent2);
	PCreature generateFirstGeneration(CreatureId creatureId);
	PCreature generateFirstGeneration(CreatureId creatureId, Gender gender);
	const vector<CreatureId>& getInitialCreatureIds() const;

  private:	
  struct Crossing {
    CreatureId parent1;
		CreatureId parent2;		
		CreatureId child;		
  };

	optional<Crossing> GetNewCrossing(const vector<CreatureId>& definedCreatures, CreatureId child);
  optional<Crossing> tryGetCrossing(CreatureId parent1, CreatureId parent2);
	PCreature generateChild(const Creature* parent1, const Creature* parent2);

  Collective* collective;
	const vector<ImmigrantInfo>* immigrants;
	vector<Crossing> crossings;
	vector<CreatureId> initialCreatures;
};