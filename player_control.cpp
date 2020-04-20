/* Copyright (C) 2013-2014 Michal Brzozowski (rusolis@poczta.fm)

   This file is part of KeeperRL.

   KeeperRL is free software; you can redistribute it and/or modify it under the terms of the
   GNU General Public License as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   KeeperRL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along with this program.
   If not, see http://www.gnu.org/licenses/ . */

#include "stdafx.h"

#include "player_control.h"
#include "level.h"
#include "task.h"
#include "model.h"
#include "statistics.h"
#include "options.h"
#include "technology.h"
#include "village_control.h"
#include "item.h"
#include "item_factory.h"
#include "creature.h"
#include "square.h"
#include "view_id.h"
#include "collective.h"
#include "effect.h"
#include "music.h"
#include "encyclopedia.h"
#include "map_memory.h"
#include "item_action.h"
#include "equipment.h"
#include "collective_teams.h"
#include "minion_equipment.h"
#include "task_map.h"
#include "construction_map.h"
#include "minion_activity_map.h"
#include "spell.h"
#include "tribe.h"
#include "visibility_map.h"
#include "creature_name.h"
#include "view.h"
#include "view_index.h"
#include "collective_attack.h"
#include "territory.h"
#include "sound.h"
#include "game.h"
#include "collective_name.h"
#include "creature_attributes.h"
#include "collective_config.h"
#include "villain_type.h"
#include "workshops.h"
#include "attack_trigger.h"
#include "view_object.h"
#include "body.h"
#include "furniture.h"
#include "furniture_type.h"
#include "furniture_factory.h"
#include "known_tiles.h"
#include "zones.h"
#include "inventory.h"
#include "immigration.h"
#include "scroll_position.h"
#include "tutorial.h"
#include "tutorial_highlight.h"
#include "container_range.h"
#include "collective_warning.h"
#include "furniture_usage.h"
#include "message_generator.h"
#include "message_buffer.h"
#include "minion_controller.h"
#include "build_info.h"
#include "vision.h"
#include "external_enemies.h"
#include "resource_info.h"
#include "workshop_item.h"
#include "time_queue.h"
#include "quarters.h"
#include "unknown_locations.h"
#include "furniture_click.h"
#include "campaign.h"
#include "game_event.h"
#include "view_object_action.h"
#include "storage_id.h"
#include "fx_name.h"
#include "content_factory.h"
#include "tech_id.h"
#include "pretty_printing.h"
#include "game_save_type.h"
#include "immigrant_info.h"
#include "special_trait.h"
#include "user_input.h"
#include "automaton_part.h"
#include "shortest_path.h"

template <class Archive>
void PlayerControl::serialize(Archive& ar, const unsigned int version) {
  ar& SUBCLASS(CollectiveControl) & SUBCLASS(EventListener);
  ar(memory, introText, nextKeeperWarning, tribeAlignment);
  ar(newAttacks, ransomAttacks, notifiedAttacks, messages, hints);
  ar(visibilityMap, unknownLocations, dismissedVillageInfos, buildInfo);
  ar(messageHistory, tutorial, controlModeMessages, stunnedCreatures);
}

SERIALIZABLE(PlayerControl)

SERIALIZATION_CONSTRUCTOR_IMPL(PlayerControl)

using ResourceId = Collective::ResourceId;

const LocalTime hintFrequency = LocalTime(700);
static vector<string> getHints() {
  return {
    "Morale affects minion productivity and chances of fleeing from battle.",
 //   "You can turn these hints off in the settings (F2).",
    "Killing a leader greatly lowers the morale of his tribe and stops immigration.",
//    "Your minions' morale is boosted when they are commanded by the Keeper.",
  };
}

PlayerControl::PlayerControl(Private, Collective* col, TribeAlignment alignment)
    : CollectiveControl(col), hints(getHints()), tribeAlignment(alignment) {
  controlModeMessages = make_shared<MessageBuffer>();
  visibilityMap = make_shared<VisibilityMap>();
  unknownLocations = make_shared<UnknownLocations>();
  memory.reset(new MapMemory());
  for (auto pos : col->getModel()->getTopLevel()->getAllPositions())
    if (auto f = pos.getFurniture(FurnitureLayer::MIDDLE))
      if (f->isClearFogOfWar())
        addToMemory(pos);
}

PPlayerControl PlayerControl::create(Collective* col, vector<string> introText, TribeAlignment alignment) {
  auto ret = makeOwner<PlayerControl>(Private{}, col, alignment);
  ret->subscribeTo(col->getModel());
  ret->introText = introText;
  return ret;
}

PlayerControl::~PlayerControl() {
}

void PlayerControl::loadBuildingMenu(const ContentFactory* contentFactory, const KeeperCreatureInfo& keeperCreatureInfo) {
  for (auto& group : keeperCreatureInfo.buildingGroups)
    buildInfo.append(contentFactory->buildInfo.at(group));
  for (auto& info : buildInfo)
    if (auto furniture = info.type.getReferenceMaybe<BuildInfoTypes::Furniture>()) {
      for (auto type : furniture->types) {
        double luxury = getGame()->getContentFactory()->furniture.getData(type).getLuxuryInfo().luxury;
        if (luxury > 0) {
          info.help += " Increases luxury by " + toString(luxury) + ".";
          break;
        }
      }
      if (auto increase = getGame()->getContentFactory()->furniture
          .getPopulationIncreaseDescription(furniture->types[0], keeperCreatureInfo.populationString))
        info.help += " " + *increase;
      for (auto expType : ENUM_ALL(ExperienceType))
        if (auto increase = getGame()->getContentFactory()->furniture.getData(furniture->types[0]).getMaxTraining(expType))
          info.help += " Adds up to " + toString(increase) + " " + toLower(getName(expType)) + " levels.";
    }
}

void PlayerControl::loadImmigrationAndWorkshops(ContentFactory* contentFactory,
    const KeeperCreatureInfo& keeperCreatureInfo) {
  Technology technology = contentFactory->technology;
  for (auto& tech : copyOf(technology.techs))
    if (!keeperCreatureInfo.technology.contains(tech.first))
      technology.techs.erase(tech.first);
  WorkshopArray merged;
  for (auto& group : keeperCreatureInfo.workshopGroups)
    for (auto& elem : contentFactory->workshopGroups.at(group))
      merged[elem.first].append(elem.second);
  collective->setWorkshops(unique<Workshops>(std::move(merged), contentFactory));
  vector<ImmigrantInfo> immigrants;
  for (auto elem : keeperCreatureInfo.immigrantGroups)
    append(immigrants, contentFactory->immigrantsData.at(elem));
  CollectiveConfig::addBedRequirementToImmigrants(immigrants, contentFactory);
  collective->setImmigration(makeOwner<Immigration>(collective, std::move(immigrants)));
  collective->setTechnology(std::move(technology));
  loadBuildingMenu(contentFactory, keeperCreatureInfo);
}

const vector<Creature*>& PlayerControl::getControlled() const {
  return getGame()->getPlayerCreatures();
}

optional<TeamId> PlayerControl::getCurrentTeam() const {
  // try returning a non-persistent team first
  for (TeamId team : getTeams().getAllActive())
    if (!getTeams().isPersistent(team) && getTeams().getLeader(team)->isPlayer())
      return team;
  for (TeamId team : getTeams().getAllActive())
    if (getTeams().getLeader(team)->isPlayer())
      return team;
  return none;
}

void PlayerControl::onControlledKilled(const Creature* victim) {
  TeamId currentTeam = *getCurrentTeam();
  if (getTeams().getLeader(currentTeam) == victim && getGame()->getPlayerCreatures().size() == 1) {
    vector<CreatureInfo> team;
    for (auto c : getTeams().getMembers(currentTeam))
      if (c != victim)
        team.push_back(CreatureInfo(c));
    if (team.empty())
      return;
    optional<Creature::Id> newLeader;
    if (team.size() == 1)
      newLeader = team[0].uniqueId;
    else
      newLeader = getView()->chooseCreature("Choose new team leader:", team, "Order team back to base");
    if (newLeader) {
      if (Creature* c = getCreature(*newLeader)) {
        getTeams().setLeader(currentTeam, c);
        if (!c->isPlayer())
          c->pushController(createMinionController(c));
        return;
      }
    }
    leaveControl();
  }
}

void PlayerControl::onSunlightVisibilityChanged() {
  for (auto pos : collective->getConstructions().getBuiltPositions(FurnitureType("EYEBALL")))
    visibilityMap->updateEyeball(pos);
}

void PlayerControl::setTutorial(STutorial t) {
  tutorial = t;
}

STutorial PlayerControl::getTutorial() const {
  return tutorial;
}

bool PlayerControl::canControlSingle(const Creature* c) const {
  return !collective->hasTrait(c, MinionTrait::PRISONER);
}

bool PlayerControl::canControlInTeam(const Creature* c) const {
  return collective->hasTrait(c, MinionTrait::FIGHTER) || collective->hasTrait(c, MinionTrait::LEADER);
}

void PlayerControl::addToCurrentTeam(Creature* c) {
  collective->freeTeamMembers({c});
  if (auto teamId = getCurrentTeam()) {
    getTeams().add(*teamId, c);
    if (getGame()->getPlayerCreatures().size() > 1)
      c->pushController(createMinionController(c));
  }
}

void PlayerControl::teamMemberAction(TeamMemberAction action, Creature::Id id) {
  if (Creature* c = getCreature(id))
    switch (action) {
      case TeamMemberAction::MOVE_NOW:
        c->getPosition().getModel()->getTimeQueue().moveNow(c);
        break;
      case TeamMemberAction::CHANGE_LEADER:
        if (auto teamId = getCurrentTeam())
          if (getTeams().getMembers(*teamId).size() > 1 && canControlInTeam(c)) {
            auto controlled = getControlled();
            if (controlled.size() == 1) {
              getTeams().getLeader(*teamId)->popController();
              getTeams().setLeader(*teamId, c);
              c->pushController(createMinionController(c));
            }
          }
        break;
      case TeamMemberAction::REMOVE_MEMBER: {
        auto& teams = getTeams();
        if (auto teamId = getCurrentTeam())
          if (teams.getMembers(*teamId).size() > 1 && teams.contains(*teamId, c)) {
            teams.remove(*teamId, c);
            if (c->isPlayer()) {
              c->popController();
              auto newLeader = teams.getLeader(*teamId);
              if (!newLeader->isPlayer())
                newLeader->pushController(createMinionController(newLeader));
            }
          }
        break;
      }
    }
}

void PlayerControl::leaveControl() {
  set<TeamId> allTeams;
  for (auto controlled : copyOf(getControlled())) {
    if (collective->hasTrait(controlled, MinionTrait::LEADER))
      nextKeeperWarning = collective->getGlobalTime();
    auto controlledLevel = controlled->getPosition().getLevel();
    if (getModel()->getMainLevels().contains(controlledLevel))
      setScrollPos(controlled->getPosition());
    controlled->popController();
    for (TeamId team : getTeams().getActive(controlled))
      allTeams.insert(team);
  }
  for (auto team : allTeams) {
    // a creature may die when landing and be removed from the team so copy the members vector
    for (Creature* c : copyOf(getTeams().getMembers(team)))
//      if (getGame()->canTransferCreature(c, collective->getLevel()->getModel()))
        getGame()->transferCreature(c, getModel());
    if (!getTeams().isPersistent(team)) {
      if (getTeams().getMembers(team).size() == 1)
        getTeams().cancel(team);
      else
        getTeams().deactivate(team);
      break;
    }
  }
  getView()->stopClock();
}

void PlayerControl::render(View* view) {
  //collective->getConstructions().checkDebtConsistency();
  if (firstRender) {
    firstRender = false;
    for (Creature* c : getCreatures())
      updateMinionVisibility(c);
  }
  if (getControlled().empty()) {
    view->updateView(this, false);
  }
  if (!introText.empty() && getGame()->getOptions()->getBoolValue(OptionId::HINTS)) {
    view->updateView(this, false);
    for (auto& msg : introText)
      view->presentText("", msg);
    introText.clear();
  }
}

void PlayerControl::addConsumableItem(Creature* creature) {
  ScrollPosition scrollPos;
  while (1) {
    Item* chosenItem = chooseEquipmentItem(creature, {}, [&](const Item* it) {
        return !collective->getMinionEquipment().isOwner(it, creature)
            && !it->canEquip()
            && collective->getMinionEquipment().needsItem(creature, it, true); }, &scrollPos);
    if (chosenItem) {
      creature->removeEffect(LastingEffect::SLEEP);
      CHECK(collective->getMinionEquipment().tryToOwn(creature, chosenItem));
    } else
      break;
  }
}

void PlayerControl::addEquipment(Creature* creature, EquipmentSlot slot) {
  vector<Item*> currentItems = creature->getEquipment().getSlotItems(slot);
  Item* chosenItem = chooseEquipmentItem(creature, currentItems, [&](const Item* it) {
      return !collective->getMinionEquipment().isOwner(it, creature)
      && creature->canEquipIfEmptySlot(it, nullptr) && it->getEquipmentSlot() == slot; });
  if (chosenItem) {
    if (auto creatureId = collective->getMinionEquipment().getOwner(chosenItem))
      if (Creature* c = getCreature(*creatureId))
        c->removeEffect(LastingEffect::SLEEP);
    creature->removeEffect(LastingEffect::SLEEP);
    CHECK(collective->getMinionEquipment().tryToOwn(creature, chosenItem));
  }
}

void PlayerControl::minionEquipmentAction(const EquipmentActionInfo& action) {
  if (auto creature = getCreature(action.creature))
    switch (action.action) {
      case ItemAction::DROP:
        for (auto id : action.ids)
          collective->getMinionEquipment().discard(id);
        break;
      case ItemAction::REPLACE:
        if (action.slot)
          addEquipment(creature, *action.slot);
        else
          addConsumableItem(creature);
        break;
      case ItemAction::LOCK:
        /*if (action.ids.empty() && action.slot)
          collective->getMinionEquipment().toggleLocked(creature, *action.slot);
        else*/
          for (auto id : action.ids)
            collective->getMinionEquipment().toggleLocked(creature, id);
        break;
      default:
        break;
    }
}

void PlayerControl::minionTaskAction(const TaskActionInfo& action) {
  if (auto c = getCreature(action.creature)) {
    if (action.switchTo)
      collective->setMinionActivity(c, *action.switchTo);
    for (MinionActivity task : action.lock)
      c->getAttributes().getMinionActivities().toggleLock(task);
    auto& groupLocked = collective->getGroupLockedActivities(c);
    groupLocked = groupLocked.ex_or(action.lockGroup);
  }
}

static ItemInfo getItemInfo(const ContentFactory* factory, const vector<Item*>& stack, bool equiped, bool pending, bool locked,
    optional<ItemInfo::Type> type = none) {
  return CONSTRUCT(ItemInfo,
    c.name = stack[0]->getShortName(nullptr, stack.size() > 1);
    c.fullName = stack[0]->getNameAndModifiers(false);
    c.description = stack[0]->getDescription(factory);
    c.number = stack.size();
    if (stack[0]->canEquip())
      c.slot = stack[0]->getEquipmentSlot();
    c.viewId = stack[0]->getViewObject().id();
    for (auto it : stack)
      c.ids.insert(it->getUniqueId());
    c.actions = {ItemAction::DROP};
    c.equiped = equiped;
    c.locked = locked;
    if (type)
      c.type = *type;
    c.pending = pending;
    if (auto& part = stack[0]->getAutomatonPart())
      fillInstalledPartDescription(factory, c, *part);
  );
}

static ViewId getSlotViewId(EquipmentSlot slot) {
  switch (slot) {
    case EquipmentSlot::BOOTS: return ViewId("leather_boots");
    case EquipmentSlot::WEAPON: return ViewId("sword");
    case EquipmentSlot::RINGS: return ViewId("ring_red");
    case EquipmentSlot::HELMET: return ViewId("leather_helm");
    case EquipmentSlot::RANGED_WEAPON: return ViewId("bow");
    case EquipmentSlot::GLOVES: return ViewId("leather_gloves");
    case EquipmentSlot::BODY_ARMOR: return ViewId("leather_armor");
    case EquipmentSlot::AMULET: return ViewId("amulet1");
    case EquipmentSlot::SHIELD: return ViewId("wooden_shield");
  }
}

