#include "stdafx.h"
#include "collective.h"
#include "collective_control.h"
#include "creature.h"
#include "effect.h"
#include "level.h"
#include "item.h"
#include "item_factory.h"
#include "statistics.h"
#include "technology.h"
#include "monster.h"
#include "options.h"
#include "model.h"
#include "game.h"
#include "spell.h"
#include "view_id.h"
#include "equipment.h"
#include "view_index.h"
#include "minion_equipment.h"
#include "task_map.h"
#include "collective_teams.h"
#include "known_tiles.h"
#include "construction_map.h"
#include "minion_activity_map.h"
#include "tribe.h"
#include "collective_config.h"
#include "creature_name.h"
#include "cost_info.h"
#include "monster_ai.h"
#include "task.h"
#include "territory.h"
#include "collective_attack.h"
#include "gender.h"
#include "collective_name.h"
#include "creature_attributes.h"
#include "villain_type.h"
#include "workshops.h"
#include "attack_trigger.h"
#include "spell_map.h"
#include "body.h"
#include "furniture.h"
#include "furniture_factory.h"
#include "zones.h"
#include "experience_type.h"
#include "furniture_usage.h"
#include "collective_warning.h"
#include "immigration.h"
#include "creature_factory.h"
#include "resource_info.h"
#include "workshop_item.h"
#include "quarters.h"
#include "position_matching.h"
#include "storage_id.h"
#include "game_config.h"
#include "conquer_condition.h"
#include "game_event.h"
#include "view_object.h"
#include "content_factory.h"
#include "effect_type.h"
#include "immigrant_info.h"
#include "item_types.h"
#include "health_type.h"
#include "village_control.h"
#include "automaton_part.h"
#include "item_fetch_info.h"
#include "enemy_aggression_level.h"

template <class Archive>
void Collective::serialize(Archive& ar, const unsigned int version) {
  ar(SUBCLASS(TaskCallback), SUBCLASS(UniqueEntity<Collective>), SUBCLASS(EventListener));
  ar(creatures, taskMap, tribe, control, byTrait, populationGroups, hadALeader);
  ar(territory, alarmInfo, markedItems, constructions, minionEquipment, groupLockedAcitivities);
  ar(delayedPos, knownTiles, technology, kills, points, currentActivity, recordedEvents);
  ar(credit, model, immigration, teams, name, minionActivities, attackedByPlayer);
  ar(config, warnings, knownVillains, knownVillainLocations, banished, positionMatching);
  ar(villainType, enemyId, workshops, zones, discoverable, quarters, populationIncrease, dungeonLevel);
}

SERIALIZABLE(Collective)

SERIALIZATION_CONSTRUCTOR_IMPL(Collective)

Collective::Collective(Private, WModel model, TribeId t, const optional<CollectiveName>& n,
    const ContentFactory* contentFactory)
    : positionMatching(makeOwner<PositionMatching>()), tribe(t), model(NOTNULL(model)), name(n),
      villainType(VillainType::NONE), minionActivities(contentFactory) {
}

PCollective Collective::create(WModel model, TribeId tribe, const optional<CollectiveName>& name, bool discoverable,
    const ContentFactory* contentFactory) {
  auto ret = makeOwner<Collective>(Private {}, model, tribe, name, contentFactory);
  ret->subscribeTo(model);
  ret->discoverable = discoverable;
  ret->workshops = unique<Workshops>(WorkshopArray(), contentFactory);
  ret->immigration = makeOwner<Immigration>(ret.get(), vector<ImmigrantInfo>());
  return ret;
}

void Collective::init(CollectiveConfig cfg) {
  config.reset(std::move(cfg));
}

void Collective::setImmigration(PImmigration i) {
  immigration = std::move(i);
}

void Collective::setWorkshops(unique_ptr<Workshops> w) {
  workshops = std::move(w);
}

const heap_optional<CollectiveName>& Collective::getName() const {
  return name;
}

void Collective::setVillainType(VillainType t) {
  villainType = t;
}

bool Collective::isDiscoverable() const {
  return discoverable;
}

void Collective::setEnemyId(EnemyId id) {
  enemyId = id;
}

VillainType Collective::getVillainType() const {
  return villainType;
}

optional<EnemyId> Collective::getEnemyId() const {
  return enemyId;
}

Collective::~Collective() {
}

void Collective::setPopulationGroup(const vector<Creature*>& creatures) {
  for (auto c : copyOf(populationGroups))
    if (c.size() == 1 && creatures.contains(c[0]))
      populationGroups.removeElement(c);
  populationGroups.push_back(creatures);
}

Creature* Collective::addCreature(PCreature creature, Position pos, EnumSet<MinionTrait> traits) {
  Creature* c = creature.get();
  if (pos.landCreature(std::move(creature))) {
    addCreature(c, traits);
    return c;
  }
  return nullptr;
}

void Collective::updateCreatureStatus(Creature* c) {
  c->getStatus().set(CreatureStatus::CIVILIAN,
      getCreatures().contains(c) &&
      c->getBody().isHumanoid() &&
      !c->isAffected(LastingEffect::STUNNED) &&
      !hasTrait(c, MinionTrait::FIGHTER) &&
      !hasTrait(c, MinionTrait::LEADER));
  c->getStatus().set(CreatureStatus::FIGHTER, hasTrait(c, MinionTrait::FIGHTER));
  c->getStatus().set(CreatureStatus::LEADER, hasTrait(c, MinionTrait::LEADER));
  c->getStatus().set(CreatureStatus::PRISONER, hasTrait(c, MinionTrait::PRISONER));
}

void Collective::addCreature(Creature* c, EnumSet<MinionTrait> traits) {
  CHECK(!creatures.contains(c));
  if (c->getGlobalTime()) { // only do this if creature already exists on the map
    c->addEffect(LastingEffect::RESTED, 500_visible, false);
    c->addEffect(LastingEffect::SATIATED, 500_visible, false);
  }
  if (c->isAffected(LastingEffect::SUMMONED)) {
    traits.insert(MinionTrait::NO_LIMIT);
    traits.insert(MinionTrait::SUMMONED);
  }
  if (!traits.contains(MinionTrait::FARM_ANIMAL) && !c->getController()->dontReplaceInCollective())
    c->setController(makeOwner<Monster>(c, MonsterAIFactory::collective(this)));
  if (traits.contains(MinionTrait::LEADER)) {
    hadALeader = true;
    if (config->isLeaderFighter())
      traits.insert(MinionTrait::FIGHTER);
  }
  if (c->getTribeId() != *tribe)
    c->setTribe(*tribe);
  if (auto game = getGame())
    for (Collective* col : getGame()->getCollectives())
      if (col->getCreatures().contains(c))
        col->removeCreature(c);
  creatures.push_back(c);
  populationGroups.push_back({c});
  for (MinionTrait t : traits)
    byTrait[t].push_back(c);
  updateCreatureStatus(c);
  for (Item* item : c->getEquipment().getItems())
    CHECK(minionEquipment->tryToOwn(c, item));
  for (auto minion : getCreatures()) {
    c->removePrivateEnemy(minion);
    minion->removePrivateEnemy(c);
  }
  control->onMemberAdded(c);
}

void Collective::removeCreature(Creature* c) {
  creatures.removeElement(c);
  for (auto& group : populationGroups)
    group.removeElementMaybe(c);
  for (auto& group : copyOf(populationGroups))
    if (group.size() == 0)
      populationGroups.removeElement(group);
  returnResource(taskMap->freeFromTask(c));
  for (auto team : teams->getContaining(c))
    teams->remove(team, c);
  for (MinionTrait t : ENUM_ALL(MinionTrait))
    byTrait[t].removeElementMaybePreserveOrder(c);
  updateCreatureStatus(c);
}