static ItemInfo getEmptySlotItem(EquipmentSlot slot, bool locked) {
  return CONSTRUCT(ItemInfo,
    c.name = "";
    c.fullName = "";
    c.slot = slot;
    c.number = 1;
    c.viewId = getSlotViewId(slot);
    c.actions = {ItemAction::REPLACE};
    c.locked = locked;
    c.equiped = false;
    c.pending = false;);
}

static ItemInfo getTradeItemInfo(const ContentFactory* factory, const vector<Item*>& stack, int budget) {
  return CONSTRUCT(ItemInfo,
    c.name = stack[0]->getShortName(nullptr, stack.size() > 1);
    c.price = make_pair(ViewId("gold"), stack[0]->getPrice());
    c.fullName = stack[0]->getNameAndModifiers(false);
    c.description = stack[0]->getDescription(factory);
    c.number = stack.size();
    c.viewId = stack[0]->getViewObject().id();
    for (auto it : stack)
      c.ids.insert(it->getUniqueId());
    c.unavailable = c.price->second > budget;);
}

void PlayerControl::fillAutomatonParts(Creature* creature, PlayerInfo& info) const {
  int index = 0;
  for (auto item : collective->getMinionEquipment().getItemsOwnedBy(creature))
    if (auto& part = item->getAutomatonPart()) {
      if ([&] {
        for (auto& d : creature->drops)
          if (d.get() == item)
            return true;
        return false; }())
        continue;
      info.bodyParts.push_back(getInstalledPartInfo(getGame()->getContentFactory(), *part, ++index));
      info.bodyParts.back().pending = true;
    }
  info.canAddBodyPart = creature->getAttributes().getAutomatonSlots() > info.bodyParts.size();
}

void PlayerControl::fillEquipment(Creature* creature, PlayerInfo& info) const {
  info.inventory.clear();
  if (!creature->getBody().isHumanoid())
    return;
  vector<EquipmentSlot> slots;
  for (auto slot : Equipment::slotTitles)
    slots.push_back(slot.first);
  vector<Item*> ownedItems = collective->getMinionEquipment().getItemsOwnedBy(creature);
  for (auto slot : slots) {
    vector<Item*> items;
    for (Item* it : ownedItems)
      if (it->canEquip() && it->getEquipmentSlot() == slot)
        items.push_back(it);
    for (int i = creature->getEquipment().getMaxItems(slot, creature); i < items.size(); ++i)
      // a rare occurence that minion owns too many items of the same slot,
      //should happen only when an item leaves the fortress and then is braught back
      if (!collective->getMinionEquipment().isLocked(creature, items[i]->getUniqueId()))
        collective->getMinionEquipment().discard(items[i]);
    for (Item* item : items) {
      ownedItems.removeElement(item);
      bool equiped = creature->getEquipment().isEquipped(item);
      bool locked = collective->getMinionEquipment().isLocked(creature, item->getUniqueId());
      info.inventory.push_back(getItemInfo(getGame()->getContentFactory(), {item}, equiped, !equiped, locked, ItemInfo::EQUIPMENT));
    }
    for (int i : Range(creature->getEquipment().getMaxItems(slot, creature) - items.size()))
      info.inventory.push_back(getEmptySlotItem(slot, collective->getMinionEquipment().isLocked(creature, slot)));
    if (slot == EquipmentSlot::WEAPON && tutorial &&
        tutorial->getHighlights(getGame()).contains(TutorialHighlight::EQUIPMENT_SLOT_WEAPON))
      info.inventory.back().tutorialHighlight = true;
  }
  vector<vector<Item*>> consumables = Item::stackItems(ownedItems,
      [&](const Item* it) { if (!creature->getEquipment().hasItem(it)) return " (pending)"; else return ""; } );
  for (auto& stack : consumables)
    info.inventory.push_back(getItemInfo(getGame()->getContentFactory(), stack, false,
          !creature->getEquipment().hasItem(stack[0]), false, ItemInfo::CONSUMABLE));
  vector<Item*> otherItems;
  for (auto item : creature->getEquipment().getItems())
    if (!collective->getMinionEquipment().isItemUseful(item))
      otherItems.push_back(item);
  for (auto item : Item::stackItems(otherItems))
    info.inventory.push_back(getItemInfo(getGame()->getContentFactory(), {item}, false, false, false, ItemInfo::OTHER));
}

Item* PlayerControl::chooseEquipmentItem(Creature* creature, vector<Item*> currentItems, ItemPredicate predicate,
    ScrollPosition* scrollPos) {
  return chooseEquipmentItem(creature, std::move(currentItems), collective->getAllItems().filter(predicate), scrollPos);
}

Item* PlayerControl::chooseEquipmentItem(Creature* creature, vector<Item*> currentItems, vector<Item*> allItems,
    ScrollPosition* scrollPos) {
  vector<Item*> availableItems;
  vector<Item*> usedItems;
  collective->getMinionEquipment().sortByEquipmentValue(creature, allItems);
  for (Item* item : allItems)
    if (!currentItems.contains(item)) {
      auto owner = collective->getMinionEquipment().getOwner(item);
      if (owner && getCreature(*owner))
        usedItems.push_back(item);
      else
        availableItems.push_back(item);
    }
  if (currentItems.empty() && availableItems.empty() && usedItems.empty())
    return nullptr;
  vector<vector<Item*>> usedStacks = Item::stackItems(usedItems,
      [&](const Item* it) {
        const Creature* c = getCreature(*collective->getMinionEquipment().getOwner(it));
        return c->getName().bare() + toString<int>(c->getBestAttack().value);});
  vector<Item*> allStacked;
  vector<ItemInfo> options;
  for (Item* it : currentItems)
    options.push_back(getItemInfo(getGame()->getContentFactory(), {it}, true, false, false));
  for (auto& stack : concat(Item::stackItems(availableItems), usedStacks)) {
    options.emplace_back(getItemInfo(getGame()->getContentFactory(), stack, false, false, false));
    if (auto creatureId = collective->getMinionEquipment().getOwner(stack[0]))
      if (const Creature* c = getCreature(*creatureId))
        options.back().owner = CreatureInfo(c);
    allStacked.push_back(stack.front());
  }
  auto index = getView()->chooseItem(options, scrollPos);
  if (!index)
    return nullptr;
  return concat(currentItems, allStacked)[*index];
}

int PlayerControl::getNumMinions() const {
  return (int) collective->getCreatures(MinionTrait::FIGHTER).size();
}

typedef CollectiveInfo::Button Button;

optional<pair<ViewId, int>> PlayerControl::getCostObj(CostInfo cost) const {
  auto& resourceInfo = collective->getResourceInfo(cost.id);
  if (cost.value > 0 && resourceInfo.viewId)
    return make_pair(*resourceInfo.viewId, (int) cost.value);
  else
    return none;
}

optional<pair<ViewId, int>> PlayerControl::getCostObj(const optional<CostInfo>& cost) const {
  if (cost)
    return getCostObj(*cost);
  else
    return none;
}

string PlayerControl::getMinionName(CreatureId id) const {
  static map<CreatureId, string> names;
  if (!names.count(id))
    names[id] = getGame()->getContentFactory()->getCreatures().fromId(id, TribeId::getMonster())->getName().bare();
  return names.at(id);
}

ViewId PlayerControl::getViewId(const BuildInfo& info) const {
  return info.type.visit<ViewId>(
      [&](const BuildInfoTypes::Furniture& elem) {
        return getGame()->getContentFactory()->furniture.getData(elem.types[0]).getViewObject()->id();
      },
      [&](const BuildInfoTypes::Dig&) {
        return ViewId("dig_icon");
      },
      [&](const BuildInfoTypes::ImmediateDig&) {
        return ViewId("dig_icon");
      },
      [&](ZoneId zone) {
        return ::getViewId(zone);
      },
      [&](BuildInfoTypes::ClaimTile) {
        return ViewId("keeper_floor");
      },
      [&](BuildInfoTypes::Dispatch) {
        return ViewId("imp");
      },
      [&](const BuildInfoTypes::Trap& elem) {
        return elem.viewId;
      },
      [&](const BuildInfoTypes::DestroyLayers&) {
         return ViewId("destroy_button");
      },
      [&](BuildInfoTypes::ForbidZone) {
        return ViewId("forbid_zone");
      },
      [&](BuildInfoTypes::PlaceMinion) {
        return ViewId("special_blbn");
      },
      [&](BuildInfoTypes::PlaceItem) {
        return ViewId("potion1");
      }
  );
}

vector<Button> PlayerControl::fillButtons() const {
  vector<Button> buttons;
  for (auto& button : buildInfo) {
    button.type.visit<void>(
        [&](const BuildInfoTypes::Furniture& elem) {
          string description;
          if (elem.cost.value > 0) {
            int num = 0;
            for (auto type : elem.types)
              num += collective->getConstructions().getBuiltCount(type);
            if (num > 0)
              description = "[" + toString(num) + "]";
          }
          int availableNow = !elem.cost.value ? 1 : collective->numResource(elem.cost.id) / elem.cost.value;
          if (!collective->getResourceInfo(elem.cost.id).viewId && availableNow)
            description += " (" + toString(availableNow) + " available)";
          buttons.push_back({getViewId(button), button.name,
              getCostObj(elem.cost),
              description,
              (elem.noCredit && !availableNow) ?
                 CollectiveInfo::Button::GRAY_CLICKABLE : CollectiveInfo::Button::ACTIVE });
          },
        [&](const BuildInfoTypes::Dig&) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](const BuildInfoTypes::ImmediateDig&) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](ZoneId) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](BuildInfoTypes::ClaimTile) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](BuildInfoTypes::Dispatch) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](const BuildInfoTypes::Trap& elem) {
          buttons.push_back({elem.viewId, button.name, none});
        },
        [&](const BuildInfoTypes::DestroyLayers&) {
           buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](BuildInfoTypes::ForbidZone) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](BuildInfoTypes::PlaceMinion) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        },
        [&](BuildInfoTypes::PlaceItem) {
          buttons.push_back({getViewId(button), button.name, none, "", CollectiveInfo::Button::ACTIVE});
        }
    );
    vector<string> unmetReqText;
    for (auto& req : button.requirements)
      if (!BuildInfo::meetsRequirement(collective, req)) {
        unmetReqText.push_back("Requires " + BuildInfo::getRequirementText(req) + ".");
        buttons.back().state = CollectiveInfo::Button::INACTIVE;
      }
    if (unmetReqText.empty())
      buttons.back().help = button.help;
    else
      buttons.back().help = combineSentences(concat({button.help}, unmetReqText));
    buttons.back().hotkey = button.hotkey;
    buttons.back().groupName = button.groupName;
    buttons.back().hotkeyOpensGroup = button.hotkeyOpensGroup;
    buttons.back().tutorialHighlight = button.tutorialHighlight;
  }
  return buttons;
}

string PlayerControl::getTriggerLabel(const AttackTrigger& trigger) const {
  return trigger.visit<string>(
      [&](const Timer&) {
        return "evil";
      },
      [&](const RoomTrigger& t) {
        auto myCount = collective->getConstructions().getBuiltCount(t.type);
        return getGame()->getContentFactory()->furniture.getData(t.type).getName(myCount);
      },
      [&](const Power&) {
        return "your power";
      },
      [&](const FinishOff&) {
        return "finishing you off";
      },
      [&](const SelfVictims&) {
        return "killed tribe members";
      },
      [&](const EnemyPopulation&) {
        return "population";
      },
      [&](const Gold&) {
        return "gold";
      },
      [&](const StolenItems&) {
        return "item theft";
      },
      [&](const MiningInProximity&) {
        return "breach of territory";
      },
      [&](const Proximity&) {
        return "proximity";
      },
      [&](const NumConquered&) {
        return "aggression";
      },
      [&](Immediate) {
        return "just doesn't like you";
      }
  );
}

VillageInfo::Village PlayerControl::getVillageInfo(const Collective* col) const {
  VillageInfo::Village info;
  info.name = col->getName()->shortened;
  info.id = col->getUniqueId();
  info.tribeName = col->getName()->race;
  info.viewId = col->getName()->viewId;
  info.triggers.clear();
  info.type = col->getVillainType();
  info.attacking = false;
  for (auto& attack : notifiedAttacks)
    if (attack.getAttacker() == col && attack.isOngoing())
        info.attacking = true;
  auto addTriggers = [&] {
    for (auto& trigger : col->getTriggers(collective))
//#ifdef RELEASE
      if (trigger.value > 0)
//#endif
        info.triggers.push_back({getTriggerLabel(trigger.trigger), trigger.value});
  };
  if (col->getModel() == getModel()) {
    if (!collective->isKnownVillainLocation(col) && !getGame()->getOptions()->getBoolValue(OptionId::SHOW_MAP))
      info.access = VillageInfo::Village::NO_LOCATION;
    else
      info.access = VillageInfo::Village::LOCATION;
    addTriggers();
  } else if (!getGame()->isVillainActive(col))
    info.access = VillageInfo::Village::INACTIVE;
  else {
    info.access = VillageInfo::Village::ACTIVE;
    addTriggers();
  }
  if ((info.isConquered = col->isConquered())) {
    info.triggers.clear();
    if (canPillage(col))
      info.actions.push_back({VillageAction::PILLAGE, none});
  } else if (!col->getTribe()->isEnemy(collective->getTribe())) {
    if (collective->isKnownVillainLocation(col)) {
      if (col->hasTradeItems())
        info.actions.push_back({VillageAction::TRADE, none});
    } else if (getGame()->isVillainActive(col)) {
      if (col->hasTradeItems())
        info.actions.push_back({VillageAction::TRADE, string("You must discover the location of the ally in order to trade.")});
    }
  }
  return info;
}

void PlayerControl::handleTrading(Collective* ally) {
  ScrollPosition scrollPos;
  auto& storage = collective->getZones().getPositions(ZoneId::STORAGE_EQUIPMENT);
  if (storage.empty()) {
    getView()->presentText("Information", "You need a storage room for equipment in order to trade.");
    return;
  }
  while (1) {
    vector<Item*> available = ally->getTradeItems();
    vector<vector<Item*>> items = Item::stackItems(available);
    if (items.empty())
      break;
    int budget = collective->numResource(ResourceId("GOLD"));
    vector<ItemInfo> itemInfo = items.transform(
        [&] (const vector<Item*>& it) { return getTradeItemInfo(getGame()->getContentFactory(), it, budget); });
    auto index = getView()->chooseTradeItem("Trade with " + ally->getName()->full,
        {ViewId("gold"), collective->numResource(ResourceId("GOLD"))}, itemInfo, &scrollPos);
    if (!index)
      break;
    for (Item* it : available)
      if (it->getUniqueId() == *index && it->getPrice() <= budget) {
        collective->takeResource({ResourceId("GOLD"), it->getPrice()});
        Random.choose(storage).dropItem(ally->buyItem(it));
      }
    getView()->updateView(this, true);
  }
}

static ItemInfo getPillageItemInfo(const ContentFactory* factory, const vector<Item*>& stack, bool noStorage) {
  return CONSTRUCT(ItemInfo,
    c.name = stack[0]->getShortName(nullptr, stack.size() > 1);
    c.fullName = stack[0]->getNameAndModifiers(false);
    c.description = stack[0]->getDescription(factory);
    c.number = stack.size();
    c.viewId = stack[0]->getViewObject().id();
    for (auto it : stack)
      c.ids.insert(it->getUniqueId());
    c.unavailable = noStorage;
    c.unavailableReason = noStorage ? "No storage is available for this item." : "";
  );
}

vector<PItem> PlayerControl::retrievePillageItems(Collective* col, vector<Item*> items) {
  vector<PItem> ret;
  EntitySet<Item> index(items);
  for (auto pos : col->getTerritory().getAll()) {
    bool update = false;
    for (auto item : copyOf(pos.getInventory().getItems()))
      if (index.contains(item)) {
        ret.push_back(pos.removeItem(item));
        update = true;
      }
    if (update)
      addToMemory(pos);
  }
  return ret;
}

vector<Item*> PlayerControl::getPillagedItems(Collective* col) const {
  vector<Item*> ret;
  for (Position v : col->getTerritory().getAll()) {
    if (!collective->getTerritory().contains(v))
      append(ret, v.getItems().filter([this, v](auto item) { return !collective->getStorageForPillagedItem(item).count(v); }));
  }
  return ret;
}

bool PlayerControl::canPillage(const Collective* col) const {
  for (Position v : col->getTerritory().getAll())
    if (!collective->getTerritory().contains(v) && !v.getItems().empty())
      return true;
  return false;
}

void PlayerControl::handlePillage(Collective* col) {
  ScrollPosition scrollPos;
  while (1) {
    struct PillageOption {
      vector<Item*> items;
      PositionSet storage;
    };
    vector<PillageOption> options;
    for (auto& elem : Item::stackItems(getPillagedItems(col)))
      options.push_back({elem, collective->getStorageForPillagedItem(elem.front())});
    if (options.empty())
      return;
    vector<ItemInfo> itemInfo = options.transform([&] (const PillageOption& it) {
            return getPillageItemInfo(getGame()->getContentFactory(), it.items, it.storage.empty());});
    auto index = getView()->choosePillageItem("Pillage " + col->getName()->full, itemInfo, &scrollPos);
    if (!index)
      break;
    CHECK(!options[*index].storage.empty());
    Random.choose(options[*index].storage).dropItems(retrievePillageItems(col, options[*index].items));
    if (auto& name = col->getName())
      collective->addRecordedEvent("the pillaging of " + name->full);
    getView()->updateView(this, true);
  }
}

void PlayerControl::handleRansom(bool pay) {
  if (ransomAttacks.empty())
    return;
  auto& ransom = ransomAttacks.front();
  int amount = *ransom.getRansom();
  if (pay && collective->hasResource({ResourceId("GOLD"), amount})) {
    collective->takeResource({ResourceId("GOLD"), amount});
    ransom.getAttacker()->onRansomPaid();
  }
  ransomAttacks.removeIndex(0);
}

vector<Collective*> PlayerControl::getKnownVillains() const {
  auto showAll = getGame()->getOptions()->getBoolValue(OptionId::SHOW_MAP);
  return getGame()->getCollectives().filter([&](Collective* c) {
      return showAll || collective->isKnownVillain(c) || !c->getTriggers(collective).empty();});
}

ViewId PlayerControl::getMinionGroupViewId(Creature* c) const {
  if (collective->hasTrait(c, MinionTrait::PRISONER)) {
    return ViewId("prisoner");
  } else
    return c->getViewObject().id();
}

vector<Creature*> PlayerControl::getMinionsLike(Creature* like) const {
  vector<Creature*> minions;
  for (Creature* c : getCreatures())
    if (collective->getMinionGroupName(c) == collective->getMinionGroupName(like))
      minions.push_back(c);
  return minions;
}

void PlayerControl::sortMinionsForUI(vector<Creature*>& minions) const {
  std::sort(minions.begin(), minions.end(), [] (const Creature* c1, const Creature* c2) {
      auto l1 = (int) max(c1->getAttr(AttrType::DAMAGE), c1->getAttr(AttrType::SPELL_DAMAGE));
      auto l2 = (int) max(c2->getAttr(AttrType::DAMAGE), c2->getAttr(AttrType::SPELL_DAMAGE));
      return l1 > l2 || (l1 == l2 && c1->getUniqueId() > c2->getUniqueId());
      });
}

vector<PlayerInfo> PlayerControl::getPlayerInfos(vector<Creature*> creatures, UniqueEntity<Creature>::Id chosenId) const {
  PROFILE;
  sortMinionsForUI(creatures);
  vector<PlayerInfo> minions;
  for (Creature* c : creatures) {
    minions.emplace_back(c, getGame()->getContentFactory());
    auto& minionInfo = minions.back();
    minionInfo.groupName = collective->getMinionGroupName(c);
    // only fill equipment for the chosen minion to avoid lag
    if (c->getUniqueId() == chosenId) {
      for (auto expType : ENUM_ALL(ExperienceType))
        if (auto requiredDummy = collective->getMissingTrainingFurniture(c, expType))
          minionInfo.experienceInfo.warning[expType] =
              "Requires " + getGame()->getContentFactory()->furniture.getData(*requiredDummy).getName() + " to train further.";
      for (MinionActivity t : ENUM_ALL(MinionActivity))
        if (c->getAttributes().getMinionActivities().isAvailable(collective, c, t, true)) {
          minionInfo.minionTasks.push_back({t,
              !collective->isActivityGood(c, t, true),
              collective->getCurrentActivity(c).activity == t,
              c->getAttributes().getMinionActivities().isLocked(t),
              collective->getGroupLockedActivities(c).contains(t)});
        }
      if (collective->usesEquipment(c))
        fillEquipment(c, minionInfo);
      if (c->getAttributes().getAutomatonSlots() > 0)
        fillAutomatonParts(c, minionInfo);
      if (canControlSingle(c))
        minionInfo.actions.push_back(PlayerInfo::CONTROL);
      if (!collective->hasTrait(c, MinionTrait::PRISONER)) {
        minionInfo.actions.push_back(PlayerInfo::RENAME);
      } else
        minionInfo.experienceInfo.limit.clear();
      auto& leaders = collective->getLeaders();
      if (c->getAttributes().getAutomatonSlots() > 0)
        minionInfo.actions.push_back(PlayerInfo::DISASSEMBLE);
      else if (leaders.size() > 1 || !collective->hasTrait(c, MinionTrait::LEADER))
        minionInfo.actions.push_back(PlayerInfo::BANISH);
      if (!collective->hasTrait(c, MinionTrait::WORKER)) {
        minionInfo.canAssignQuarters = true;
        auto& quarters = collective->getQuarters();
        if (auto index = quarters.getAssigned(c->getUniqueId()))
          minionInfo.quarters = quarters.getAllQuarters()[*index].viewId;
        else
          minionInfo.quarters = none;
      } else
        minionInfo.canAssignQuarters = false;
      if (c->isAffected(LastingEffect::CONSUMPTION_SKILL))
        minionInfo.actions.push_back(PlayerInfo::CONSUME);
      minionInfo.actions.push_back(PlayerInfo::LOCATE);
    }
  }
  return minions;
}

vector<CollectiveInfo::CreatureGroup> PlayerControl::getCreatureGroups(vector<Creature*> v) const {
  sortMinionsForUI(v);
  map<string, CollectiveInfo::CreatureGroup> groups;
  for (Creature* c : v) {
    auto groupName = collective->getMinionGroupName(c);
    auto viewId = getMinionGroupViewId(c);
    if (!groups.count(groupName))
      groups[groupName] = { c->getUniqueId(), groupName, viewId, 0};
    ++groups[groupName].count;
    if (chosenCreature == c->getUniqueId() && !getChosenTeam())
      groups[groupName].highlight = true;
  }
  return getValues(groups);
}

void PlayerControl::fillMinions(CollectiveInfo& info) const {
  vector<Creature*> minions;
  for (auto trait : {MinionTrait::FIGHTER, MinionTrait::PRISONER, MinionTrait::WORKER, MinionTrait::INCREASE_POPULATION, MinionTrait::LEADER})
    for (Creature* c : collective->getCreatures(trait))
      if (!minions.contains(c))
        minions.push_back(c);
  info.minionGroups = getCreatureGroups(minions);
  info.minions = minions.transform([](const Creature* c) { return CreatureInfo(c) ;});
  info.minionCount = collective->getPopulationSize();
  info.minionLimit = collective->getMaxPopulation();
  info.populationString = collective->getConfig().getPopulationString();
}

ItemInfo PlayerControl::getWorkshopItem(const WorkshopItem& option, int queuedCount) const {
  return CONSTRUCT(ItemInfo,
      c.number = queuedCount * option.batchSize;
      c.name = c.number == 1 ? option.name : toString(c.number) + " " + option.pluralName;
      c.viewId = option.viewId;
      c.price = getCostObj(option.cost * queuedCount);
      if (option.techId && !collective->getTechnology().researched.count(*option.techId)) {
        c.unavailable = true;
        c.unavailableReason = "Requires technology: "_s + option.techId->data();
      }
      c.description = option.description;
      c.tutorialHighlight = tutorial && option.tutorialHighlight &&
          tutorial->getHighlights(getGame()).contains(*option.tutorialHighlight);
    );
}

void PlayerControl::acquireTech(TechId tech) {
  auto techs = collective->getTechnology().getNextTechs();
  if (techs.contains(tech)) {
    if (collective->getDungeonLevel().numResearchAvailable() > 0) {
      collective->acquireTech(tech, true);
    }
  }
}

void PlayerControl::fillTechUnlocks(CollectiveInfo::LibraryInfo::TechInfo& techInfo) const {
  auto tech = techInfo.id;
  for (auto& t2 : collective->getTechnology().getAllowed(tech))
    techInfo.unlocks.push_back({ViewId("book"), t2.data(), "technology"});
  for (auto& elem : buildInfo)
    for (auto& r : elem.requirements)
      if (r == tech)
        techInfo.unlocks.push_back({getViewId(elem), elem.name, "constructions"});
  for (auto& workshop : collective->getWorkshops().types)
    for (auto& option : workshop.second.getOptions())
      if (option.techId == tech)
        techInfo.unlocks.push_back({option.viewId, option.name, "workshop items"});
  auto& creatureFactory = getGame()->getContentFactory()->getCreatures();
  for (auto& immigrant : collective->getImmigration().getImmigrants())
    for (auto& req : immigrant.requirements)
      if (req.type.getValueMaybe<TechId>() == tech) {
        techInfo.unlocks.push_back({creatureFactory.getViewId(immigrant.getId(0)),
            creatureFactory.getName(immigrant.getId(0)), "immigrants"});
      }
}

void PlayerControl::fillLibraryInfo(CollectiveInfo& collectiveInfo) const {
  auto& info = collectiveInfo.libraryInfo;
  auto& dungeonLevel = collective->getDungeonLevel();
  if (dungeonLevel.numResearchAvailable() == 0)
    info.warning = "Conquer some villains to advance your level."_s;
  info.totalProgress = 100 * dungeonLevel.getNecessaryProgress(dungeonLevel.level);
  info.currentProgress = int(100 * dungeonLevel.progress * dungeonLevel.getNecessaryProgress(dungeonLevel.level));
  auto& technology = collective->getTechnology();
  auto techs = technology.getNextTechs();
  for (auto& tech : techs) {
    info.available.emplace_back();
    auto& techInfo = info.available.back();
    techInfo.id = tech;
    techInfo.active = !info.warning && dungeonLevel.numResearchAvailable() > 0;
    fillTechUnlocks(techInfo);
  }
  for (auto& tech : collective->getTechnology().researched) {
    info.researched.emplace_back();
    auto& techInfo = info.researched.back();
    techInfo.id = tech;
    fillTechUnlocks(techInfo);
  }
}

vector<pair<vector<Item*>, Position>> PlayerControl::getItemUpgradesFor(const WorkshopItem& workshopItem) const {
  vector<pair<vector<Item*>, Position>> ret;
  for (auto& pos : collective->getStoragePositions(StorageId::EQUIPMENT))
    for (auto& item : pos.getItems(ItemIndex::RUNE))
      if (auto& upgradeInfo = item->getUpgradeInfo())
        if (upgradeInfo->type == workshopItem.upgradeType) {
          /*for (auto& existing : ret)
            if (existing.first[0]->getUpgradeInfo() == upgradeInfo) {
              existing.first.push_back(item);
              goto found;
            }*/
          ret.push_back({{item}, pos});
          //found:;
        }
  return ret;
}

struct WorkshopOptionInfo {
  ItemInfo itemInfo;
  int optionIndex;
  optional<pair<Item*, Position>> ingredient;
};

vector<WorkshopOptionInfo> PlayerControl::getWorkshopOptions() const {
  vector<WorkshopOptionInfo> ret;
  auto& options = collective->getWorkshops().types.at(*chosenWorkshop).getOptions();
  for (int i : All(options)) {
    auto& option = options[i];
    auto itemInfo = getWorkshopItem(option, 1);
    if (option.requireIngredient) {
      for (auto& pos : collective->getStoragePositions(StorageId::EQUIPMENT))
        for (auto& item : pos.getItems(ItemIndex::RUNE))
          if (item->getIngredientType() == option.requireIngredient) {
            auto it = itemInfo;
            it.ingredient = getItemInfo(getGame()->getContentFactory(), {item}, false, false, false);
            it.description.push_back("Crafted from " + item->getName());
            ret.push_back({it, i, make_pair(item, pos)});
          }
      }
    else
      ret.push_back({itemInfo, i, none});
  }
  return ret;
}

CollectiveInfo::QueuedItemInfo PlayerControl::getQueuedItemInfo(const WorkshopQueuedItem& item, int cnt,
    int itemIndex) const {
  CollectiveInfo::QueuedItemInfo ret {item.state, item.paid, getWorkshopItem(item.item, cnt), {}, {}, 0, itemIndex};
  if (!item.paid)
    ret.itemInfo.description.push_back("Cannot afford item");
  for (auto& it : getItemUpgradesFor(item.item)) {
    ret.available.push_back({it.first[0]->getViewObject().id(), it.first[0]->getName(), it.first.size(),
        it.first[0]->getUpgradeInfo()->getDescription(getGame()->getContentFactory())});
  }
  for (auto& it : item.runes) {
    if (auto& upgradeInfo = it->getUpgradeInfo())
      ret.added.push_back({it->getViewObject().id(), it->getName(), 1,
          upgradeInfo->getDescription(getGame()->getContentFactory())});
    else {
      ret.itemInfo.ingredient = getItemInfo(getGame()->getContentFactory(), {it.get()}, false, false, false);
      ret.itemInfo.description.push_back("Crafted from " + it->getName());
    }
  }
  if (!item.runes.empty() && !item.item.notArtifact)
    ret.itemInfo.description.push_back("Requires a craftsman of legendary skills.");
  ret.itemInfo.actions = {ItemAction::REMOVE};
  ret.maxUpgrades = item.item.maxUpgrades;
  return ret;
}

vector<CollectiveInfo::QueuedItemInfo> PlayerControl::getQueuedWorkshopItems() const {
  vector<CollectiveInfo::QueuedItemInfo> ret;
  auto& queued = collective->getWorkshops().types.at(*chosenWorkshop).getQueued();
  for (int i : All(queued)) {
    if (i > 0 && queued[i - 1].indexInWorkshop == queued[i].indexInWorkshop && queued[i - 1].paid == queued[i].paid &&
        queued[i].runes.empty() && queued[i - 1].runes.empty() && queued[i].state == 0 && queued[i - 1].state == 0)
      ret.back() = getQueuedItemInfo(queued[i], ret.back().itemInfo.number + 1, ret.back().itemIndex);
    else
      ret.push_back(getQueuedItemInfo(queued[i], 1, i));
  }
  return ret;
}

void PlayerControl::fillWorkshopInfo(CollectiveInfo& info) const {
  info.workshopButtons.clear();
  int index = 0;
  int i = 0;
  for (auto workshopType : collective->getWorkshops().getWorkshopsTypes()) {
    auto& workshopInfo = getGame()->getContentFactory()->workshopInfo.at(workshopType);
    bool unavailable = collective->getConstructions().getBuiltPositions(workshopInfo.furniture).empty();
    info.workshopButtons.push_back({capitalFirst(workshopInfo.name),
        getGame()->getContentFactory()->furniture.getConstructionObject(workshopInfo.furniture).id(), false, unavailable});
    if (chosenWorkshop == workshopType) {
      index = i;
      info.workshopButtons.back().active = true;
    }
    ++i;
  }
  if (chosenWorkshop)
    info.chosenWorkshop = CollectiveInfo::ChosenWorkshopInfo {
        getWorkshopOptions().transform([](auto& option) { return option.itemInfo; }),
        getQueuedWorkshopItems(),
        index
    };
}