void Collective::banishCreature(Creature* c) {
  removeCreature(c);
  if (c->getAttributes().getAutomatonSlots()) {
    taskMap->addTask(Task::disassemble(c), c->getPosition(), MinionActivity::CRAFT);
  } else {
    decreaseMoraleForBanishing(c);
    vector<Position> exitTiles = territory->getExtended(10, 20);
    vector<PTask> tasks;
    vector<Item*> items = c->getEquipment().getItems();
    if (!items.empty())
      tasks.push_back(Task::dropItemsAnywhere(items));
    if (!exitTiles.empty())
      tasks.push_back(Task::goToTryForever(Random.choose(exitTiles)));
    tasks.push_back(Task::disappear());
    c->setController(makeOwner<Monster>(c, MonsterAIFactory::singleTask(Task::chain(std::move(tasks)))));
  }
  banished.insert(c);
}

bool Collective::wasBanished(const Creature* c) const {
  return banished.contains(c);
}

/*vector<Creature*> Collective::getRecruits() const {
  vector<Creature*> ret;
  vector<Creature*> possibleRecruits = filter(getCreatures(MinionTrait::FIGHTER),
      [] (const Creature* c) { return c->getAttributes().getRecruitmentCost() > 0; });
  if (auto minPop = config->getRecruitingMinPopulation())
    for (int i = *minPop; i < possibleRecruits.size(); ++i)
      ret.push_back(possibleRecruits[i]);
  return ret;
}*/

bool Collective::hasTradeItems() const {
  for (Position pos : territory->getAll())
    if (!pos.getItems(ItemIndex::FOR_SALE).empty())
      return true;
  return false;
}

//kocham Cię

vector<Item*> Collective::getTradeItems() const {
  vector<Item*> ret;
  for (Position pos : territory->getAll())
    append(ret, pos.getItems(ItemIndex::FOR_SALE));
  return ret;
}

PItem Collective::buyItem(Item* item) {
  for (Position pos : territory->getAll())
    for (Item* it : pos.getItems(ItemIndex::FOR_SALE))
      if (it == item) {
        PItem ret = pos.removeItem(it);
        ret->setShopkeeper(nullptr);
        return ret;
      }
  FATAL << "Couldn't find item";
  return nullptr;
}

vector<TriggerInfo> Collective::getTriggers(const Collective* against) const {
  if (!isConquered())
    return control->getTriggers(against);
  else
    return {};
}

const vector<Creature*>& Collective::getLeaders() const {
  return byTrait[MinionTrait::LEADER];
}

WGame Collective::getGame() const {
  return model->getGame();
}

CollectiveControl* Collective::getControl() const {
  return control.get();
}

TribeId Collective::getTribeId() const {
  return *tribe;
}

Tribe* Collective::getTribe() const {
  return getGame()->getTribe(*tribe);
}

WModel Collective::getModel() const {
  return model;
}

const vector<Creature*>& Collective::getCreatures() const {
  return creatures;
}

void Collective::setMinionActivity(Creature* c, MinionActivity activity) {
  auto current = getCurrentActivity(c);
  if (current.activity != activity) {
    freeFromTask(c);
    c->removeEffect(LastingEffect::SLEEP);
    currentActivity.set(c, {activity, getLocalTime() +
        MinionActivities::getDuration(c, activity).value_or(-1_visible)});
  }
}

Collective::CurrentActivity Collective::getCurrentActivity(const Creature* c) const {
  return currentActivity.getMaybe(c)
      .value_or(CurrentActivity{MinionActivity::IDLE, getLocalTime() - 1_visible});
}

string Collective::getMinionGroupName(const Creature* c) const {
  if (hasTrait(c, MinionTrait::PRISONER)) {
    return "prisoner";
  } else
    return c->getName().stack();
}

bool Collective::isActivityGoodAssumingHaveTasks(Creature* c, MinionActivity activity, bool ignoreTaskLock) {
  PROFILE;
  if (!c->getAttributes().getMinionActivities().isAvailable(this, c, activity, ignoreTaskLock))
    return false;
  switch (activity) {
    case MinionActivity::BE_WHIPPED:
      return c->getMorale() < 0.95;
    case MinionActivity::CROPS:
    case MinionActivity::EXPLORE:
      return getGame()->getSunlightInfo().getState() == SunlightState::DAY;
    case MinionActivity::SLEEP:
      if (!config->hasVillainSleepingTask())
        return true;
      FALLTHROUGH;
    case MinionActivity::EXPLORE_NOCTURNAL:
      return getGame()->getSunlightInfo().getState() == SunlightState::NIGHT;
    case MinionActivity::BE_TORTURED:
      return getMaxPopulation() > getPopulationSize();
    default: return true;
  }
}

Collective::GroupLockedActivities& Collective::getGroupLockedActivities(const Creature* c) {
  return groupLockedAcitivities[getMinionGroupName(c)];
}

const Collective::GroupLockedActivities& Collective::getGroupLockedActivities(const Creature* c) const {
  static GroupLockedActivities empty;
  auto group = getMinionGroupName(c);
  if (groupLockedAcitivities.count(group))
    return groupLockedAcitivities.at(group);
  else
    return empty;
}

bool Collective::isActivityGood(Creature* c, MinionActivity activity, bool ignoreTaskLock) {
  PROFILE;
  return isActivityGoodAssumingHaveTasks(c, activity, ignoreTaskLock) &&
      (minionActivities->generate(this, c, activity) || MinionActivities::getExisting(this, c, activity));
}

bool Collective::isConquered() const {
  return config->isConquered(this);
}

void Collective::setTask(Creature* c, PTask task) {
  returnResource(taskMap->freeFromTask(c));
  taskMap->addTaskFor(std::move(task), c);
}

bool Collective::hasTask(const Creature* c) const {
  return taskMap->hasTask(c);
}

void Collective::freeFromTask(const Creature* c) {
  taskMap->freeFromTask(c);
}

void Collective::setControl(PCollectiveControl c) {
  control = std::move(c);
}

void Collective::makeConqueredRetired(Collective* conqueror) {
  auto control = VillageControl::copyOf(this, dynamic_cast<VillageControl*>(conqueror->control.get()));
  control->updateAggression(EnemyAggressionLevel::NONE);
  setControl(std::move(control));
  name = conqueror->name;
  config = conqueror->config;
  discoverable = conqueror->discoverable;
  immigration = makeOwner<Immigration>(this, conqueror->immigration->getImmigrants());
  setVillainType(conqueror->getVillainType());
  setEnemyId(*conqueror->getEnemyId());
  auto oldCreatures = getCreatures();
  for (auto c : copyOf(conqueror->getCreatures()))
    addCreature(c, conqueror->getAllTraits(c));
  for (auto c : creatures)
    getGame()->transferCreature(c, getModel(), territory->getAll());
  for (auto c : oldCreatures)
    c->dieNoReason();
}

vector<Position> Collective::getEnemyPositions() const {
  PROFILE;
  vector<Position> enemyPos;
  for (Position pos : territory->getExtended(10))
    if (const Creature* c = pos.getCreature())
      if (getTribe()->isEnemy(c) && !c->isAffected(LastingEffect::STUNNED))
        enemyPos.push_back(pos);
  return enemyPos;
}

void Collective::addNewCreatureMessage(const vector<Creature*>& immigrants) {
  if (immigrants.size() == 1)
    control->addMessage(PlayerMessage(immigrants[0]->getName().a() + " joins your forces.")
        .setCreature(immigrants[0]->getUniqueId()));
  else {
    control->addMessage(PlayerMessage("A " + immigrants[0]->getName().groupOf(immigrants.size()) +
          " joins your forces.").setCreature(immigrants[0]->getUniqueId()));
  }
}