void PlayerControl::acceptPrisoner(int index) {
  index = -index - 1;
  auto immigrants = getPrisonerImmigrantStack();
  if (index < immigrants.size() && !immigrants[index].collective) {
    auto victim = immigrants[index].creatures[0];
    victim->removeEffect(LastingEffect::STUNNED);
    victim->getAttributes().getSkills().setValue(SkillId::DIGGING,
        victim->isAffected(LastingEffect::NAVIGATION_DIGGING_SKILL) ? 1 : 0.2);
    victim->removePermanentEffect(LastingEffect::NAVIGATION_DIGGING_SKILL);
    victim->removePermanentEffect(LastingEffect::BRIDGE_BUILDING_SKILL);
    collective->addCreature(victim, {MinionTrait::WORKER, MinionTrait::PRISONER, MinionTrait::NO_LIMIT});
    addMessage(PlayerMessage("You enslave " + victim->getName().a()).setPosition(victim->getPosition()));
    for (auto& elem : copyOf(stunnedCreatures))
      if (elem.first == victim)
        stunnedCreatures.removeElement(elem);
  }
}

void PlayerControl::rejectPrisoner(int index) {
  index = -index - 1;
  auto immigrants = getPrisonerImmigrantStack();
  if (index < immigrants.size()) {
    auto victim = immigrants[index].creatures[0];
    victim->dieWithLastAttacker();
  }
}

vector<PlayerControl::StunnedInfo> PlayerControl::getPrisonerImmigrantStack() const {
  vector<StunnedInfo> ret;
  vector<Creature*> outside;
  unordered_map<Collective*, vector<Creature*>, CustomHash<Collective*>> inside;
  for (auto stunned : stunnedCreatures)
    if (stunned.first->isAffected(LastingEffect::STUNNED) && !stunned.first->isDead()) {
      if (auto villain = stunned.second) {
        if (villain->isConquered() || !villain->getTerritory().contains(stunned.first->getPosition()))
          outside.push_back(stunned.first);
        else
          inside[villain].push_back(stunned.first);
      } else
        outside.push_back(stunned.first);
    }
  for (auto& elem : inside)
    for (auto& stack : Creature::stack(elem.second))
      ret.push_back(StunnedInfo{stack, elem.first});
  for (auto& stack : Creature::stack(outside))
    ret.push_back(StunnedInfo{stack, nullptr});
  return ret;
}

vector<ImmigrantDataInfo> PlayerControl::getPrisonerImmigrantData() const {
  vector<ImmigrantDataInfo> ret;
  int index = -1;
  for (auto stack : getPrisonerImmigrantStack()) {
    auto c = stack.creatures[0];
    const int numFreePrisoners = 4;
    const int requiredPrisonSize = 2;
    const int numPrisoners = collective->getCreatures(MinionTrait::PRISONER).size() - numFreePrisoners;
    const int prisonSize = collective->getConstructions().getBuiltCount(FurnitureType("PRISON"));
    vector<string> requirements;
    const int missingSize = (numPrisoners + 1) * requiredPrisonSize - prisonSize;
    if (missingSize > 0) {
      if (prisonSize == 0)
        requirements.push_back("Requires a prison.");
      else
        requirements.push_back("Requires " + toString(missingSize) + " more prison tiles.");
    }
    if (stack.collective)
      requirements.push_back("Requires conquering " + stack.collective->getName()->full);
    ret.push_back(ImmigrantDataInfo());
    ret.back().requirements = requirements;
    ret.back().name = c->getName().bare() + " (prisoner)";
    ret.back().viewId = c->getViewObject().id();
    ret.back().attributes = AttributeInfo::fromCreature(c);
    ret.back().count = stack.creatures.size() == 1 ? none : optional<int>(stack.creatures.size());
    ret.back().timeLeft = c->getTimeRemaining(LastingEffect::STUNNED);
    ret.back().id = index;
    --index;
  }
  return ret;
}

static ImmigrantDataInfo::SpecialTraitInfo getSpecialTraitInfo(const SpecialTrait& trait, const ContentFactory* factory) {
  using TraitInfo = ImmigrantDataInfo::SpecialTraitInfo;
  return trait.visit<ImmigrantDataInfo::SpecialTraitInfo>(
      [&] (const ExtraTraining& t) {
        return TraitInfo{"Extra "_s + toLower(getName(t.type)) + " training potential", false};
      },
      [&] (const AttrBonus& t) {
        return TraitInfo{toStringWithSign(t.increase) + " " + getName(t.attr), t.increase <= 0};
      },
      [&] (const Lasting& effect) {
        if (effect.time) {
          if (auto adj = LastingEffects::getGoodAdjective(effect.effect))
            return TraitInfo{"Temporary trait: "_s + *adj + " (" + toString(*effect.time) + " turns)", false};
          if (auto adj = LastingEffects::getBadAdjective(effect.effect))
            return TraitInfo{"Temporary trait: "_s + *adj + " (" + toString(*effect.time) + " turns)", true};
        } else {
          if (auto adj = LastingEffects::getGoodAdjective(effect.effect))
            return TraitInfo{"Permanent trait: "_s + *adj, false};
          if (auto adj = LastingEffects::getBadAdjective(effect.effect))
            return TraitInfo{"Permanent trait: "_s + *adj, true};
        }
        FATAL << "No adjective found: "_s + LastingEffects::getName(effect.effect);
        fail();
      },
      [&] (WorkshopType type) {
        return TraitInfo{"Legendary craftsman at: " + factory->workshopInfo.at(type).name, false};
      },
      [&] (ExtraBodyPart part) {
        if (part.count == 1)
          return TraitInfo{"Extra "_s + getName(part.part), false};
        else
          return TraitInfo{toString(part.count) + " extra "_s + getName(part.part) + "s", false};
      },
      [&] (const ExtraIntrinsicAttack& a) {
        return TraitInfo{capitalFirst(a.item.get(factory)->getName()), false};
      },
      [&] (const OneOfTraits&) -> TraitInfo {
        FATAL << "Can't draw traits alternative";
        fail();
      }
  );
}

void PlayerControl::fillImmigration(CollectiveInfo& info) const {
  PROFILE;
  info.immigration.clear();
  auto& immigration = collective->getImmigration();
  info.immigration.append(getPrisonerImmigrantData());
  for (auto& elem : immigration.getAvailable()) {
    const auto& candidate = elem.second.get();
    if (candidate.getInfo().isInvisible())
      continue;
    const int count = (int) candidate.getCreatures().size();
    optional<TimeInterval> timeRemaining;
    if (auto time = candidate.getEndTime())
      timeRemaining = *time - getGame()->getGlobalTime();
    vector<string> infoLines;
    candidate.getInfo().visitRequirements(makeVisitor(
        [&](const Pregnancy&) {
          optional<TimeInterval> maxT;
          for (Creature* c : collective->getCreatures())
            if (c->isAffected(LastingEffect::PREGNANT))
              if (auto remaining = c->getTimeRemaining(LastingEffect::PREGNANT))
                if (!maxT || *remaining > *maxT)
                  maxT = *remaining;
          if (maxT && (!timeRemaining || *maxT > *timeRemaining))
            timeRemaining = *maxT;
        },
        [&](const RecruitmentInfo& info) {
          infoLines.push_back(
              toString(info.getAvailableRecruits(getGame(), candidate.getInfo().getNonRandomId(0)).size()) +
              " recruits available");
        },
        [&](const auto&) {}
    ));
    Creature* c = candidate.getCreatures()[0];
    string name = count > 1 ? c->getName().groupOf(count) : c->getName().title();
    if (auto& s = c->getName().stackOnly())
      name += " (" + *s + ")";
    if (count > 1)
      infoLines.push_back("The entire group takes up one population spot");
		if(count == 1 && c->getAttributes().isAffectedPermanently(LastingEffect::COPULATION_SKILL)){
			infoLines.push_back(getName(c->getAttributes().getGender()));
		}	
    for (auto trait : candidate.getInfo().getTraits())
      if (auto desc = getImmigrantDescription(trait))
        infoLines.push_back(desc);
    info.immigration.push_back(ImmigrantDataInfo());
    info.immigration.back().requirements = immigration.getMissingRequirements(candidate);
    info.immigration.back().info = infoLines;
    info.immigration.back().specialTraits = candidate.getSpecialTraits().transform(
        [&](const auto& trait){ return getSpecialTraitInfo(trait, getGame()->getContentFactory()); });
    info.immigration.back().cost = getCostObj(candidate.getCost());
    info.immigration.back().name = name;
    info.immigration.back().viewId = c->getViewObject().id();
    info.immigration.back().attributes = AttributeInfo::fromCreature(c);
    info.immigration.back().count = count == 1 ? none : optional<int>(count);
    info.immigration.back().timeLeft = timeRemaining;
    info.immigration.back().id = elem.first;
    info.immigration.back().generatedTime = candidate.getCreatedTime();
    info.immigration.back().keybinding = candidate.getInfo().getKeybinding();
    info.immigration.back().tutorialHighlight = candidate.getInfo().getTutorialHighlight();
  }
  sort(info.immigration.begin(), info.immigration.end(),
      [](const ImmigrantDataInfo& i1, const ImmigrantDataInfo& i2) {
        return (i1.timeLeft && (!i2.timeLeft || *i1.timeLeft > *i2.timeLeft)) ||
            (!i1.timeLeft && !i2.timeLeft && i1.id > i2.id);
      });
}

void PlayerControl::fillImmigrationHelp(CollectiveInfo& info) const {
  info.allImmigration.clear();
  static map<CreatureId, PCreature> creatureStats;
  auto getStats = [&](CreatureId id) -> Creature* {
    if (!creatureStats[id]) {
      creatureStats[id] = getGame()->getContentFactory()->getCreatures().fromId(id, TribeId::getDarkKeeper());
    }
    return creatureStats[id].get();
  };
  for (auto elem : Iter(collective->getImmigration().getImmigrants())) {
    if (elem->isHiddenInHelp())
      continue;
    auto creatureId = elem->getNonRandomId(0);
    Creature* c = getStats(creatureId);
    optional<pair<ViewId, int>> costObj;
    vector<string> requirements;
    vector<string> infoLines;		
    elem->visitRequirements(makeVisitor(
        [&](const AttractionInfo& attraction) {
          int required = attraction.amountClaimed;
          requirements.push_back("Requires " + toString(required) + " " +
              combineWithOr(attraction.types.transform([&](const AttractionType& type) {
                return AttractionInfo::getAttractionName(collective->getGame()->getContentFactory(), type, required); })));
        },
        [&](const TechId& techId) {
          requirements.push_back("Requires technology: "_s + techId.data());
        },
        [&](const SunlightState& state) {
          requirements.push_back("Will only join during the "_s + SunlightInfo::getText(state));
        },
        [&](const FurnitureType& type) {
          requirements.push_back("Requires at least one " + getGame()->getContentFactory()->furniture.getData(type).getName());
        },
        [&](const CostInfo& cost) {
          costObj = getCostObj(cost);
          if (!costObj && cost.id == ResourceId("DEMON_PIETY"))
            requirements.push_back("Requires " + collective->getResourceInfo(cost.id).name);
        },
        [&](const ExponentialCost& cost) {
          auto& resourceInfo = collective->getResourceInfo(cost.base.id);
          if (resourceInfo.viewId)
            costObj = make_pair(*resourceInfo.viewId, (int) cost.base.value);
          infoLines.push_back("Cost doubles for every " + toString(cost.numToDoubleCost) + " "
              + c->getName().plural());
          if (cost.numFree > 0)
            infoLines.push_back("First " + toString(cost.numFree) + " " + c->getName().plural() + " are free");
        },
        [&](const Pregnancy&) {
          requirements.push_back("Requires a pregnant minion");
        },
        [&](const RecruitmentInfo& info) {
          if (!info.findEnemy(getGame()).empty())
            requirements.push_back("Ally must be discovered and have recruits available");
          else
            requirements.push_back("Recruit is not available in this game");
        },
        [&](const TutorialRequirement&) {
        },
        [&](const MinTurnRequirement&) {
        },
        [&](const NegateRequirement&) {
        }
    ));
    if (auto limit = elem->getLimit())
      infoLines.push_back("Limited to " + toString(*limit) + " creatures");
    for (auto trait : elem->getTraits())
      if (auto desc = getImmigrantDescription(trait))
        infoLines.push_back(desc);
    info.allImmigration.push_back(ImmigrantDataInfo());
    info.allImmigration.back().requirements = requirements;
    info.allImmigration.back().info = infoLines;
    info.allImmigration.back().cost = costObj;
    info.allImmigration.back().name = c->getName().stack();
    info.allImmigration.back().viewId = c->getViewObject().id();
    info.allImmigration.back().attributes = AttributeInfo::fromCreature(c);
    info.allImmigration.back().id = elem.index();
    info.allImmigration.back().autoState = collective->getImmigration().getAutoState(elem.index());
  }
  info.allImmigration.push_back(ImmigrantDataInfo());
  info.allImmigration.back().requirements = {"Requires 2 prison tiles", "Requires knocking out a hostile creature"};
  info.allImmigration.back().info = {"Supplies your imp force", "Can be converted to your side using torture"};
  info.allImmigration.back().name = "prisoner";
  info.allImmigration.back().viewId = ViewId("prisoner");
  info.allImmigration.back().id =-1;
}

static optional<CollectiveInfo::RebellionChance> getRebellionChance(double prob) {
  if (prob > 0.6)
    return CollectiveInfo::RebellionChance::HIGH;
  if (prob > 0.2)
    return CollectiveInfo::RebellionChance::MEDIUM;
  if (prob > 0)
    return CollectiveInfo::RebellionChance::LOW;
  return none;
}

void PlayerControl::fillCurrentLevelInfo(GameInfo& gameInfo) const {
  auto level = getCurrentLevel();
    gameInfo.currentLevel = CurrentLevelInfo {
    level->name,
    level->depth,
    getModel()->getMainLevels().transform([](auto level) { return level->name; }),
  };
}

void PlayerControl::fillDungeonLevel(AvatarLevelInfo& info) const {
  const auto& dungeonLevel = collective->getDungeonLevel();
  info.level = dungeonLevel.level + 1;
  if (auto leader = collective->getLeaders().getFirstElement()) {
    info.viewId = leader[0]->getViewObject().id();
    info.title = leader[0]->getName().title();
  }
  info.progress = dungeonLevel.progress;
  info.numAvailable = min(dungeonLevel.numResearchAvailable(), collective->getTechnology().getNextTechs().size());
}

void PlayerControl::fillResources(CollectiveInfo& info) const {
  info.numResource.clear();
  for (auto& resource : getGame()->getContentFactory()->resourceOrder) {
    auto& elem = getGame()->getContentFactory()->resourceInfo.at(resource);
    if (elem.viewId)
      info.numResource.push_back(
          {*elem.viewId, collective->numResourcePlusDebt(resource), elem.name, elem.tutorialHighlight});
  }
}

struct PlayerControl::KeeperDangerInfo {
  Creature* c;
  string warning;
};

void PlayerControl::refreshGameInfo(GameInfo& gameInfo) const {
  if (getGame()->getOptions()->getBoolValue(OptionId::KEEPER_WARNING))
    if (auto info = checkKeeperDanger())
      gameInfo.keeperInDanger = info->warning;
  gameInfo.isSingleMap = getGame()->isSingleModel();
  getGame()->getEncyclopedia()->setKeeperThings(getGame()->getContentFactory(),
      &collective->getTechnology(), &collective->getWorkshops());
  gameInfo.encyclopedia = getGame()->getEncyclopedia();
  gameInfo.takingScreenshot = takingScreenshot;
  fillCurrentLevelInfo(gameInfo);
  if (tutorial)
    tutorial->refreshInfo(getGame(), gameInfo.tutorial);
  gameInfo.villageInfo.villages.clear();
  gameInfo.villageInfo.numMainVillains = gameInfo.villageInfo.numConqueredMainVillains = 0;
  for (auto& col : getGame()->getVillains(VillainType::MAIN)) {
    ++gameInfo.villageInfo.numMainVillains;
    if (col->isConquered())
      ++gameInfo.villageInfo.numConqueredMainVillains;
  }
  for (auto& col : getKnownVillains())
    if (col->getName() && col->isDiscoverable())
      gameInfo.villageInfo.villages.push_back(getVillageInfo(col));
  gameInfo.villageInfo.dismissedInfos = dismissedVillageInfos;
  std::stable_sort(gameInfo.villageInfo.villages.begin(), gameInfo.villageInfo.villages.end(),
       [](const auto& v1, const auto& v2) { return (int) v1.type < (int) v2.type; });
  SunlightInfo sunlightInfo = getGame()->getSunlightInfo();
  gameInfo.sunlightInfo = { sunlightInfo.getText(), sunlightInfo.getTimeRemaining() };
  gameInfo.infoType = GameInfo::InfoType::BAND;
  gameInfo.playerInfo = CollectiveInfo();
  auto& info = *gameInfo.playerInfo.getReferenceMaybe<CollectiveInfo>();
  info.buildings = fillButtons();
  fillMinions(info);
  fillImmigration(info);
  fillImmigrationHelp(info);
  info.chosenCreature.reset();
  if (chosenCreature)
    if (Creature* c = getCreature(*chosenCreature)) {
      if (!getChosenTeam())
        info.chosenCreature = CollectiveInfo::ChosenCreatureInfo {
            *chosenCreature, getPlayerInfos(getMinionsLike(c), *chosenCreature)};
      else
        info.chosenCreature = CollectiveInfo::ChosenCreatureInfo {
            *chosenCreature, getPlayerInfos(getTeams().getMembers(*getChosenTeam()), *chosenCreature), *getChosenTeam()};
    }
  fillWorkshopInfo(info);
  fillLibraryInfo(info);
  info.monsterHeader = info.populationString + ": " + toString(info.minionCount) + " / " + toString(info.minionLimit);
  fillDungeonLevel(info.avatarLevelInfo);
  fillResources(info);
  info.warning = "";
  gameInfo.time = collective->getGame()->getGlobalTime();
  gameInfo.modifiedSquares = gameInfo.totalSquares = 0;
  gameInfo.modifiedSquares += getCurrentLevel()->getNumGeneratedSquares();
  gameInfo.totalSquares += getCurrentLevel()->getNumTotalSquares();
  info.teams.clear();
  for (int i : All(getTeams().getAll())) {
    TeamId team = getTeams().getAll()[i];
    info.teams.emplace_back();
    for (Creature* c : getTeams().getMembers(team))
      info.teams.back().members.push_back(c->getUniqueId());
    info.teams.back().active = getTeams().isActive(team);
    info.teams.back().id = team;
    if (getChosenTeam() == team)
      info.teams.back().highlight = true;
  }
  gameInfo.messageBuffer = messages;
  info.taskMap.clear();
  for (WConstTask task : collective->getTaskMap().getAllTasks()) {
    optional<UniqueEntity<Creature>::Id> creature;
    if (auto c = collective->getTaskMap().getOwner(task))
      creature = c->getUniqueId();
    info.taskMap.push_back(CollectiveInfo::Task{task->getDescription(), creature, collective->getTaskMap().isPriorityTask(task)});
  }
  for (auto& elem : ransomAttacks) {
    info.ransom = CollectiveInfo::Ransom {make_pair(ViewId("gold"), *elem.getRansom()), elem.getAttackerName(),
        collective->hasResource({ResourceId("GOLD"), *elem.getRansom()})};
    break;
  }
  for (auto& elem : notifiedAttacks)
    if (elem.isOngoing())
      info.onGoingAttacks.push_back(CollectiveInfo::OnGoingAttack{elem.getAttackerViewId(), elem.getAttackerName(),
          elem.getCreatures()[0]->getUniqueId()});
  const auto maxEnemyCountdown = 500_visible;
  if (auto& enemies = getModel()->getExternalEnemies())
    if (auto nextWave = enemies->getNextWave()) {
      if (!nextWave->enemy.behaviour.contains<HalloweenKids>()) {
        auto countDown = nextWave->attackTime - getModel()->getLocalTime() + (*enemies->getStartTime() - 0_local);
        auto index = enemies->getNextWaveIndex();
        auto name = nextWave->enemy.name;
        auto viewId = nextWave->viewId;
        if (index % 6 == 5) {
          name = "Unknown";
          viewId = ViewId("unknown_monster");
        }
        if (!dismissedNextWaves.count(index) && countDown < maxEnemyCountdown)
          info.nextWave = CollectiveInfo::NextWave {
            viewId,
            name,
            countDown
          };
      }
    }
  if (auto rebellionWarning = getRebellionChance(collective->getRebellionProbability()))
    if (!lastWarningDismiss || getModel()->getLocalTime() > *lastWarningDismiss + 1000_visible)
      info.rebellionChance = *rebellionWarning;
  info.allQuarters = collective->getQuarters().getAllQuarters().transform(
      [](const auto& info) { return info.viewId; });
}

void PlayerControl::addMessage(const PlayerMessage& msg) {
  messages.push_back(msg);
  messageHistory.push_back(msg);
  if (msg.getPriority() == MessagePriority::CRITICAL) {
    getView()->stopClock();
    for (auto c : getControlled()) {
      c->privateMessage(msg);
      break;
    }
  }
}

void PlayerControl::updateMinionVisibility(const Creature* c) {
  PROFILE;
  auto visibleTiles = c->getVisibleTiles();
  for (Position pos : visibilityMap->update(c, visibleTiles))
    if (collective->addKnownTile(pos))
      updateKnownLocations(pos);
  for (Position pos : visibleTiles)
    addToMemory(pos);
}

void PlayerControl::onEvent(const GameEvent& event) {
  using namespace EventInfo;
  event.visit<void>(
      [&](const Projectile& info) {
        if (getControlled().empty() && (canSee(info.begin) || canSee(info.end)) && info.begin.isSameLevel(getCurrentLevel())) {
          getView()->animateObject(info.begin.getCoord(), info.end.getCoord(), info.viewId, info.fx);
          if (info.sound)
            getView()->addSound(*info.sound);
        }
      },
      [&](const CreatureEvent& info) {
        if (collective->getCreatures().contains(info.creature))
          addMessage(PlayerMessage(info.message).setCreature(info.creature->getUniqueId()));
      },
      [&](const VisibilityChanged& info) {
        visibilityMap->onVisibilityChanged(info.pos);
      },
      [&](const CreatureMoved& info) {
        if (getCreatures().contains(info.creature))
          updateMinionVisibility(info.creature);
      },
      [&](const ItemsEquipped& info) {
        if (info.creature->isPlayer() &&
            !collective->getMinionEquipment().tryToOwn(info.creature, info.items.getOnlyElement()))
          getView()->presentText("", "Item won't be permanently assigned to creature because the equipment slot is locked.");
      },
      [&](const WonGame&) {
        getGame()->conquered(*collective->getName()->shortened, collective->getKills().getSize(),
            (int) collective->getDangerLevel() + collective->getPoints());
        getView()->presentText("", "When you are ready, retire your dungeon and share it online. "
          "Other players will be able to invade it as adventurers. To do this, press Escape and choose \'retire\'.");
      },
      [&](const RetiredGame&) {
        if (getGame()->getVillains(VillainType::MAIN).empty())
          // No victory condition in this game, so we generate a highscore when retiring.
          getGame()->retired(*collective->getName()->shortened, collective->getKills().getSize(),
              (int) collective->getDangerLevel() + collective->getPoints());
      },
      [&](const TechbookRead& info) {
        auto tech = info.technology;
        vector<TechId> nextTechs = collective->getTechnology().getNextTechs();
        if (!collective->getTechnology().researched.count(tech)) {
          if (!nextTechs.contains(tech))
            getView()->presentText("Information", "The tome describes the knowledge of "_s + tech.data()
                + ", but you do not comprehend it.");
          else {
            getView()->presentText("Information", "You have acquired the knowledge of "_s + tech.data());
            collective->acquireTech(tech, false);
          }
        } else {
          getView()->presentText("Information", "The tome describes the knowledge of "_s + tech.data()
              + ", which you already possess.");
        }
      },
      [&](const CreatureStunned& info) {
        for (auto villain : getGame()->getCollectives())
          if (villain->getCreatures().contains(info.victim)) {
            stunnedCreatures.push_back({info.victim, villain});
            return;
          }
        stunnedCreatures.push_back({info.victim, nullptr});
      },
      [&](const CreatureKilled& info) {
        auto pos = info.victim->getPosition();
        if (canSee(pos))
          if (auto anim = info.victim->getBody().getDeathAnimation())
            getView()->animation(pos.getCoord(), *anim);
      },
      [&](const CreatureAttacked& info) {
        auto pos = info.victim->getPosition();
        if (canSee(pos) && pos.isSameLevel(getCurrentLevel())) {
          auto dir = info.attacker->getPosition().getDir(pos);
          if (dir.length8() == 1) {
            auto orientation = dir.getCardinalDir();
            if (info.damageAttr == AttrType::DAMAGE)
              getView()->animation(pos.getCoord(), AnimationId::ATTACK, orientation);
            else
              getView()->animation(FXSpawnInfo({FXName::MAGIC_MISSILE_SPLASH}, pos.getCoord(), Vec2(0, 0)));
          }
        }
      },
      [&](const FurnitureDestroyed& info) {
        if (info.type == FurnitureType("EYEBALL"))
          visibilityMap->removeEyeball(info.position);
        if (info.type == FurnitureType("PIT") && collective->getKnownTiles().isKnown(info.position))
          addToMemory(info.position);
      },
      [&](const FX& info) {
        if (getControlled().empty() && canSee(info.position) && info.position.isSameLevel(getCurrentLevel()))
          getView()->animation(FXSpawnInfo(info.fx, info.position.getCoord(), info.direction.value_or(Vec2(0, 0))));
      },
      [&](const auto&) {}
  );
}

void PlayerControl::updateKnownLocations(const Position& pos) {
  /*if (pos.getModel() == getModel())
    if (const Location* loc = pos.getLocation())
      if (!knownLocations.count(loc)) {
        knownLocations.insert(loc);
        if (auto name = loc->getName())
          addMessage(PlayerMessage("Your minions discover the location of " + *name, MessagePriority::HIGH)
              .setLocation(loc));
        else if (loc->isMarkedAsSurprise())
          addMessage(PlayerMessage("Your minions discover a new location.").setLocation(loc));
      }*/
  if (getGame()) // check in case this method is called before Game is constructed
    for (const Collective* col : getGame()->getCollectives())
      if (col != collective && col->getTerritory().contains(pos)) {
        collective->addKnownVillain(col);
        if (!collective->isKnownVillainLocation(col)) {
          collective->addKnownVillainLocation(col);
          if (col->isDiscoverable())
            if (auto& name = col->getName())
              addMessage(PlayerMessage("Your minions discover the location of " + name->full,
                  MessagePriority::HIGH).setPosition(pos));
        }
      }
}


const MapMemory& PlayerControl::getMemory() const {
  return *memory;
}

void PlayerControl::getSquareViewIndex(Position pos, bool canSee, ViewIndex& index) const {
  // use the leader as a generic viewer
  auto leaders = collective->getLeaders();
  if (leaders.empty()) { // if no leader try any creature, else bail out
    auto& creatures = collective->getCreatures();
    if (!creatures.empty())
      leaders = {creatures[0]};
    else
      return;
  }
  if (canSee)
    pos.getViewIndex(index, leaders[0]);
  else
    index.setHiddenId(pos.getTopViewId());
  if (const Creature* c = pos.getCreature())
    if (canSee) {
      index.modEquipmentCounts() = c->getEquipment().getCounts();
      index.insert(c->getViewObject());
      auto& object = index.getObject(ViewLayer::CREATURE);
      if (isEnemy(c)) {
        object.setModifier(ViewObject::Modifier::HOSTILE);
        if (c->canBeCaptured())
          object.setClickAction(c->isCaptureOrdered() ?
              ViewObjectAction::CANCEL_CAPTURE_ORDER : ViewObjectAction::ORDER_CAPTURE);
      } else
        object.getCreatureStatus().intersectWith(getDisplayedOnMinions());
    }
}

void PlayerControl::getViewIndex(Vec2 pos, ViewIndex& index) const {
  PROFILE;
  auto furnitureFactory = &getGame()->getContentFactory()->furniture;
  Position position(pos, getCurrentLevel());
  if (!position.isValid())
    return;
  bool canSeePos = canSee(position);
  getSquareViewIndex(position, canSeePos, index);
  if (!canSeePos)
    if (auto memIndex = getMemory().getViewIndex(position))
      index.mergeFromMemory(*memIndex);
  if (collective->getTerritory().contains(position)) {
    if (auto furniture = position.getFurniture(FurnitureLayer::MIDDLE)) {
      if (auto clickType = furniture->getClickType())
        if (auto& obj = furniture->getViewObject())
          if (index.hasObject(obj->layer()))
            index.getObject(obj->layer()).setClickAction(FurnitureClick::getClickAction(*clickType, position, furniture));
      auto workshopType = getGame()->getContentFactory()->getWorkshopType(furniture->getType());
      if (furniture->hasUsageType(BuiltinUsageId::STUDY) || !!workshopType)
        index.setHighlight(HighlightType::CLICKABLE_FURNITURE);
      if (chosenWorkshop && chosenWorkshop == workshopType)
        index.setHighlight(HighlightType::CLICKED_FURNITURE);
      if (draggedCreature)
        if (Creature* c = getCreature(*draggedCreature))
          if (auto task = collective->getMinionActivities().getActivityFor(collective, c, furniture->getType()))
            if (collective->isActivityGood(c, *task, true))
              index.setHighlight(HighlightType::CREATURE_DROP);
      if (furnitureFactory->getData(furniture->getType()).isRequiresLight() && position.getLightingEfficiency() < 0.99)
        index.setHighlight(HighlightType::INSUFFICIENT_LIGHT);
    }
    for (auto furniture : position.getFurniture())
      if (furniture->getLuxuryInfo().luxury > 0)
        if (auto obj = furniture->getViewObject())
          if (index.hasObject(obj->layer()))
            index.getObject(obj->layer()).setAttribute(ViewObject::Attribute::LUXURY, furniture->getLuxuryInfo().luxury);
  }
  if (collective->isMarked(position))
    index.setHighlight(collective->getMarkHighlight(position));
  if (collective->hasPriorityTasks(position))
    index.setHighlight(HighlightType::PRIORITY_TASK);
  if (!index.hasObject(ViewLayer::CREATURE))
    for (auto task : collective->getTaskMap().getTasks(position))
      if (auto viewId = task->getViewId())
          index.insert(ViewObject(*viewId, ViewLayer::CREATURE));
  if (position.isTribeForbidden(getTribeId()))
    index.setHighlight(HighlightType::FORBIDDEN_ZONE);
  collective->getZones().setHighlights(position, index);
  if (rectSelection
      && pos.inRectangle(Rectangle::boundingBox({rectSelection->corner1, rectSelection->corner2})))
    index.setHighlight(rectSelection->deselect ? HighlightType::RECT_DESELECTION : HighlightType::RECT_SELECTION);
#ifndef RELEASE
  if (getGame()->getOptions()->getBoolValue(OptionId::SHOW_MAP))
    if (collective->getKnownTiles().isKnown(position))
      index.setHighlight(HighlightType::RECT_SELECTION);
#endif
  const ConstructionMap& constructions = collective->getConstructions();
  if (auto trap = constructions.getTrap(position))
    if (!trap->isArmed())
      index.insert(furnitureFactory->getConstructionObject(trap->getType()));
  for (auto layer : ENUM_ALL(FurnitureLayer))
    if (auto f = constructions.getFurniture(position, layer))
      if (!f->isBuilt(position)) {
        auto obj = furnitureFactory->getConstructionObject(f->getFurnitureType());
        if (!f->hasTask())
          obj.setModifier(ViewObjectModifier::UNPAID);
        if (auto viewId = furnitureFactory->getData(f->getFurnitureType()).getSupportViewId(position))
          obj.setId(*viewId);
        index.insert(std::move(obj));
      }
  if (unknownLocations->contains(position))
    index.insert(ViewObject(ViewId("unknown_monster"), ViewLayer::TORCH2, "Surprise"));
}