static int getKeeperUpgradeLevel(int dungeonLevel) {
  return (dungeonLevel + 1) / 5;
}

void Collective::update(bool currentlyActive) {
  for (auto leader : getLeaders()) {
    leader->upgradeViewId(getKeeperUpgradeLevel(dungeonLevel.level));
    name->viewId = leader->getViewObject().id();
  }
  control->update(currentlyActive);
  if (config->hasImmigrantion(currentlyActive) && (!getLeaders().empty() || !hadALeader) && !isConquered())
    immigration->update();
}

double Collective::getRebellionProbability() const {
  const double allowedPrisonerRatio = 0.5;
  const double maxPrisonerRatio = 1.5;
  const int numPrisoners = getCreatures(MinionTrait::PRISONER).size();
  const int numFighters = getCreatures(MinionTrait::FIGHTER).size();
  const int numFreePrisoners = 4;
  if (numPrisoners <= numFreePrisoners)
    return 0;
  if (numFighters == 0)
    return 1;
  double ratio = double(numPrisoners - numFreePrisoners) / double(numFighters);
  return min(1.0, max(0.0, (ratio - allowedPrisonerRatio) / (maxPrisonerRatio - allowedPrisonerRatio)));
}

void Collective::considerRebellion() {
  if (Random.chance(getRebellionProbability() / 1000)) {
    Position escapeTarget = model->getTopLevel()->getLandingSquare(StairKey::transferLanding(),
        Random.choose(Vec2::directions8()));
    for (auto c : copyOf(getCreatures(MinionTrait::PRISONER))) {
      removeCreature(c);
      c->setController(makeOwner<Monster>(c, MonsterAIFactory::singleTask(
          Task::chain(Task::goToTryForever(escapeTarget), Task::disappear()))));
      c->setTribe(TribeId::getMonster());
      c->removeEffect(LastingEffect::SLEEP);
      c->removeEffect(LastingEffect::ENTANGLED);
      c->removeEffect(LastingEffect::TIED_UP);
      c->toggleCaptureOrder();
    }
    control->addMessage(PlayerMessage("Prisoners escaping!", MessagePriority::CRITICAL));
  }
}

void Collective::updateBorderTiles() {
  if (!updatedBorderTiles) {
    knownTiles->limitBorderTiles(getModel());
    updatedBorderTiles = true;
  }
}

void Collective::updateGuardTasks() {
  const auto activity = MinionActivity::GUARDING;
  for (auto& pos : zones->getPositions(ZoneId::GUARD))
    if (!taskMap->hasTask(pos, activity))
      taskMap->addTask(Task::goToAndWait(pos, 400_visible), pos, activity);
  for (auto& task : taskMap->getTasks(activity))
    if (auto pos = taskMap->getPosition(task))
      if (!zones->getPositions(ZoneId::GUARD).count(*pos))
        taskMap->removeTask(task);
}

void Collective::updateAutomatonEngines() {
  auto totalPop = getMaxPopulation();
  for (auto c : getCreatures(MinionTrait::AUTOMATON)) {
    bool off = c->getAttributes().isAffectedPermanently(LastingEffect::TURNED_OFF);
    if (totalPop > 0) {
      if (off)
        c->removePermanentEffect(LastingEffect::TURNED_OFF);
    } else
      if (!off)
        c->addPermanentEffect(LastingEffect::TURNED_OFF);
    --totalPop;
  }
}

void Collective::tick() {
  PROFILE_BLOCK("Collective::tick");
  updateBorderTiles();
  considerRebellion();
  updateGuardTasks();
  dangerLevelCache = none;
  control->tick();
  zones->tick();
  taskMap->tick();
  constructions->clearUnsupportedFurniturePlans();
  if (config->getWarnings() && Random.roll(5))
    warnings->considerWarnings(this);
  if (config->getEnemyPositions() && Random.roll(5)) {
    vector<Position> enemyPos = getEnemyPositions();
    if (!enemyPos.empty())
      delayDangerousTasks(enemyPos, getLocalTime() + 20_visible);
    else
      alarmInfo.reset();
  }
  if (config->getConstructions())
    updateConstructions();
  for (auto& workshop : workshops->types)
    workshop.second.updateState(this);
  if (Random.roll(5)) {
    auto fetchInfo = getConfig().getFetchInfo(getGame()->getContentFactory());
    if (!fetchInfo.empty()) {
      for (Position pos : territory->getAll())
        if (!isDelayed(pos) && pos.canEnterEmpty(MovementTrait::WALK) && !pos.getItems().empty())
          for (const ItemFetchInfo& elem : fetchInfo)
            fetchItems(pos, elem);
      for (Position pos : zones->getPositions(ZoneId::FETCH_ITEMS))
        if (!isDelayed(pos) && pos.canEnterEmpty(MovementTrait::WALK))
          for (const ItemFetchInfo& elem : fetchInfo)
            fetchItems(pos, elem);
      for (Position pos : zones->getPositions(ZoneId::PERMANENT_FETCH_ITEMS))
        if (!isDelayed(pos) && pos.canEnterEmpty(MovementTrait::WALK))
          for (const ItemFetchInfo& elem : fetchInfo)
            fetchItems(pos, elem);
    }
  }

  if (config->getManageEquipment() && Random.roll(40)) {
    minionEquipment->updateOwners(getCreatures());
    minionEquipment->updateItems(getAllItems(ItemIndex::MINION_EQUIPMENT, true));
  }
  for (auto c : getCreatures())
    if (!usesEquipment(c) && c->getAttributes().getAutomatonSlots() == 0)
      for (auto it : minionEquipment->getItemsOwnedBy(c))
        minionEquipment->discard(it);
  updateAutomatonPartsTasks();
  updateAutomatonEngines();
}

void Collective::updateAutomatonPartsTasks() {
  for (auto c : getCreatures())
    if (c->getAttributes().getAutomatonSlots() > 0)
      for (auto items : getStoredItems(ItemIndex::MINION_EQUIPMENT, StorageId::EQUIPMENT))
        for (auto item : items.second)
          if (item->getAutomatonPart() && minionEquipment->isOwner(item, c) && !getItemTask(item)) {
            auto task = taskMap->addTask(Task::chain(Task::pickUpItem(items.first, {item}),
                Task::installBodyPart(this, c, item)),
                items.first, MinionActivity::CRAFT);
            markItem(item, task);
          }
}

const vector<Creature*>& Collective::getCreatures(MinionTrait trait) const {
  return byTrait[trait];
}

EnumSet<MinionTrait> Collective::getAllTraits(Creature* c) const {
  CHECK(creatures.contains(c));
  EnumSet<MinionTrait> ret;
  for (auto t : ENUM_ALL(MinionTrait))
    if (hasTrait(c, t))
      ret.insert(t);
  return ret;
}

bool Collective::hasTrait(const Creature* c, MinionTrait t) const {
  return byTrait[t].contains(c);
}

void Collective::setTrait(Creature* c, MinionTrait t) {
  if (!hasTrait(c, t)) {
    byTrait[t].push_back(c);
    updateCreatureStatus(c);
  }
}

void Collective::removeTrait(Creature* c, MinionTrait t) {
  if (byTrait[t].removeElementMaybePreserveOrder(c))
    updateCreatureStatus(c);
}

void Collective::addMoraleForKill(const Creature* killer, const Creature* victim) {
  for (Creature* c : getCreatures(MinionTrait::FIGHTER))
    c->addMorale(c == killer ? 0.25 : 0.015);
}

void Collective::decreaseMoraleForKill(const Creature* killer, const Creature* victim) {
  for (Creature* c : getCreatures(MinionTrait::FIGHTER)) {
    double change = -0.015;
    if (hasTrait(victim, MinionTrait::LEADER) && getCreatures(MinionTrait::LEADER).size() == 1)
      change = -2;
    c->addMorale(change);
  }
}