Vec2 PlayerControl::getScrollCoord() const {
  auto currentLevel = getCurrentLevel();
  auto processTiles = [&] (const auto& tiles) -> optional<Vec2> {
    Vec2 topLeft(100000, 100000);
    Vec2 bottomRight(-100000, -100000);
    for (auto& pos : tiles)
      if (pos.isSameLevel(currentLevel)) {
        auto coord = pos.getCoord();
        topLeft.x = min(coord.x, topLeft.x);
        topLeft.y = min(coord.y, topLeft.y);
        bottomRight.x = max(coord.x, bottomRight.x);
        bottomRight.y = max(coord.y, bottomRight.y);
      }
    if (topLeft.x < 100000)
      return (topLeft + bottomRight) / 2;
    else
      return none;
  };
  if (auto pos = processTiles(collective->getTerritory().getAll()))
    return *pos;
  if (auto leader = collective->getLeaders().getFirstElement()) {
    auto keeperPos = leader[0]->getPosition();
    if (keeperPos.isSameLevel(currentLevel))
      return keeperPos.getCoord();
  }
  if (auto pos = processTiles(collective->getKnownTiles().getAll()))
    return *pos;
  return currentLevel->getBounds().middle();
}

Level* PlayerControl::getCreatureViewLevel() const {
  return getCurrentLevel();
}

static enum Selection { SELECT, DESELECT, NONE } selection = NONE;

void PlayerControl::controlSingle(Creature* c) {
  CHECK(getCreatures().contains(c));
  CHECK(!c->isDead());
  commandTeam(getTeams().create({c}));
}

Creature* PlayerControl::getCreature(UniqueEntity<Creature>::Id id) const {
  for (Creature* c : getCreatures())
    if (c->getUniqueId() == id)
      return c;
  return nullptr;
}

vector<Creature*> PlayerControl::getTeam(const Creature* c) {
  vector<Creature*> ret;
  for (auto team : getTeams().getActive(c))
    append(ret, getTeams().getMembers(team));
  return ret;
}

void PlayerControl::commandTeam(TeamId team) {
  if (!getControlled().empty())
    leaveControl();
  auto c = getTeams().getLeader(team);
  c->pushController(createMinionController(c));
  getTeams().activate(team);
  collective->freeTeamMembers(getTeams().getMembers(team));
  getView()->resetCenter();
}

void PlayerControl::toggleControlAllTeamMembers() {
  if (auto teamId = getCurrentTeam()) {
    auto members = getTeams().getMembers(*teamId);
    if (members.size() > 1) {
      if (getControlled().size() == 1) {
        for (auto c : members)
          if (!c->isPlayer() && canControlInTeam(c))
            c->pushController(createMinionController(c));
      } else
        for (auto c : members)
          if (c->isPlayer() && c != getTeams().getLeader(*teamId))
            c->popController();
    }
  }
}

optional<PlayerMessage&> PlayerControl::findMessage(PlayerMessage::Id id){
  for (auto& elem : messages)
    if (elem.getUniqueId() == id)
      return elem;
  return none;
}

CollectiveTeams& PlayerControl::getTeams() {
  return collective->getTeams();
}

const CollectiveTeams& PlayerControl::getTeams() const {
  return collective->getTeams();
}

void PlayerControl::setScrollPos(Position pos) {
  if (getModel()->getMainLevels().contains(pos.getLevel())) {
    currentLevel = pos.getLevel();
    getView()->setScrollPos(pos);
  }
}

Collective* PlayerControl::getVillain(UniqueEntity<Collective>::Id id) {
  for (auto col : getGame()->getCollectives())
    if (col->getUniqueId() == id)
      return col;
  return nullptr;
}

optional<TeamId> PlayerControl::getChosenTeam() const {
  if (chosenTeam && getTeams().exists(*chosenTeam))
    return chosenTeam;
  else
    return none;
}

void PlayerControl::setChosenCreature(optional<UniqueEntity<Creature>::Id> id) {
  clearChosenInfo();
  chosenCreature = id;
}

void PlayerControl::setChosenTeam(optional<TeamId> team, optional<UniqueEntity<Creature>::Id> creature) {
  clearChosenInfo();
  chosenTeam = team;
  chosenCreature = creature;
}

void PlayerControl::clearChosenInfo() {
  setChosenWorkshop(none);
  chosenCreature = none;
  chosenTeam = none;
}

void PlayerControl::setChosenWorkshop(optional<WorkshopType> type) {
  auto refreshHighlights = [&] {
    if (chosenWorkshop)
      for (auto pos : collective->getConstructions().getBuiltPositions(
             getGame()->getContentFactory()->workshopInfo.at(*chosenWorkshop).furniture))
        pos.setNeedsRenderUpdate(true);
  };
  refreshHighlights();
  if (type)
    clearChosenInfo();
  chosenWorkshop = type;
  refreshHighlights();
}

void PlayerControl::minionDragAndDrop(const CreatureDropInfo& info, bool creatureGroup) {
  PROFILE;
  Position pos(info.pos, getCurrentLevel());
  if (Creature* dropped = getCreature(info.creatureId)) {
    auto handle = [&] (Creature* c) {
      c->removeEffect(LastingEffect::TIED_UP);
      c->removeEffect(LastingEffect::SLEEP);
      if (auto furniture = collective->getConstructions().getFurniture(pos, FurnitureLayer::MIDDLE))
        if (auto task = collective->getMinionActivities().getActivityFor(collective, c, furniture->getFurnitureType())) {
          if (collective->isActivityGood(c, *task, true)) {
            collective->setMinionActivity(c, *task);
            collective->setTask(c, Task::goTo(pos));
            return;
          }
        }
      PTask task = Task::goToAndWait(pos, 15_visible);
      task->setViewId(ViewId("guard_post"));
      collective->setTask(c, std::move(task));
    };
    if (creatureGroup)
      for (auto c : getMinionsLike(dropped))
        handle(c);
    else
      handle(dropped);
    pos.setNeedsRenderUpdate(true);
  }
}

void PlayerControl::exitAction() {
  enum Action { SAVE, RETIRE, OPTIONS, ABANDON};
#ifdef RELEASE
  bool canRetire = !tutorial && getGame()->getPlayerCreatures().empty() && getGame()->gameWon();
#else
  bool canRetire = !tutorial && getGame()->getPlayerCreatures().empty();
#endif
  vector<ListElem> elems { "Save and exit the game",
    {"Retire", canRetire ? ListElem::NORMAL : ListElem::INACTIVE} , "Change options", "Abandon the game" };
  auto ind = getView()->chooseFromList("Would you like to:", elems);
  if (!ind)
    return;
  switch (Action(*ind)) {
    case RETIRE:
      getView()->stopClock();
      takingScreenshot = true;
      break;
    case SAVE:
      getGame()->setExitInfo(GameSaveType::KEEPER);
      break;
    case ABANDON:
      if (getView()->yesOrNoPrompt("Do you want to abandon your game? This is permanent and the save file will be removed!")) {
        getGame()->setExitInfo(ExitAndQuit());
        return;
      }
      break;
    case OPTIONS:
      getGame()->getOptions()->handle(getView(), OptionSet::GENERAL);
      break;
  }
}

void PlayerControl::handleBanishing(Creature* c) {
  auto message = c->getAttributes().getAutomatonSlots()
      ? "Do you want to disassemble " + c->getName().the() + "?"
      : "Do you want to banish " + c->getName().the() + " forever? "
          "Banishing has a negative impact on morale of other minions.";
  if (getView()->yesOrNoPrompt(message)) {
    vector<Creature*> like = getMinionsLike(c);
    sortMinionsForUI(like);
    if (like.size() > 1)
      for (int i : All(like))
        if (like[i] == c) {
          if (i < like.size() - 1)
            setChosenCreature(like[i + 1]->getUniqueId());
          else
            setChosenCreature(like[like.size() - 2]->getUniqueId());
          break;
        }
    collective->banishCreature(c);
  }
}