void Collective::decreaseMoraleForBanishing(const Creature*) {
  for (Creature* c : getCreatures(MinionTrait::FIGHTER))
    c->addMorale(-0.05);
}

const optional<Collective::AlarmInfo>& Collective::getAlarmInfo() const {
  return alarmInfo;
}

bool Collective::needsToBeKilledToConquer(const Creature* c) const {
  return hasTrait(c, MinionTrait::FIGHTER) || hasTrait(c, MinionTrait::LEADER);
}

bool Collective::creatureConsideredPlayer(Creature* c) const {
  return c->isPlayer() ||
    (!getGame()->getPlayerCreatures().empty() && getGame()->getPlayerCreatures()[0]->getTribe() == c->getTribe()) ||
    (getGame()->getPlayerCollective() && getGame()->getPlayerCollective()->getCreatures().contains(c));
}

void Collective::onEvent(const GameEvent& event) {
  PROFILE;
  using namespace EventInfo;
  event.visit<void>(
      [&](const Alarm& info) {
        static const auto alarmTime = 100_visible;
        if (getTerritory().contains(info.pos)) {
          if (!info.silent)
            control->addMessage(PlayerMessage("An alarm goes off.", MessagePriority::HIGH).setPosition(info.pos));
          alarmInfo = AlarmInfo {getGlobalTime() + alarmTime, info.pos };
          for (Creature* c : byTrait[MinionTrait::FIGHTER])
            if (c->isAffected(LastingEffect::SLEEP))
              c->removeEffect(LastingEffect::SLEEP);
        }
      },
      [&](const CreatureKilled& info) {
        if (getCreatures().contains(info.victim))
          onMinionKilled(info.victim, info.attacker);
        if (getCreatures().contains(info.attacker))
          onKilledSomeone(info.attacker, info.victim);
      },
      [&](const CreatureTortured& info) {
        auto victim = info.victim;
        if (getCreatures().contains(victim)) {
          if (Random.roll(30)) {
            addRecordedEvent("the torturing of " + victim->getName().title());
            if (Random.roll(2)) {
              victim->dieWithReason("killed by torture");
            } else {
              control->addMessage("A prisoner is converted to your side");
              removeTrait(victim, MinionTrait::PRISONER);
              removeTrait(victim, MinionTrait::WORKER);
              removeTrait(victim, MinionTrait::NO_LIMIT);
              setTrait(victim, MinionTrait::FIGHTER);
              victim->removeEffect(LastingEffect::TIED_UP);
              setMinionActivity(victim, MinionActivity::IDLE);
            }
          }
        }
      },
      [&](const CreatureStunned& info) {
        auto victim = info.victim;
        addRecordedEvent("the capturing of " + victim->getName().title());
        if (getCreatures().contains(victim)) {
          if (info.attacker && creatureConsideredPlayer(info.attacker))
            attackedByPlayer = true;
          bool fighterStunned = needsToBeKilledToConquer(victim);
          removeTrait(victim, MinionTrait::FIGHTER);
          removeTrait(victim, MinionTrait::LEADER);
          control->addMessage(PlayerMessage(victim->getName().a() + " is unconsious.")
              .setPosition(victim->getPosition()));
          if (isConquered() && fighterStunned)
            getGame()->addEvent(EventInfo::ConqueredEnemy{this, attackedByPlayer});
          freeFromTask(victim);
        }
      },
      [&](const TrapDisarmed& info) {
        if (auto trap = constructions->getTrap(info.pos)) {
          control->addMessage(PlayerMessage(info.creature->getName().a() +
              " disarms a " + getGame()->getContentFactory()->furniture.getData(trap->getType()).getName() + " trap.",
              MessagePriority::HIGH).setPosition(info.pos));
          trap->reset();
        }
      },
      [&](const MovementChanged& info) {
        positionMatching->updateMovement(info.pos);
      },
      [&](const FurnitureDestroyed& info) {
        if (info.position.getModel() == model) {
          populationIncrease -= getGame()->getContentFactory()->furniture.getPopulationIncrease(
              info.type, constructions->getBuiltCount(info.type));
          constructions->onFurnitureDestroyed(info.position, info.layer, info.type);
          populationIncrease += getGame()->getContentFactory()->furniture.getPopulationIncrease(
              info.type, constructions->getBuiltCount(info.type));
        }
      },
      [&](const ConqueredEnemy& info) {
        auto col = info.collective;
        if (col->isDiscoverable() && info.byPlayer) {
          if (auto& name = col->getName()) {
            addRecordedEvent("the conquering of " + name->full);
            control->addMessage(PlayerMessage("The tribe of " + name->full + " is destroyed.",
                MessagePriority::CRITICAL));
          } else
            control->addMessage(PlayerMessage("An unnamed tribe is destroyed.", MessagePriority::CRITICAL));
          dungeonLevel.onKilledVillain(col->getVillainType());
        }
      },
      [&](const auto&) {}
  );
}

void Collective::onMinionKilled(Creature* victim, Creature* killer) {
  if (killer)
    addRecordedEvent("the slaying of " + victim->getName().title() + " by " + killer->getName().a());
  else
    addRecordedEvent("the death of " + victim->getName().title());
  if (killer && creatureConsideredPlayer(killer))
    attackedByPlayer = true;
  string deathDescription = victim->getAttributes().getDeathDescription();
  control->onMemberKilled(victim, killer);
  if (hasTrait(victim, MinionTrait::PRISONER) && killer && getCreatures().contains(killer))
    returnResource({ResourceId("PRISONER_HEAD"), 1});
  if (!hasTrait(victim, MinionTrait::FARM_ANIMAL) && !hasTrait(victim, MinionTrait::SUMMONED)) {
    decreaseMoraleForKill(killer, victim);
    if (killer)
      control->addMessage(PlayerMessage(victim->getName().a() + " is " + deathDescription + " by " + killer->getName().a(),
            MessagePriority::HIGH).setPosition(victim->getPosition()));
    else
      control->addMessage(PlayerMessage(victim->getName().a() + " is " + deathDescription + ".", MessagePriority::HIGH)
          .setPosition(victim->getPosition()));
  }
  if (hasTrait(victim, MinionTrait::FIGHTER) || hasTrait(victim, MinionTrait::LEADER))
    for (auto other : getCreatures(MinionTrait::FIGHTER))
      if (other != victim && other->canSee(victim))
        LastingEffects::onAllyKilled(other);
  bool fighterKilled = needsToBeKilledToConquer(victim);
  removeCreature(victim);
  if (isConquered() && fighterKilled) {
    control->onConquered(victim, killer);
    getGame()->addEvent(EventInfo::ConqueredEnemy{this, attackedByPlayer});
  }
  if (auto& guardianInfo = getConfig().getGuardianInfo())
    if (Random.chance(guardianInfo->probability)) {
      auto& extended = territory->getStandardExtended();
      if (!extended.empty())
        Random.choose(extended).landCreature(getGame()->getContentFactory()->getCreatures().fromId(
            guardianInfo->creature, getTribeId()));
    }
}

void Collective::onKilledSomeone(Creature* killer, Creature* victim) {
  string deathDescription = victim->getAttributes().getDeathDescription();
  if (victim->getTribe() != getTribe()) {
    if (victim->getStatus().contains(CreatureStatus::LEADER))
      addRecordedEvent("the slaying of " + victim->getName().title());
    addMoraleForKill(killer, victim);
    kills.insert(victim);
    int difficulty = victim->getDifficultyPoints();
    CHECK(difficulty >=0 && difficulty < 100000) << difficulty << " " << victim->getName().bare();
    points += difficulty;
    control->addMessage(PlayerMessage(victim->getName().a() + " is " + deathDescription + " by " + killer->getName().a())
        .setPosition(victim->getPosition()));
  }
}

double Collective::getEfficiency(const Creature* c) const {
  return pow(2.0, c->getMorale());
}

const Territory& Collective::getTerritory() const {
  return *territory;
}

Territory& Collective::getTerritory() {
  return *territory;
}

bool Collective::canClaimSquare(Position pos) const {
  return getKnownTiles().isKnown(pos) &&
      pos.isCovered() &&
      pos.canEnterEmpty({MovementTrait::WALK}) &&
      !pos.isWall();
}

void Collective::claimSquare(Position pos) {
  //CHECK(canClaimSquare(pos));
  territory->insert(pos);
  addKnownTile(pos);
  for (auto layer : {FurnitureLayer::FLOOR, FurnitureLayer::MIDDLE, FurnitureLayer::CEILING})
    if (auto furniture = pos.modFurniture(layer))
      if (!furniture->forgetAfterBuilding()) {
        if (!constructions->containsFurniture(pos, furniture->getLayer()))
          constructions->addFurniture(pos, ConstructionMap::FurnitureInfo::getBuilt(furniture->getType()), layer);
        furniture->setTribe(getTribeId());
      }
  control->onClaimedSquare(pos);
}

const KnownTiles& Collective::getKnownTiles() const {
  return *knownTiles;
}

LocalTime Collective::getLocalTime() const {
  return getModel()->getLocalTime();
}

GlobalTime Collective::getGlobalTime() const {
  return getGame()->getGlobalTime();
}

int Collective::numResource(ResourceId id) const {
  int ret = getValueMaybe(credit, id).value_or(0);
  if (auto storage = getResourceInfo(id).storageId)
    for (auto& pos : getStoragePositions(*storage))
      ret += pos.getItems(id).size();
  return ret;
}

int Collective::numResourcePlusDebt(ResourceId id) const {
  return numResource(id) - getDebt(id);
}

int Collective::getDebt(ResourceId id) const {
  int ret = constructions->getDebt(id);
  for (auto& elem : taskMap->getCompletionCosts())
    if (elem.second.id == id && !taskMap->getTask(elem.first)->isDone())
      ret -= elem.second.value;
  ret += workshops->getDebt(id);
  return ret;
}

bool Collective::hasResource(const CostInfo& cost) const {
  return numResource(cost.id) >= cost.value;
}

const ResourceInfo& Collective::getResourceInfo(ResourceId id) const {
  return getGame()->getContentFactory()->resourceInfo.at(id);
}

void Collective::takeResource(const CostInfo& cost) {
  int num = cost.value;
  if (num == 0)
    return;
  CHECK(num > 0);
  if (credit[cost.id]) {
    if (credit[cost.id] >= num) {
      credit[cost.id] -= num;
      return;
    } else {
      num -= credit[cost.id];
      credit[cost.id] = 0;
    }
  }
  if (auto storage = getResourceInfo(cost.id).storageId)
    for (auto& pos : getStoragePositions(*storage)) {
      vector<Item*> goldHere = pos.getItems(cost.id);
      for (Item* it : goldHere) {
        pos.removeItem(it);
        if (--num == 0)
          return;
      }
    }
  FATAL << "Not enough " << getResourceInfo(cost.id).name << " missing " << num << " of " << cost.value;
}

void Collective::returnResource(const CostInfo& amount) {
  if (amount.value == 0)
    return;
  CHECK(amount.value > 0);
  auto& info = getResourceInfo(amount.id);
  if (info.storageId && info.itemId) {
    const auto& destination = getStoragePositions(*info.storageId);
    if (!destination.empty()) {
      Random.choose(destination).dropItems(
          info.itemId->get(amount.value, getGame()->getContentFactory()));
      return;
    }
  }
  credit[amount.id] += amount.value;
}

struct Collective::TrapItemInfo {
  Item* item;
  Position pos;
  FurnitureType type;
};

vector<Collective::TrapItemInfo> Collective::getTrapItems(const vector<Position>& squares) const {
  PROFILE;
  vector<TrapItemInfo> ret;
  for (Position pos : squares)
    for (auto it : pos.getItems(ItemIndex::TRAP))
      if (!getItemTask(it))
        ret.push_back(TrapItemInfo{it, pos, it->getEffect()->effect->getValueMaybe<Effects::PlaceFurniture>()->furniture});
  return ret;
}

bool Collective::usesEquipment(const Creature* c) const {
  return config->getManageEquipment()
    && c->getBody().isHumanoid() && !hasTrait(c, MinionTrait::NO_EQUIPMENT)
    && !hasTrait(c, MinionTrait::PRISONER);
}

vector<Item*> Collective::getAllItems(bool includeMinions) const {
  return getAllItemsImpl(none, includeMinions);
}

vector<Item*> Collective::getAllItems(ItemIndex index, bool includeMinions) const {
  return getAllItemsImpl(index, includeMinions);
}

vector<pair<Position, vector<Item*>>> Collective::getStoredItems(ItemIndex index, StorageId storage) const {
  vector<pair<Position, vector<Item*>>> ret;
  for (auto& v : getStoragePositions(storage))
    ret.push_back(make_pair(v, v.getItems(index)));
  return ret;
}

vector<Item*> Collective::getAllItemsImpl(optional<ItemIndex> index, bool includeMinions) const {
  vector<Item*> allItems;
  for (auto& v : territory->getAll())
    append(allItems, index ? v.getItems(*index) : v.getItems());
  for (auto& v : zones->getPositions(ZoneId::STORAGE_EQUIPMENT))
    if (!territory->contains(v))
      append(allItems, v.getItems());
  if (includeMinions)
    for (Creature* c : getCreatures())
      append(allItems, index ? c->getEquipment().getItems(*index) : c->getEquipment().getItems());
  return allItems;
}

int Collective::getNumItems(ItemIndex index, bool includeMinions) const {
  int ret = 0;
  for (Position v : territory->getAll())
    ret += v.getItems(index).size();
  if (includeMinions)
    for (Creature* c : getCreatures())
      ret += c->getEquipment().getItems(index).size();
  return ret;
}

const PositionSet& Collective::getStorageForPillagedItem(const Item* item) const {
  for (auto& info : config->getFetchInfo(getGame()->getContentFactory()))
    if (info.applies(this, item))
      return getStoragePositions(info.storageId);
  return zones->getPositions(ZoneId::STORAGE_EQUIPMENT);
}

void Collective::addKnownVillain(const Collective* col) {
  knownVillains.insert(col);
}

bool Collective::isKnownVillain(const Collective* col) const {
  return (getModel() != col->getModel() && col->getVillainType() != VillainType::NONE) || knownVillains.contains(col);
}

void Collective::addKnownVillainLocation(const Collective* col) {
  knownVillainLocations.insert(col);
}

bool Collective::isKnownVillainLocation(const Collective* col) const {
  return knownVillainLocations.contains(col);
}

WConstTask Collective::getItemTask(const Item* it) const {
  return markedItems.getOrElse(it, nullptr).get();
}

void Collective::markItem(const Item* it, WConstTask task) {
  CHECK(!getItemTask(it));
  markedItems.set(it, task);
}

void Collective::removeTrap(Position pos) {
  constructions->removeTrap(pos);
}

bool Collective::canAddFurniture(Position position, FurnitureType type) const {
  auto layer = getGame()->getContentFactory()->furniture.getData(type).getLayer();
  return knownTiles->isKnown(position)
      && (territory->contains(position) ||
          canClaimSquare(position) ||
          getGame()->getContentFactory()->furniture.getData(type).buildOutsideOfTerritory())
      && (!getConstructions().getTrap(position) || layer != FurnitureLayer::MIDDLE)
      && !getConstructions().containsFurniture(position, layer)
      && position.canConstruct(type);
}