void PlayerControl::processInput(View* view, UserInput input) {
  switch (input.getId()) {
    case UserInputId::MESSAGE_INFO:
      if (auto message = findMessage(input.get<PlayerMessage::Id>())) {
        if (auto pos = message->getPosition())
          setScrollPos(*pos);
        else if (auto id = message->getCreature()) {
          if (const Creature* c = getCreature(*id))
            setScrollPos(c->getPosition());
        }
      }
      break;
    case UserInputId::DISMISS_VILLAGE_INFO: {
      auto& info = input.get<DismissVillageInfo>();
      dismissedVillageInfos.insert({info.collectiveId, info.infoText});
      break;
    }
    case UserInputId::CREATE_TEAM:
      if (Creature* c = getCreature(input.get<Creature::Id>()))
        if (canControlInTeam(c))
          getTeams().create({c});
      break;
    case UserInputId::CREATE_TEAM_FROM_GROUP:
      if (Creature* creature = getCreature(input.get<Creature::Id>())) {
        vector<Creature*> group = getMinionsLike(creature);
        optional<TeamId> team;
        for (Creature* c : group)
          if (canControlInTeam(c)) {
            if (!team)
              team = getTeams().create({c});
            else
              getTeams().add(*team, c);
          }
      }
      break;
    case UserInputId::CREATURE_DRAG:
      draggedCreature = input.get<Creature::Id>();
      for (auto task : ENUM_ALL(MinionActivity))
        for (auto& pos : collective->getMinionActivities().getAllPositions(collective, nullptr, task))
          pos.first.setNeedsRenderUpdate(true);
      break;
    case UserInputId::CREATURE_DRAG_DROP:
      minionDragAndDrop(input.get<CreatureDropInfo>(), false);
      draggedCreature = none;
      break;
    case UserInputId::CREATURE_GROUP_DRAG_ON_MAP:
      minionDragAndDrop(input.get<CreatureDropInfo>(), true);
      draggedCreature = none;
      break;
    case UserInputId::TEAM_DRAG_DROP: {
      auto& info = input.get<TeamDropInfo>();
      Position pos = Position(info.pos, getCurrentLevel());
      if (getTeams().exists(info.teamId))
        for (Creature* c : getTeams().getMembers(info.teamId)) {
          c->removeEffect(LastingEffect::TIED_UP);
          c->removeEffect(LastingEffect::SLEEP);
          PTask task = Task::goToAndWait(pos, 15_visible);
          task->setViewId(ViewId("guard_post"));
          collective->setTask(c, std::move(task));
          pos.setNeedsRenderUpdate(true);
        }
      break;
    }
    case UserInputId::CANCEL_TEAM:
      if (getChosenTeam() == input.get<TeamId>()) {
        setChosenTeam(none);
        chosenCreature = none;
      }
      getTeams().cancel(input.get<TeamId>());
      break;
    case UserInputId::SELECT_TEAM: {
      auto teamId = input.get<TeamId>();
      if (getChosenTeam() == teamId) {
        setChosenTeam(none);
        chosenCreature = none;
      } else
        setChosenTeam(teamId, getTeams().getLeader(teamId)->getUniqueId());
      break;
    }
    case UserInputId::ACTIVATE_TEAM:
      if (!getTeams().isActive(input.get<TeamId>()))
        getTeams().activate(input.get<TeamId>());
      else
        getTeams().deactivate(input.get<TeamId>());
      break;
    case UserInputId::TILE_CLICK: {
      Vec2 pos = input.get<Vec2>();
      if (pos.inRectangle(getCurrentLevel()->getBounds()))
        onSquareClick(Position(pos, getCurrentLevel()));
      break;
    }
    case UserInputId::DRAW_LEVEL_MAP: view->drawLevelMap(this); break;
    case UserInputId::DRAW_WORLD_MAP: getGame()->presentWorldmap(); break;
    case UserInputId::WORKSHOP: {
      auto types = collective->getWorkshops().getWorkshopsTypes();
      int index = input.get<int>();
      if (index < 0 || index >= types.size())
        setChosenWorkshop(none);
      else {
        WorkshopType type = types[index];
        if (chosenWorkshop == type)
          setChosenWorkshop(none);
        else
          setChosenWorkshop(type);
      }
      break;
    }
    case UserInputId::WORKSHOP_ADD:
      if (chosenWorkshop) {
        auto& workshop = collective->getWorkshops().types.at(*chosenWorkshop);
        int index = input.get<int>();
        auto options = getWorkshopOptions();
        auto& item = options[index];
        workshop.queue(collective, item.optionIndex);
        if (item.ingredient) {
          workshop.addUpgrade(workshop.getQueued().size() - 1, item.ingredient->second.removeItem(item.ingredient->first));
        }
      }
      break;
    case UserInputId::WORKSHOP_UPGRADE: {
      auto& info = input.get<WorkshopUpgradeInfo>();
      if (chosenWorkshop) {
        auto& workshop = collective->getWorkshops().types.at(*chosenWorkshop);
        if (info.itemIndex < workshop.getQueued().size()) {
          auto& item = workshop.getQueued()[info.itemIndex];
          if (info.remove) {
            if (info.upgradeIndex < item.runes.size())
              Random.choose(collective->getStoragePositions(StorageId::EQUIPMENT))
                  .dropItem(workshop.removeUpgrade(info.itemIndex, info.upgradeIndex));
          } else {
            auto runes = getItemUpgradesFor(item.item);
            if (info.upgradeIndex < runes.size()) {
              auto& rune = runes[info.upgradeIndex];
              workshop.addUpgrade(info.itemIndex, rune.second.removeItem(rune.first[0]));
            }
          }
        }
      }
      break;
    }
    case UserInputId::REMOVE_WORKSHOP_ITEM: {
      int itemIndex = input.get<int>();
      if (chosenWorkshop) {
        auto& workshop = collective->getWorkshops().types.at(*chosenWorkshop);
        if (itemIndex < workshop.getQueued().size()) {
          for (auto& upgrade : workshop.unqueue(collective, itemIndex))
            Random.choose(collective->getStoragePositions(StorageId::EQUIPMENT))
                .dropItem(std::move(upgrade));
        }
      }
      break;
    }
    case UserInputId::LIBRARY_ADD:
      acquireTech(input.get<TechId>());
      break;
    case UserInputId::CREATURE_GROUP_BUTTON:
      if (Creature* c = getCreature(input.get<Creature::Id>()))
        if (!chosenCreature || getChosenTeam() || !getCreature(*chosenCreature) ||
            getCreature(*chosenCreature)->getName().stack() != c->getName().stack()) {
          setChosenTeam(none);
          setChosenCreature(input.get<Creature::Id>());
          break;
        }
      setChosenTeam(none);
      chosenCreature = none;
      break;
    case UserInputId::CREATURE_MAP_CLICK: {
      if (Creature* c = Position(input.get<Vec2>(), getCurrentLevel()).getCreature()) {
        if (getCreatures().contains(c)) {
          if (!getChosenTeam() || !getTeams().contains(*getChosenTeam(), c))
            setChosenCreature(c->getUniqueId());
          else
            setChosenTeam(*chosenTeam, c->getUniqueId());
        } else
          c->toggleCaptureOrder();
      }
      break;
    }
    case UserInputId::CREATURE_BUTTON: {
      auto chosenId = input.get<Creature::Id>();
      if (Creature* c = getCreature(chosenId)) {
        if (!getChosenTeam() || !getTeams().contains(*getChosenTeam(), c))
          setChosenCreature(chosenId);
        else
          setChosenTeam(*chosenTeam, chosenId);
      }
      else
        setChosenTeam(none);
      break;
    }
    case UserInputId::CREATURE_TASK_ACTION:
      minionTaskAction(input.get<TaskActionInfo>());
      break;
    case UserInputId::CREATURE_EQUIPMENT_ACTION:
      minionEquipmentAction(input.get<EquipmentActionInfo>());
      break;
    case UserInputId::CREATURE_ADD_BODY_PART:
      if (Creature* c = getCreature(input.get<Creature::Id>()))
        addConsumableItem(c);
      break;
    case UserInputId::CREATURE_CONTROL:
      if (Creature* c = getCreature(input.get<Creature::Id>())) {
        if (getChosenTeam() && getTeams().exists(*getChosenTeam())) {
          getTeams().setLeader(*getChosenTeam(), c);
          commandTeam(*getChosenTeam());
        } else
          controlSingle(c);
        chosenCreature = none;
        setChosenTeam(none);
      }
      break;
    case UserInputId::CREATURE_RENAME:
      if (Creature* c = getCreature(input.get<RenameActionInfo>().creature))
        c->getName().setFirst(input.get<RenameActionInfo>().name);
      break;
    case UserInputId::CREATURE_CONSUME:
      if (Creature* c = getCreature(input.get<Creature::Id>())) {
        if (auto creatureId = getView()->chooseCreature("Choose minion to absorb",
            getConsumptionTargets(c).transform(
                [] (const Creature* c) { return CreatureInfo(c);}), "cancel"))
          if (Creature* consumed = getCreature(*creatureId))
            collective->setTask(c, Task::consume(consumed));
      }
      break;
    case UserInputId::CREATURE_LOCATE:
      if (Creature* c = getCreature(input.get<Creature::Id>()))
        setScrollPos(c->getPosition());
      break;
    case UserInputId::CREATURE_BANISH:
      if (Creature* c = getCreature(input.get<Creature::Id>()))
        handleBanishing(c);
      break;
    case UserInputId::GO_TO_ENEMY:
      for (auto c : getModel()->getAllCreatures())
        if (c->getUniqueId() == input.get<Creature::Id>())
          setScrollPos(c->getPosition());
      break;
    case UserInputId::ADD_GROUP_TO_TEAM: {
      auto info = input.get<TeamCreatureInfo>();
      if (Creature* creature = getCreature(info.creatureId)) {
        vector<Creature*> group = getMinionsLike(creature);
        for (Creature* c : group)
          if (getTeams().exists(info.team) && !getTeams().contains(info.team, c) && canControlInTeam(c))
            getTeams().add(info.team, c);
      }
      break;
    }
    case UserInputId::ADD_TO_TEAM: {
      auto info = input.get<TeamCreatureInfo>();
      if (Creature* c = getCreature(info.creatureId))
        if (getTeams().exists(info.team) && !getTeams().contains(info.team, c) && canControlInTeam(c))
          getTeams().add(info.team, c);
      break;
    }
    case UserInputId::REMOVE_FROM_TEAM: {
      auto info = input.get<TeamCreatureInfo>();
      if (Creature* c = getCreature(info.creatureId))
        if (getTeams().exists(info.team) && getTeams().contains(info.team, c)) {
          getTeams().remove(info.team, c);
          if (getTeams().exists(info.team)) {
            if (chosenCreature == info.creatureId)
              setChosenTeam(info.team, getTeams().getLeader(info.team)->getUniqueId());
          } else
            chosenCreature = none;
        }
      break;
    }
    case UserInputId::ASSIGN_QUARTERS: {
      auto& info = input.get<AssignQuartersInfo>();
      collective->getQuarters().assign(info.index, info.minionId);
      break;
    }
    case UserInputId::IMMIGRANT_ACCEPT: {
      int index = input.get<int>();
      if (index < 0)
        acceptPrisoner(index);
      else {
        auto available = collective->getImmigration().getAvailable();
        if (auto info = getReferenceMaybe(available, index))
          if (auto sound = info->get().getInfo().getSound())
            getView()->addSound(*sound);
        collective->getImmigration().accept(index);
      }
      break;
    }
    case UserInputId::IMMIGRANT_REJECT: {
      int index = input.get<int>();
      if (index < 0)
        rejectPrisoner(index);
      else {
        collective->getImmigration().rejectIfNonPersistent(index);
      }
      break;
    }
    case UserInputId::IMMIGRANT_AUTO_ACCEPT: {
      int id = input.get<int>();
      if (id >= 0) { // Otherwise the player has clicked the dummy prisoner element
        if (!!collective->getImmigration().getAutoState(id))
          collective->getImmigration().setAutoState(id, none);
        else
          collective->getImmigration().setAutoState(id, ImmigrantAutoState::AUTO_ACCEPT);
      }
      break;
    }
    case UserInputId::IMMIGRANT_AUTO_REJECT: {
      int id = input.get<int>();
      if (id >= 0) { // Otherwise the player has clicked the dummy prisoner element
        if (!!collective->getImmigration().getAutoState(id))
          collective->getImmigration().setAutoState(id, none);
        else
          collective->getImmigration().setAutoState(id, ImmigrantAutoState::AUTO_REJECT);
      }
      break;
    }
    case UserInputId::RECT_SELECTION: {
      auto& info = input.get<BuildingClickInfo>();
      if (buildInfo[info.building].canSelectRectangle()) {
        updateSelectionSquares();
        if (rectSelection) {
          rectSelection->corner2 = info.pos;
        } else
          rectSelection = CONSTRUCT(SelectionInfo, c.corner1 = c.corner2 = info.pos;);
        updateSelectionSquares();
      } else
        handleSelection(info.pos, buildInfo[info.building], false);
      break;
    }
    case UserInputId::RECT_DESELECTION:
      updateSelectionSquares();
      if (rectSelection) {
        rectSelection->corner2 = input.get<Vec2>();
      } else
        rectSelection = CONSTRUCT(SelectionInfo, c.corner1 = c.corner2 = input.get<Vec2>(); c.deselect = true;);
      updateSelectionSquares();
      break;
    case UserInputId::BUILD: {
      auto& info = input.get<BuildingClickInfo>();
      handleSelection(info.pos, buildInfo[info.building], false);
      break;
    }
    case UserInputId::VILLAGE_ACTION: {
      auto& info = input.get<VillageActionInfo>();
      if (Collective* village = getVillain(info.id))
        switch (info.action) {
          case VillageAction::TRADE:
            handleTrading(village);
            break;
          case VillageAction::PILLAGE:
            handlePillage(village);
            break;
        }
      break;
    }
    case UserInputId::PAY_RANSOM:
      handleRansom(true);
      break;
    case UserInputId::IGNORE_RANSOM:
      handleRansom(false);
      break;
    case UserInputId::SHOW_HISTORY:
      PlayerMessage::presentMessages(getView(), messageHistory);
      break;
    case UserInputId::CHEAT_ATTRIBUTES:
      for (auto& resource : getGame()->getContentFactory()->resourceInfo)
        collective->returnResource(CostInfo(resource.first, 1000));
      collective->getDungeonLevel().increaseLevel();
      break;
    case UserInputId::TUTORIAL_CONTINUE:
      if (tutorial)
        tutorial->continueTutorial(getGame());
      break;
    case UserInputId::TUTORIAL_GO_BACK:
      if (tutorial)
        tutorial->goBack();
      break;
    case UserInputId::RECT_CONFIRM:
      if (rectSelection) {
        selection = rectSelection->deselect ? DESELECT : SELECT;
        for (Vec2 v : Rectangle::boundingBox({rectSelection->corner1, rectSelection->corner2}))
          handleSelection(v, buildInfo[input.get<BuildingClickInfo>().building], true, rectSelection->deselect);
      }
      FALLTHROUGH;
    case UserInputId::RECT_CANCEL:
      updateSelectionSquares();
      rectSelection = none;
      selection = NONE;
      break;
    case UserInputId::EXIT: exitAction(); return;
    case UserInputId::IDLE: break;
    case UserInputId::DISMISS_NEXT_WAVE:
      if (auto& enemies = getModel()->getExternalEnemies())
        if (auto nextWave = enemies->getNextWave())
          dismissedNextWaves.insert(enemies->getNextWaveIndex());
      break;
    case UserInputId::DISMISS_WARNING_WINDOW:
      lastWarningDismiss = getModel()->getLocalTime();
      break;
    case UserInputId::SCROLL_TO_HOME:
      setScrollPos(collective->getLeaders()[0]->getPosition());
      break;
    case UserInputId::SCROLL_STAIRS:
      scrollStairs(input.get<int>());
      break;
    case UserInputId::CONTROL_KEEPER:
      if (auto info = checkKeeperDanger())
        controlSingle(info->c);
      break;
    case UserInputId::DISMISS_KEEPER_DANGER:
      nextKeeperWarning = getGame()->getGlobalTime() +
          TimeInterval(getGame()->getOptions()->getIntValue(OptionId::KEEPER_WARNING_TIMEOUT));
      break;
    case UserInputId::TAKE_SCREENSHOT:
      getView()->dungeonScreenshot(input.get<Vec2>());
      getGame()->addEvent(EventInfo::RetiredGame{});
      getGame()->setExitInfo(GameSaveType::RETIRED_SITE);
      break;
    case UserInputId::CANCEL_SCREENSHOT:
      takingScreenshot = false;
      break;
    default:
      break;
  }
}

void PlayerControl::scrollStairs(int dir) {
  if (!currentLevel)
    currentLevel = getModel()->getTopLevel();
  auto& levels = getModel()->getMainLevels();
  int index = *levels.findElement(currentLevel);
  index += dir;
  index = max(min(index, levels.size() - 1), 0);
  currentLevel = levels[index];
  getView()->updateView(this, false);
  CHECK(currentLevel);
}

vector<Creature*> PlayerControl::getConsumptionTargets(Creature* consumer) const {
  vector<Creature*> ret;
  for (Creature* c : getCreatures())
    if (consumer->canConsume(c) && (collective->getLeaders().size() > 1 || !collective->hasTrait(c, MinionTrait::LEADER)))
      ret.push_back(c);
  return ret;
}

void PlayerControl::updateSelectionSquares() {
  if (rectSelection)
    for (Vec2 v : Rectangle::boundingBox({rectSelection->corner1, rectSelection->corner2}))
      Position(v, getCurrentLevel()).setNeedsRenderUpdate(true);
}

void PlayerControl::handleSelection(Vec2 pos, const BuildInfo& building, bool rectangle, bool deselectOnly) {
  PROFILE;
  Position position(pos, getCurrentLevel());
  for (auto& req : building.requirements)
    if (!BuildInfo::meetsRequirement(collective, req))
      return;
  if (position.isUnavailable())
    return;
  if (!deselectOnly && rectangle && !building.canSelectRectangle())
    return;
  building.type.visit<void>(
    [&](const BuildInfoTypes::Trap& trap) {
      if (collective->getConstructions().getTrap(position) && selection != SELECT) {
        collective->removeTrap(position);
        getView()->addSound(SoundId::DIG_UNMARK);
        selection = DESELECT;
        // Does this mean I can remove the order if the trap physically exists?
      } else
      if (position.canEnterEmpty({MovementTrait::WALK}) &&
          collective->getTerritory().contains(position) &&
          !collective->getConstructions().getTrap(position) &&
          !collective->getConstructions().getFurniture(position, FurnitureLayer::MIDDLE) &&
          selection != DESELECT) {
        collective->addTrap(position, trap.type);
        getView()->addSound(SoundId::ADD_CONSTRUCTION);
        selection = SELECT;
      }
    },
    [&](const BuildInfoTypes::ImmediateDig&) {
      auto furniture = position.modFurniture(FurnitureLayer::MIDDLE);
      if (furniture && furniture->isWall())
        furniture->destroy(position, DestroyAction::Type::BASH);
      for (Position v : position.getRectangle(Rectangle::centered(3))) {
        addToMemory(v);
        collective->addKnownTile(v);
      }
    },
    [&](const BuildInfoTypes::PlaceItem&) {
      ItemType item;
      if (auto num = getView()->getNumber("Enter amount", Range(1, 10000), 1))
        if (auto input = getView()->getText("Enter item type", "", 100, "")) {
          if (auto error = PrettyPrinting::parseObject(item, *input))
            getView()->presentText("Sorry", "Couldn't parse \"" + *input + "\": " + *error);
          else {
              position.dropItems(item.get(*num, getGame()->getContentFactory()));
          }
        }
    },
    [&](const BuildInfoTypes::DestroyLayers& layers) {
      for (auto layer : layers) {
        auto f = collective->getConstructions().getFurniture(position, layer);
        if (f && !f->isBuilt(position)) {
          collective->removeUnbuiltFurniture(position, layer);
          getView()->addSound(SoundId::DIG_UNMARK);
          selection = SELECT;
        } else
        if (collective->getKnownTiles().isKnown(position) && !position.isBurning()) {
          selection = SELECT;
          collective->destroyOrder(position, layer);
          if (auto f = position.getFurniture(layer))
            if (f->getType() == FurnitureType("TREE_TRUNK") || f->getType() == FurnitureType("STONES"))
              position.removeFurniture(f);
          getView()->addSound(SoundId::REMOVE_CONSTRUCTION);
          updateSquareMemory(position);
        } else
          if (auto f = position.getFurniture(layer))
            if (f->getType() == FurnitureType("TREE_TRUNK") || f->getType() == FurnitureType("STONES"))
              position.removeFurniture(f);
      }
    },
    [&](const BuildInfoTypes::ForbidZone) {
      if (position.isTribeForbidden(getTribeId()) && selection != SELECT) {
        position.allowMovementForTribe(getTribeId());
        selection = DESELECT;
      }
      else if (!position.isTribeForbidden(getTribeId()) && selection != DESELECT) {
        position.forbidMovementForTribe(getTribeId());
        selection = SELECT;
      }
    },
    [&](const BuildInfoTypes::Dig&) {
      bool markedToDig = collective->isMarked(position) &&
          (collective->getMarkHighlight(position) == HighlightType::DIG ||
           collective->getMarkHighlight(position) == HighlightType::CUT_TREE);
      if (markedToDig && selection != SELECT) {
        collective->cancelMarkedTask(position);
        getView()->addSound(SoundId::DIG_UNMARK);
        selection = DESELECT;
      } else
      if (!markedToDig && selection != DESELECT) {
        if (auto furniture = position.getFurniture(FurnitureLayer::MIDDLE))
          for (auto type : {DestroyAction::Type::CUT, DestroyAction::Type::DIG})
            if (furniture->canDestroy(type)) {
              collective->orderDestruction(position, type);
              getView()->addSound(SoundId::DIG_MARK);
              selection = SELECT;
              break;
            }
      }
    },
    [&](ZoneId zone) {
      auto& zones = collective->getZones();
      if (zones.isZone(position, zone) && selection != SELECT) {
        zones.eraseZone(position, zone);
        selection = DESELECT;
      } else if (selection != DESELECT && !zones.isZone(position, zone) &&
          collective->getKnownTiles().isKnown(position) &&
          zones.canSet(position, zone, collective)) {
        zones.setZone(position, zone);
        selection = SELECT;
      }
    },
    [&](BuildInfoTypes::ClaimTile) {
      if (collective->canClaimSquare(position))
        collective->claimSquare(position);
    },
    [&](BuildInfoTypes::Dispatch) {
      collective->setPriorityTasks(position);
    },
    [&](BuildInfoTypes::PlaceMinion) {
      auto& factory = getGame()->getContentFactory()->getCreatures();
      vector<PCreature> allCreatures = factory.getAllCreatures().transform(
          [this, &factory](CreatureId id){ return factory.fromId(id, getTribeId()); });
      if (auto id = getView()->chooseCreature("Choose creature to place",
          allCreatures.transform([](auto& c) { return CreatureInfo(c.get()); }), "cancel")) {
        for (auto& c : allCreatures)
          if (c->getUniqueId() == *id) {
            collective->addCreature(std::move(c), position, {MinionTrait::FIGHTER});
          }
      }
    },
    [&](const BuildInfoTypes::Furniture& info) {
      auto layer = getGame()->getContentFactory()->furniture.getData(info.types[0]).getLayer();
      auto currentPlanned = collective->getConstructions().getFurniture(position, layer);
      if (currentPlanned && currentPlanned->isBuilt(position))
        currentPlanned = none;
      int nextIndex = 0;
      if (currentPlanned) {
        if (auto currentIndex = info.types.findElement(currentPlanned->getFurnitureType()))
          nextIndex = *currentIndex + 1;
        else
          return;
      }
      bool removed = false;
      if (!!currentPlanned && selection != SELECT) {
        collective->removeUnbuiltFurniture(position, layer);
        removed = true;
      }
      while (nextIndex < info.types.size() && !collective->canAddFurniture(position, info.types[nextIndex]))
        ++nextIndex;
      int totalCount = 0;
      for (auto type : info.types)
        totalCount += collective->getConstructions().getTotalCount(type);
      if (nextIndex < info.types.size() && selection != DESELECT &&
          (!info.limit || *info.limit > totalCount)) {
        collective->addFurniture(position, info.types[nextIndex], info.cost, info.noCredit);
        selection = SELECT;
        getView()->addSound(SoundId::ADD_CONSTRUCTION);
      } else if (removed) {
        selection = DESELECT;
        getView()->addSound(SoundId::DIG_UNMARK);
      }
    }
  );
}

void PlayerControl::onSquareClick(Position pos) {
  if (auto furniture = pos.getFurniture(FurnitureLayer::MIDDLE)) {
    if (furniture->hasUsageType(BuiltinUsageId::STAIRS)) {
      auto otherLevel = getModel()->getLinkedLevel(pos.getLevel(), *pos.getLandingLink());
      if (getModel()->getMainLevels().contains(otherLevel)) {
        currentLevel = otherLevel;
        getView()->updateView(this, false);
      }
    }
    if (collective->getTerritory().contains(pos)) {
      if (furniture->getClickType()) {
        furniture->click(pos); // this can remove the furniture
        updateSquareMemory(pos);
      } else
      if (auto workshopType = getGame()->getContentFactory()->getWorkshopType(furniture->getType()))
        if (collective->getWorkshops().getWorkshopsTypes().contains(*workshopType))
          setChosenWorkshop(*workshopType);
    }
  }
}

double PlayerControl::getAnimationTime() const {
  double localTime = getModel()->getLocalTimeDouble();
  // Snap all animations into place when the clock is paused and pausing animations has stopped.
  // This ensures that a creature that the player controlled that's an extra move ahead
  // will be positioned properly.
  if (getView()->isClockStopped() && localTime >= trunc(localTime) + pauseAnimationRemainder)
    return 10000000;
  return localTime;
}

PlayerControl::CenterType PlayerControl::getCenterType() const {
  return CenterType::NONE;
}

const vector<Vec2>& PlayerControl::getUnknownLocations(WConstLevel) const {
  PROFILE;
  return unknownLocations->getOnLevel(getCurrentLevel());
}

optional<Vec2> PlayerControl::getSelectionSize() const {
  return rectSelection.map([](const SelectionInfo& s) { return s.corner1 - s.corner2; });
}

static optional<vector<Vec2>> getCreaturePath(Creature* c, Vec2 target, Level* level) {
  auto movement = c->getMovementType();
  auto from = c->getPosition();
  auto to = Position(target, level);
  if (!to.isValid())
    return none;
  if (from.getLevel() != level) {
    if (auto stairs = to.getStairsTo(from))
      from = *stairs;
    else
      return none;
  }
  LevelShortestPath path(from, movement, to, 0);
  return path.getPath().transform([](auto& pos) { return pos.getCoord(); });
};

vector<vector<Vec2>> PlayerControl::getPathTo(UniqueEntity<Creature>::Id id, Vec2 v, bool group) const {
  vector<vector<Vec2>> ret;
  auto level = getCurrentLevel();
  if (auto creature = getCreature(id)) {
    if (group) {
      for (auto c : getMinionsLike(creature))
        if (auto path = getCreaturePath(c, v, level))
          ret.push_back(*path);
    } else
    if (auto path = getCreaturePath(creature, v, level))
      ret.push_back(*path);
  }
  return ret;
}

vector<vector<Vec2>> PlayerControl::getTeamPathTo(TeamId teamId, Vec2 v) const {
  auto teams = getTeams();
  auto level = getCurrentLevel();
  vector<vector<Vec2>> ret;
  if (teams.exists(teamId))
    for (auto c : getTeams().getMembers(teamId))
      if (auto path = getCreaturePath(c, v, level))
        ret.push_back(*path);
  return ret;
}

vector<Vec2> PlayerControl::getHighlightedPathTo(Vec2 v) const {
  auto level = getCurrentLevel();
  if (auto c = Position(v, level).getCreature()) {
    auto res = c->getCurrentPath()
        .filter([&](auto& pos) { return pos.getLevel() == level; } )
        .transform([&](auto& pos) { return pos.getCoord(); } );
    if (res.size() > 1)
      res.pop_back();
    return res;
  }
  return {};
}

void PlayerControl::addToMemory(Position pos) {
  if (!pos.needsMemoryUpdate())
    return;
  pos.setNeedsMemoryUpdate(false);
  ViewIndex index;
  getSquareViewIndex(pos, true, index);
  memory->update(pos, index);
}

optional<PlayerControl::KeeperDangerInfo> PlayerControl::checkKeeperDanger() const {
  PROFILE;
  auto controlled = getControlled();
  for (auto keeper : collective->getLeaders()) {
    auto prompt = [&] (const string& reason) {
      return KeeperDangerInfo{keeper,
          (collective->getLeaders().size() > 1 ? capitalFirst(keeper->getName().a()) : "The Keeper")
              + " " + reason + "."};
    };
    if (!keeper->isDead() && !controlled.contains(keeper) &&
        nextKeeperWarning < collective->getGlobalTime()) {
      if (auto lastCombatIntent = keeper->getLastCombatIntent())
        if (lastCombatIntent->isHostile() && lastCombatIntent->time > getGame()->getGlobalTime() - 5_visible)
          return prompt("is engaged in a fight with " + lastCombatIntent->attacker->getName().a());
      if (keeper->isAffected(LastingEffect::POISON))
        return prompt("is suffering from poisoning");
      else if (keeper->isAffected(LastingEffect::BLEEDING))
        return prompt("is bleeding");
      else if (keeper->getBody().isWounded())
        return prompt("is wounded");
    }
  }
  return none;
}

void PlayerControl::considerNightfallMessage() {
  /*if (getGame()->getSunlightInfo().getState() == SunlightState::NIGHT) {
    if (!isNight) {
      addMessage(PlayerMessage("Night is falling. Killing enemies in their sleep yields double mana.",
            MessagePriority::HIGH));
      isNight = true;
    }
  } else
    isNight = false;*/
}

void PlayerControl::update(bool currentlyActive) {
  vector<Creature*> addedCreatures;
  vector<WLevel> currentLevels {getCurrentLevel()};
  for (auto c : getControlled())
    if (!currentLevels.contains(c->getLevel()))
      currentLevels.push_back(c->getLevel());
  for (WLevel l : currentLevels)
    for (Creature* c : l->getAllCreatures())
      if (!getCreatures().contains(c) && c->getTribeId() == getTribeId() && canSee(c) && !isEnemy(c)) {
        if (!collective->wasBanished(c) && !c->getBody().isMinionFood() && c->getAttributes().getCanJoinCollective()) {
          addedCreatures.push_back(c);
          collective->addCreature(c, {MinionTrait::FIGHTER});
          for (auto controlled : getControlled())
            if (canControlInTeam(c)
                && c->getPosition().isSameLevel(controlled->getPosition())
                && canControlInTeam(c)
                && !collective->hasTrait(c, MinionTrait::SUMMONED)) {
              addToCurrentTeam(c);
              controlled->privateMessage(PlayerMessage(c->getName().a() + " joins your team.",
                  MessagePriority::HIGH));
              break;
            }
        } else
          if (c->getBody().isMinionFood())
            collective->addCreature(c, {MinionTrait::FARM_ANIMAL, MinionTrait::NO_LIMIT});
      }
  if (!addedCreatures.empty()) {
    collective->addNewCreatureMessage(addedCreatures);
  }
}

WLevel PlayerControl::getCurrentLevel() const {
  if (!currentLevel)
    return getModel()->getTopLevel();
  else
    return currentLevel;
}

bool PlayerControl::isConsideredAttacking(const Creature* c, const Collective* enemy) {
  if (enemy && enemy->getModel() == getModel())
    return canSee(c) && (collective->getTerritory().contains(c->getPosition()) ||
        collective->getTerritory().getStandardExtended().contains(c->getPosition()));
  else
    return canSee(c) && c->getLevel()->getModel() == getModel();
}

const double messageTimeout = 80;

void PlayerControl::updateUnknownLocations() {
  vector<Position> locations;
  for (auto col : getGame()->getCollectives())
    if (!collective->isKnownVillainLocation(col) && !col->isConquered())
      if (auto& pos = col->getTerritory().getCentralPoint())
        locations.push_back(*pos);
  unknownLocations->update(locations);
}

void PlayerControl::considerTransferingLostMinions() {
  if (getGame()->getCurrentModel() == getModel())
    for (auto c : copyOf(getCreatures()))
      if (c->getPosition().getModel() != getModel())
        getGame()->transferCreature(c, getModel());
}

void PlayerControl::tick() {
  PROFILE_BLOCK("PlayerControl::tick");
  updateUnknownLocations();
  considerTransferingLostMinions();
  for (auto& elem : messages)
    elem.setFreshness(max(0.0, elem.getFreshness() - 1.0 / messageTimeout));
  messages = messages.filter([&] (const PlayerMessage& msg) {
      return msg.getFreshness() > 0; });
  considerNightfallMessage();
  if (getGame()->getOptions()->getBoolValue(OptionId::KEEPER_WARNING)) {
    if (checkKeeperDanger()) {
      if (getGame()->getOptions()->getBoolValue(OptionId::KEEPER_WARNING_PAUSE) &&
        !wasPausedForWarning) {
        getView()->stopClock();
        wasPausedForWarning = true;
      }
    } else
      wasPausedForWarning = false;
  }
  if (auto msg = collective->getWarnings().getNextWarning(getModel()->getLocalTime()))
    addMessage(PlayerMessage(*msg, MessagePriority::HIGH));
  for (auto attack : copyOf(ransomAttacks))
    for (const Creature* c : attack.getCreatures())
      if (collective->getTerritory().contains(c->getPosition())) {
        ransomAttacks.removeElement(attack);
        break;
      }
  for (auto attack : copyOf(newAttacks))
    for (const Creature* c : attack.getCreatures())
      if (isConsideredAttacking(c, attack.getAttacker())) {
        addMessage(PlayerMessage("You are under attack by " + attack.getAttackerName() + "!",
            MessagePriority::CRITICAL).setPosition(c->getPosition()));
        getGame()->setCurrentMusic(MusicType::BATTLE);
        newAttacks.removeElement(attack);
        if (auto attacker = attack.getAttacker())
          collective->addKnownVillain(attacker);
        notifiedAttacks.push_back(attack);
        if (attack.getRansom())
          ransomAttacks.push_back(attack);
        break;
      }
  if (notifiedAttacks.empty())
    getGame()->setDefaultMusic();
  auto time = collective->getLocalTime();
  if (getGame()->getOptions()->getBoolValue(OptionId::HINTS) && time > hintFrequency) {
    int numHint = time.getDouble() / hintFrequency.getDouble() - 1;
    if (numHint < hints.size() && !hints[numHint].empty()) {
      addMessage(PlayerMessage(hints[numHint], MessagePriority::HIGH));
      hints[numHint] = "";
    }
  }
}

bool PlayerControl::canSee(const Creature* c) const {
  return canSee(c->getPosition());
}

bool PlayerControl::canSee(Position pos) const {
  return getGame()->getOptions()->getBoolValue(OptionId::SHOW_MAP) || visibilityMap->isVisible(pos);
}

TribeId PlayerControl::getTribeId() const {
  return collective->getTribeId();
}

bool PlayerControl::isEnemy(const Creature* c) const {
  auto& leaders = collective->getLeaders();
  return c->getTribeId() != getTribeId() && !leaders.empty() && leaders[0]->isEnemy(c);
}

void PlayerControl::onConquered(Creature* victim, Creature* killer) {
  if (!victim->isPlayer()) {
    setScrollPos(victim->getPosition().plus(Vec2(0, 5)));
    getView()->updateView(this, false);
  }
  getGame()->gameOver(victim, collective->getKills().getSize(), "enemies",
      collective->getDangerLevel() + collective->getPoints());
  if (!collective->getTerritory().isEmpty())
    for (auto col : getGame()->getCollectives())
      if (col != collective && col->getCreatures().contains(killer) && col->getConfig().xCanEnemyRetire() &&
          isOneOf(col->getVillainType(), VillainType::MAIN, VillainType::LESSER)) {
        getGame()->setExitInfo(GameSaveType::RETIRED_SITE);
        collective->makeConqueredRetired(col); // this call invalidates this
        return;
      }
}

void PlayerControl::onMemberKilled(const Creature* victim, const Creature* killer) {
  if (victim->isPlayer() &&
      (!collective->hasTrait(victim, MinionTrait::LEADER) || collective->getCreatures(MinionTrait::LEADER).size() > 1))
    onControlledKilled(victim);
  visibilityMap->remove(victim);
}

void PlayerControl::onMemberAdded(Creature* c) {
  updateMinionVisibility(c);
  auto team = getControlled();
  if (collective->hasTrait(c, MinionTrait::PRISONER) && !team.empty() &&
      team[0]->getPosition().isSameLevel(c->getPosition()))
    addToCurrentTeam(c);
}

WModel PlayerControl::getModel() const {
  return collective->getModel();
}

WGame PlayerControl::getGame() const {
  return collective->getGame();
}

View* PlayerControl::getView() const {
  return getGame()->getView();
}

void PlayerControl::addAttack(const CollectiveAttack& attack) {
  newAttacks.push_back(attack);
}

void PlayerControl::updateSquareMemory(Position pos) {
  ViewIndex index;
  pos.getViewIndex(index, collective->getLeaders()[0]); // use the leader as a generic viewer
  memory->update(pos, index);
}

void PlayerControl::onConstructed(Position pos, FurnitureType type) {
  addToMemory(pos);
  if (type == FurnitureType("EYEBALL"))
    visibilityMap->updateEyeball(pos);
}

PController PlayerControl::createMinionController(Creature* c) {
  return ::getMinionController(c, memory, this, controlModeMessages, visibilityMap, unknownLocations, tutorial);
}

static void considerAddingKeeperFloor(Position pos) {
  if (NOTNULL(pos.getFurniture(FurnitureLayer::GROUND))->getViewObject()->id() == ViewId("floor"))
    pos.modFurniture(FurnitureLayer::GROUND)->getViewObject()->setId(ViewId("keeper_floor"));
}

void PlayerControl::onClaimedSquare(Position position) {
  considerAddingKeeperFloor(position);
  position.setNeedsRenderAndMemoryUpdate(true);
  updateSquareMemory(position);
}

void PlayerControl::onDestructed(Position pos, FurnitureType type, const DestroyAction& action) {
  if (action.getType() == DestroyAction::Type::DIG) {
    Vec2 visRadius(3, 3);
    for (Position v : pos.getRectangle(Rectangle(-visRadius, visRadius + Vec2(1, 1)))) {
      collective->addKnownTile(v);
      updateSquareMemory(v);
    }
    considerAddingKeeperFloor(pos);
    pos.setNeedsRenderAndMemoryUpdate(true);
  }
}

vector<Vec2> PlayerControl::getVisibleEnemies() const {
  return {};
}

TribeAlignment PlayerControl::getTribeAlignment() const {
  return tribeAlignment;
}

REGISTER_TYPE(ListenerTemplate<PlayerControl>)