void Collective::removeUnbuiltFurniture(Position pos, FurnitureLayer layer) {
  if (auto f = constructions->getFurniture(pos, layer)) {
    if (f->hasTask())
      returnResource(taskMap->removeTask(f->getTask()));
    constructions->removeFurniturePlan(pos, layer);
  }
}

void Collective::destroyOrder(Position pos, FurnitureLayer layer) {
  auto furniture = pos.modFurniture(layer);
  if (!furniture || furniture->canRemoveWithCreaturePresent() || !pos.getCreature()) {
    if (furniture && !furniture->canDestroy(DestroyAction::Type::DIG) && !furniture->forgetAfterBuilding() &&
        (furniture->getTribe() == getTribeId() || furniture->canRemoveNonFriendly())) {
      furniture->destroy(pos, DestroyAction::Type::BASH);
    }
    if (!furniture || (!furniture->canDestroy(DestroyAction::Type::DIG) && !furniture->forgetAfterBuilding()))
      removeUnbuiltFurniture(pos, layer);
  }
  if (layer == FurnitureLayer::MIDDLE) {
    zones->onDestroyOrder(pos);
    if (constructions->getTrap(pos))
      removeTrap(pos);
  }
}

void Collective::addFurniture(Position pos, FurnitureType type, const CostInfo& cost, bool noCredit) {
  if (!noCredit || hasResource(cost)) {
    auto layer = getGame()->getContentFactory()->furniture.getData(type).getLayer();
    constructions->addFurniture(pos, ConstructionMap::FurnitureInfo(type, cost), layer);
    updateConstructions();
  }
}

const ConstructionMap& Collective::getConstructions() const {
  return *constructions;
}

ConstructionMap& Collective::getConstructions() {
  return *constructions;
}

void Collective::cancelMarkedTask(Position pos) {
  taskMap->removeTask(taskMap->getMarked(pos));
}

bool Collective::isMarked(Position pos) const {
  return !!taskMap->getMarked(pos);
}

HighlightType Collective::getMarkHighlight(Position pos) const {
  return taskMap->getHighlightType(pos);
}

void Collective::setPriorityTasks(Position pos) {
  taskMap->setPriorityTasks(pos);
}

bool Collective::hasPriorityTasks(Position pos) const {
  return taskMap->hasPriorityTasks(pos);
}

static HighlightType getHighlight(const DestroyAction& action) {
  switch (action.getType()) {
    case DestroyAction::Type::CUT:
      return HighlightType::CUT_TREE;
    default:
      return HighlightType::DIG;
  }
}

void Collective::orderDestruction(Position pos, const DestroyAction& action) {
  const auto layer = FurnitureLayer::MIDDLE;
  auto f = NOTNULL(pos.getFurniture(layer));
  CHECK(f->canDestroy(action));
  taskMap->markSquare(pos, getHighlight(action), Task::destruction(this, pos, f, action,
      pos.canEnterEmpty({MovementTrait::WALK}) ? nullptr : positionMatching.get()),
      action.getMinionActivity());
}

void Collective::addTrap(Position pos, FurnitureType type) {
  constructions->addTrap(pos, ConstructionMap::TrapInfo(type));
  updateConstructions();
}

void Collective::onAppliedItem(Position pos, Item* item) {
  CHECK(!!item->getEffect()->effect->getValueMaybe<Effects::PlaceFurniture>());
  if (auto trap = constructions->getTrap(pos))
    trap->setArmed();
}

bool Collective::isConstructionReachable(Position pos) {
  PROFILE;
  return knownTiles->getKnownTilesWithMargin().count(pos);
}

void Collective::onConstructed(Position pos, FurnitureType type) {
  if (pos.getFurniture(type)->forgetAfterBuilding()) {
    constructions->removeFurniturePlan(pos, getGame()->getContentFactory()->furniture.getData(type).getLayer());
    if (territory->contains(pos))
      territory->remove(pos);
    control->onConstructed(pos, type);
    return;
  }
  populationIncrease -= getGame()->getContentFactory()->furniture.getPopulationIncrease(type, constructions->getBuiltCount(type));
  constructions->onConstructed(pos, type);
  populationIncrease += getGame()->getContentFactory()->furniture.getPopulationIncrease(type, constructions->getBuiltCount(type));
  control->onConstructed(pos, type);
  if (WTask task = taskMap->getMarked(pos))
    taskMap->removeTask(task);
  //if (canClaimSquare(pos))
    claimSquare(pos);
}

void Collective::onDestructed(Position pos, FurnitureType type, const DestroyAction& action) {
  removeUnbuiltFurniture(pos, getGame()->getContentFactory()->furniture.getData(type).getLayer());
  switch (action.getType()) {
    case DestroyAction::Type::CUT:
      zones->setZone(pos, ZoneId::FETCH_ITEMS);
      break;
    case DestroyAction::Type::DIG:
      territory->insert(pos);
      break;
    default:
      break;
  }
  control->onDestructed(pos, type, action);
}

void Collective::handleTrapPlacementAndProduction() {
  PROFILE;
  unordered_map<FurnitureType, vector<TrapItemInfo>, CustomHash<FurnitureType>> trapItems;
  for (auto& elem : getTrapItems(territory->getAll()))
    trapItems[elem.type].push_back(elem);
  unordered_map<FurnitureType, int, CustomHash<FurnitureType>> missingTraps;
  for (auto trapPos : constructions->getAllTraps()) {
    auto& trap = *constructions->getTrap(trapPos);
    if (!trap.isArmed() && !trap.isMarked() && !isDelayed(trapPos)) {
      vector<TrapItemInfo>& items = trapItems[trap.getType()];
      if (!items.empty()) {
        Position pos = items.back().pos;
        auto item = items.back().item;
        auto task = taskMap->addTask(Task::chain(Task::pickUpItem(pos, {item}), Task::applyItem(this, trapPos, item)), pos,
            MinionActivity::CONSTRUCTION);
        markItem(items.back().item, task);
        items.pop_back();
        trap.setMarked();
      } else
        ++missingTraps[trap.getType()];
    }
  }
  for (auto& elem : missingTraps)
    scheduleAutoProduction([&elem](const Item* it) {
          if (auto& effect = it->getEffect())
            if (auto furnitureEffect = effect->effect->getValueMaybe<Effects::PlaceFurniture>())
              return furnitureEffect->furniture == elem.first;
          return false;
        }, elem.second);
}

void Collective::scheduleAutoProduction(function<bool(const Item*)> itemPredicate, int count) {
  if (count > 0)
    for (auto& workshop : workshops->types)
      for (auto& item : workshop.second.getQueued())
        if (itemPredicate(item.item.type.get(getGame()->getContentFactory()).get()))
          count -= item.item.batchSize;
  if (count > 0)
    for (auto& workshop : workshops->types) {
      auto& options = workshop.second.getOptions();
      for (int index : All(options))
        if (itemPredicate(options[index].type.get(getGame()->getContentFactory()).get())) {
          for (int i : Range((count + options[index].batchSize - 1) / options[index].batchSize))
            workshop.second.queue(this, index);
          return;
        }
    }
}

void Collective::updateConstructions() {
  PROFILE;
  handleTrapPlacementAndProduction();
  for (auto& pos : constructions->getAllFurniture()) {
    auto& construction = *constructions->getFurniture(pos.first, pos.second);
    if (!isDelayed(pos.first) &&
        !construction.hasTask() &&
        pos.first.canConstruct(construction.getFurnitureType()) &&
        !construction.isBuilt(pos.first) &&
        hasResource(construction.getCost())) {
      constructions->setTask(pos.first, pos.second,
          taskMap->addTaskCost(Task::construction(this, pos.first, construction.getFurnitureType()), pos.first,
              construction.getCost(), MinionActivity::CONSTRUCTION)->getUniqueId());
      takeResource(construction.getCost());
    }
  }
}

void Collective::delayDangerousTasks(const vector<Position>& enemyPos1, LocalTime delayTime) {
  PROFILE;
  unordered_set<WLevel, CustomHash<WLevel>> levels;
  for (auto& pos : enemyPos1)
    levels.insert(pos.getLevel());
  for (auto& level : levels) {
    vector<Vec2> enemyPos = enemyPos1
        .filter([=] (const Position& p) { return p.isSameLevel(level); })
        .transform([] (const Position& p) { return p.getCoord();});
    int infinity = 1000000;
    int radius = 10;
    Table<int> dist(Rectangle::boundingBox(enemyPos)
        .minusMargin(-radius)
        .intersection(level->getBounds()), infinity);
    queue<Vec2> q;
    for (Vec2 v : enemyPos) {
      dist[v] = 0;
      q.push(v);
    }
    while (!q.empty()) {
      Vec2 pos = q.front();
      q.pop();
      delayedPos[Position(pos, level)] = delayTime;
      if (dist[pos] >= radius)
        continue;
      for (Vec2 v : pos.neighbors8())
        if (v.inRectangle(dist.getBounds()) && dist[v] == infinity && territory->contains(Position(v, level))) {
          dist[v] = dist[pos] + 1;
          q.push(v);
        }
    }
  }
}

bool Collective::isDelayed(Position pos) {
  PROFILE
  return delayedPos.count(pos) && delayedPos.at(pos) > getLocalTime();
}

static Position chooseClosest(Position pos, const PositionSet& squares) {
  optional<Position> ret;
  for (auto& p : squares)
    if (!ret || pos.dist8(p).value_or(10000) < pos.dist8(*ret).value_or(10000))
      ret = p;
  return *ret;
}

const PositionSet& Collective::getStoragePositions(StorageId storage) const {
  switch (storage) {
    case StorageId::RESOURCE:
      return zones->getPositions(ZoneId::STORAGE_RESOURCES);
    case StorageId::EQUIPMENT:
      return zones->getPositions(ZoneId::STORAGE_EQUIPMENT);
    case StorageId::GOLD:
      return constructions->getBuiltPositions(FurnitureType("TREASURE_CHEST"));
    case StorageId::CORPSES:
      return constructions->getBuiltPositions(FurnitureType("GRAVE"));
  }
}

void Collective::fetchItems(Position pos, const ItemFetchInfo& elem) {
  PROFILE;
  const auto& destination = getStoragePositions(elem.storageId);
  if (destination.count(pos))
    return;
  vector<Item*> equipment = elem.getItems(this, pos);
  if (!equipment.empty()) {
    if (!destination.empty()) {
      warnings->setWarning(elem.warning, false);
      auto pickUpAndDrop = Task::pickUpAndDrop(pos, equipment, elem.storageId, this);
      auto task = taskMap->addTask(std::move(pickUpAndDrop.pickUp), pos, MinionActivity::HAULING);
      taskMap->addTask(std::move(pickUpAndDrop.drop), chooseClosest(pos, destination), MinionActivity::HAULING);
      for (Item* it : equipment)
        markItem(it, task);
    } else
      warnings->setWarning(elem.warning, true);
  }
}

void Collective::handleSurprise(Position pos) {
  Vec2 rad(8, 8);
  for (Position v : Random.permutation(pos.getRectangle(Rectangle(-rad, rad + Vec2(1, 1)))))
    if (Creature* other = v.getCreature())
      if (hasTrait(other, MinionTrait::FIGHTER) && *v.dist8(pos) > 1) {
        for (Position dest : pos.neighbors8(Random))
          if (v.canMoveCreature(dest)) {
            v.moveCreature(dest, true);
            break;
          }
      }
  pos.globalMessage("Surprise!");
}

void Collective::retire() {
  knownTiles->limitToModel(getModel());
  knownVillainLocations.clear();
  knownVillains.clear();
  discoverable = true;
  config->setConquerCondition(ConquerCondition::KILL_FIGHTERS_AND_LEADER);
}

CollectiveWarnings& Collective::getWarnings() {
  return *warnings;
}

const CollectiveConfig& Collective::getConfig() const {
  return *config;
}

bool Collective::addKnownTile(Position pos) {
  if (!knownTiles->isKnown(pos)) {
    pos.setNeedsRenderAndMemoryUpdate(true);
    knownTiles->addTile(pos, getModel());
    if (WTask task = taskMap->getMarked(pos))
      if (task->isBogus())
        taskMap->removeTask(task);
    return true;
  } else
    return false;
}

void Collective::addProducesMessage(const Creature* c, const vector<PItem>& items, const char* verb) {
  if (items.size() > 1)
    control->addMessage(c->getName().a() + " " + verb + " " + toString(items.size())
        + " " + items[0]->getName(true));
  else
    control->addMessage(c->getName().a() + " " + verb + " " + items[0]->getAName());
}

void Collective::onAppliedSquare(Creature* c, pair<Position, FurnitureLayer> pos) {
  if (auto furniture = pos.first.getFurniture(pos.second)) {
    // Furniture have variable usage time, so just multiply by it to be independent of changes.
    double efficiency = furniture->getUsageTime().getVisibleDouble() * getEfficiency(c);
    if (furniture->isRequiresLight())
      efficiency *= pos.first.getLightingEfficiency();
    if (furniture->getType() == FurnitureType("WHIPPING_POST"))
      taskMap->addTask(Task::whipping(pos.first, c), pos.first, MinionActivity::WORKING);
    if (furniture->getType() == FurnitureType("GALLOWS"))
      taskMap->addTask(Task::kill(c), pos.first, MinionActivity::WORKING);
    if (furniture->getType() == FurnitureType("TORTURE_TABLE"))
      taskMap->addTask(Task::torture(c), pos.first, MinionActivity::WORKING);
    if (furniture->getType() == FurnitureType("POETRY_TABLE") && Random.chance(0.01 * efficiency)) {
      auto poem = ItemType(ItemTypes::Poem{}).get(1, getGame()->getContentFactory());
      bool summonDemon = Random.roll(500);
      if (!recordedEvents.empty() && Random.roll(3)) {
        auto event = Random.choose(recordedEvents);
        recordedEvents.erase(event);
        poem = ItemType(ItemTypes::EventPoem{event}).get(1, getGame()->getContentFactory());
        summonDemon = false;
      }
      addProducesMessage(c, poem, "writes");
      c->getPosition().dropItems(std::move(poem));
      if (summonDemon) {
        auto id = Random.choose(CreatureId("SPECIAL_BLGN"), CreatureId("SPECIAL_BLGW"), CreatureId("SPECIAL_HLGN"), CreatureId("SPECIAL_HLGW"));
        Effect::summon(c, id, 1, 500_visible);
        auto message = PlayerMessage(c->getName().the() + " has summoned a friendly demon!", MessagePriority::CRITICAL);
        c->thirdPerson(message);
        control->addMessage(message);
      }
    }
    if (auto usage = furniture->getUsageType()) {
      auto increaseLevel = [&] (ExperienceType exp) {
        double increase = 0.007 * efficiency * LastingEffects::getTrainingSpeed(c);
        if (auto maxLevel = getGame()->getContentFactory()->furniture.getData(
            furniture->getType()).getMaxTraining(exp))
          increase = min(increase, maxLevel - c->getAttributes().getExpLevel(exp));
        if (increase > 0)
          c->increaseExpLevel(exp, increase);
      };
      if (auto id = usage->getReferenceMaybe<BuiltinUsageId>())
        switch (*id) {
          case BuiltinUsageId::DEMON_RITUAL: {
            vector<Creature*> toHeal;
            for (auto c : getCreatures())
              if (c->getBody().canHeal(HealthType::SPIRIT))
                toHeal.push_back(c);
            if (!toHeal.empty()) {
              for (auto c : toHeal)
                c->heal(double(efficiency) * 0.05 / toHeal.size());
            } else
              returnResource(CostInfo(ResourceId("DEMON_PIETY"), int(efficiency)));
            break;
          }
          case BuiltinUsageId::TRAIN:
            increaseLevel(ExperienceType::MELEE);
            break;
          case BuiltinUsageId::STUDY:
            increaseLevel(ExperienceType::SPELL);
            break;
          case BuiltinUsageId::ARCHERY_RANGE:
            increaseLevel(ExperienceType::ARCHERY);
            break;
          default:
            break;
        }
    }
    if (auto workshopType = getGame()->getContentFactory()->getWorkshopType(furniture->getType()))
      if (auto workshop = getReferenceMaybe(workshops->types, *workshopType)) {
        auto craftingSkill = c->getAttributes().getSkills().getValue(*workshopType);
        auto result = workshop->addWork(this, efficiency * craftingSkill * LastingEffects::getCraftingSpeed(c),
            craftingSkill, c->getMorale());
        if (!result.items.empty()) {
          if (result.items[0]->getClass() == ItemClass::WEAPON)
            getGame()->getStatistics().add(StatId::WEAPON_PRODUCED);
          if (result.items[0]->getClass() == ItemClass::ARMOR)
            getGame()->getStatistics().add(StatId::ARMOR_PRODUCED);
          if (result.items[0]->getClass() == ItemClass::POTION)
            getGame()->getStatistics().add(StatId::POTION_PRODUCED);
          addProducesMessage(c, result.items);
          if (result.wasUpgraded) {
            control->addMessage(PlayerMessage(c->getName().the() + " is depressed after crafting his masterpiece.", MessagePriority::HIGH));
            c->addMorale(-2);
            addRecordedEvent("the crafting of " + result.items[0]->getTheName(result.items.size() > 1));
          }
          if (result.applyImmediately)
            for (auto& item : result.items)
              item->getEffect()->apply(pos.first, c);
          else
            c->getPosition().dropItems(std::move(result.items));
        }
    }
  }
}

optional<FurnitureType> Collective::getMissingTrainingFurniture(const Creature* c, ExperienceType expType) const {
  if (c->getAttributes().isTrainingMaxedOut(expType))
    return none;
  optional<FurnitureType> requiredDummy;
  for (auto dummyType : getGame()->getContentFactory()->furniture.getTrainingFurniture(expType)) {
    bool canTrain = getGame()->getContentFactory()->furniture.getData(dummyType).getMaxTraining(expType) >
        c->getAttributes().getExpLevel(expType);
    bool hasDummy = getConstructions().getBuiltCount(dummyType) > 0;
    if (canTrain && hasDummy)
      return none;
    if (!requiredDummy && canTrain && !hasDummy)
      requiredDummy = dummyType;
  }
  return requiredDummy;
}

double Collective::getDangerLevel() const {
  if (!dangerLevelCache) {
    double ret = 0;
    for (const Creature* c : getCreatures(MinionTrait::FIGHTER))
      ret += c->getDifficultyPoints();
    ret += constructions->getBuiltCount(FurnitureType("IMPALED_HEAD")) * 150;
    dangerLevelCache = ret;
  }
  return *dangerLevelCache;
}

void Collective::acquireTech(TechId tech, bool throughLevelling) {
  technology->researched.insert(tech);
  if (throughLevelling)
    ++dungeonLevel.consumedLevels;
}

const Technology& Collective::getTechnology() const {
  return *technology;
}

void Collective::setTechnology(Technology t) {
  technology = std::move(t);
}

const EntitySet<Creature>& Collective::getKills() const {
  return kills;
}

int Collective::getPoints() const {
  return points;
}

void Collective::onRansomPaid() {
  control->onRansomPaid();
}

void Collective::onExternalEnemyKilled(const std::string& name) {
  control->addMessage(PlayerMessage("You resisted the attack of " + name + ".",
      MessagePriority::CRITICAL));
  dungeonLevel.onKilledWave();
}

void Collective::addRecordedEvent(string s) {
  recordedEvents.insert(std::move(s));
}

void Collective::onCopulated(Creature* who, Creature* with) {
  PROFILE;
  //if (with->getName().bare() == "vampire")
  //  control->addMessage(who->getName().a() + " makes love to " + with->getName().a()
  //      + " with a monkey on " + his(who->getAttributes().getGender()) + " knee");
  //else
   // control->addMessage(who->getName().a() + " makes love to " + with->getName().a());
  if (getCreatures().contains(with))
    with->addMorale(1);
	
	tryFecondate(who, with);
	tryFecondate(with, who);	
	//control->addMessage(who->getName().a() + " gender: " + string( ::getName(who->getAttributes().getGender())));
}

void Collective::tryFecondate(Creature* who, Creature* with) {
	if (who->getAttributes().getGender() == Gender::FEMALE 
			&& with->getAttributes().getGender() == Gender::MALE 
			&& !who->isAffected(LastingEffect::PREGNANT) 
			&& Random.chance(0.80)) {
				if(who->getAttributes().getCreatureId() != with->getAttributes().getCreatureId()){
					if(who->getAttributes().isAffectedPermanently(LastingEffect::FIRST_GENERATION) 
					||with->getAttributes().isAffectedPermanently(LastingEffect::FIRST_GENERATION)){
						std::cout << "FIRST_GENERATION can not crossbreed" << std::endl;
						return;
					}
				}
    who->addEffect(LastingEffect::PREGNANT, 100_visible);
		who->setMateCreature(with);
    control->addMessage(who->getName().a() + " becomes pregnant from " + with->getName().a());
  }
}

MinionEquipment& Collective::getMinionEquipment() {
  return *minionEquipment;
}

const MinionEquipment& Collective::getMinionEquipment() const {
  return *minionEquipment;
}

Workshops& Collective::getWorkshops() {
  return *workshops;
}

const Workshops& Collective::getWorkshops() const {
  return *workshops;
}

Immigration& Collective::getImmigration() {
  return *immigration;
}

const Immigration& Collective::getImmigration() const {
  return *immigration;
}

const MinionActivities& Collective::getMinionActivities() const {
  return *minionActivities;
}

CollectiveTeams& Collective::getTeams() {
  return *teams;
}

const CollectiveTeams& Collective::getTeams() const {
  return *teams;
}

void Collective::freeTeamMembers(const vector<Creature*>& members) {
  PROFILE;
  for (Creature* c : members) {
    if (c->isAffected(LastingEffect::SLEEP))
      c->removeEffect(LastingEffect::SLEEP);
  }
}

Zones& Collective::getZones() {
  return *zones;
}

const Zones& Collective::getZones() const {
  return *zones;
}

Quarters& Collective::getQuarters() {
  return *quarters;
}

const Quarters& Collective::getQuarters() const {
  return *quarters;
}

const TaskMap& Collective::getTaskMap() const {
  return *taskMap;
}

TaskMap& Collective::getTaskMap() {
  return *taskMap;
}

int Collective::getPopulationSize() const {
  return populationGroups.size() - getCreatures(MinionTrait::NO_LIMIT).size();
}

int Collective::getMaxPopulation() const {
  return populationIncrease + config->getMaxPopulation() + getCreatures(MinionTrait::INCREASE_POPULATION).size();
}

const DungeonLevel& Collective::getDungeonLevel() const {
  return dungeonLevel;
}

DungeonLevel& Collective::getDungeonLevel() {
  return dungeonLevel;
}

REGISTER_TYPE(Collective)
REGISTER_TYPE(ListenerTemplate<Collective>)
