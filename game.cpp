#include "stdafx.h"
#include "game.h"
#include "view.h"
#include "clock.h"
#include "tribe.h"
#include "pantheon.h"
#include "music.h"
#include "player_control.h"
#include "model.h"
#include "creature.h"
#include "spectator.h"
#include "statistics.h"
#include "collective.h"
#include "options.h"
#include "territory.h"
#include "level.h"
#include "highscores.h"
#include "player.h"
#include "item_factory.h"
#include "item.h"
#include "map_memory.h"
#include "creature_attributes.h"
#include "name_generator.h"
#include "campaign.h"

template <class Archive> 
void Game::serialize(Archive& ar, const unsigned int version) { 
  serializeAll(ar, villainsByType, collectives, lastTick, playerControl, playerCollective, won, currentTime);
  serializeAll(ar, worldName, musicType, danglingPortal, statistics, spectator, tribes, gameIdentifier, player);
  serializeAll(ar, gameDisplayName, finishCurrentMusic, models, baseModel, campaign, localTime);
  Deity::serializeAll(ar);
  if (Archive::is_loading::value)
    sunlightInfo.update(currentTime);
}

SERIALIZABLE(Game);
SERIALIZATION_CONSTRUCTOR_IMPL(Game);

static string getNewIdSuffix() {
  vector<char> chars;
  for (char c : Range(128))
    if (isalnum(c))
      chars.push_back(c);
  string ret;
  for (int i : Range(4))
    ret += Random.choose(chars);
  return ret;
}

Game::Game(const string& world, const string& player, Table<PModel>&& m, Vec2 basePos, optional<Campaign> c)
    : worldName(world), models(std::move(m)), baseModel(basePos),
      tribes(Tribe::generateTribes()), musicType(MusicType::PEACEFUL), campaign(c) {
  sunlightInfo.update(currentTime);
  gameIdentifier = player + "_" + worldName + getNewIdSuffix();
  gameDisplayName = player + " of " + worldName;
  for (Vec2 v : models.getBounds())
    if (Model* m = models[v].get()) {
      for (Collective* c : m->getCollectives()) {
        collectives.push_back(c);
        if (auto type = c->getVillainType()) {
          villainsByType[*type].push_back(c);
          if (type == VillainType::PLAYER) {
            playerControl = NOTNULL(dynamic_cast<PlayerControl*>(c->getControl()));
            playerCollective = c;
          }
        }
      }
      m->setGame(this);
      m->updateSunlightMovement();
    }
}

Game::~Game() {}

PGame Game::campaignGame(Table<PModel>&& models, Vec2 basePos, const string& playerName, const Campaign& campaign) {
  PGame game(new Game(campaign.getWorldName(), playerName, std::move(models), basePos, campaign));
  return game;
}

PGame Game::singleMapGame(const string& worldName, const string& playerName, PModel&& model) {
  Table<PModel> t(1, 1);
  t[0][0] = std::move(model);
  return PGame(new Game(worldName, playerName, std::move(t), Vec2(0, 0)));
}

PGame Game::splashScreen(PModel&& model) {
  Table<PModel> t(1, 1);
  t[0][0] = std::move(model);
  PGame game(new Game("", "", std::move(t), Vec2(0, 0)));
  game->spectator.reset(new Spectator(game->models[0][0]->getLevels()[0]));
  return game;
}

bool Game::isTurnBased() {
  return !spectator && (!playerControl || playerControl->isTurnBased());
}

double Game::getGlobalTime() const {
  return currentTime;
}

const vector<Collective*>& Game::getVillains(VillainType type) const {
  static vector<Collective*> empty;
  if (villainsByType.count(type))
    return villainsByType.at(type);
  else
    return empty;
}

Model* Game::getCurrentModel() const {
  if (Creature* c = getPlayer())
    return c->getPosition().getModel();
  else
    return models[baseModel].get();
}

optional<Game::ExitInfo> Game::update(double timeDiff) {
  currentTime += timeDiff;
  Model* currentModel = getCurrentModel();
  localTime[currentModel] += timeDiff;
  while (currentTime > lastTick + 1) {
    lastTick += 1;
    tick(lastTick);
  }
  return updateModel(currentModel, localTime[currentModel]);
}

optional<Game::ExitInfo> Game::updateModel(Model* model, double totalTime) {
  int absoluteTime = view->getTimeMilliAbsolute();
  if (playerControl && absoluteTime - lastUpdate > 20) {
    playerControl->render(view);
    lastUpdate = absoluteTime;
  } else
  if (spectator && absoluteTime - lastUpdate > 20) {
    view->updateView(spectator.get(), false);
    lastUpdate = absoluteTime;
  } 
  do {
    if (spectator)
      while (1) {
        UserInput input = view->getAction();
        if (input.getId() == UserInputId::EXIT)
          return ExitInfo{ExitId::QUIT};
        if (input.getId() == UserInputId::IDLE)
          break;
      }
    if (playerControl && !playerControl->isTurnBased()) {
      while (1) {
        UserInput input = view->getAction();
        if (input.getId() == UserInputId::IDLE)
          break;
        else
          lastUpdate = -10;
        playerControl->processInput(view, input);
        if (exitInfo)
          return exitInfo;
      }
    }
    if (model->getTime() > totalTime)
      return none;
    model->update(totalTime);
    if (exitInfo)
      return exitInfo;
    if (wasTransfered) {
      wasTransfered = false;
      return none;
    }
  } while (1);
}

void Game::tick(double time) {
  auto previous = sunlightInfo.getState();
  sunlightInfo.update(currentTime);
  if (previous != sunlightInfo.getState())
    for (Vec2 v : models.getBounds())
      if (Model* m = models[v].get())
        m->updateSunlightMovement();
  Debug() << "Global time " << time;
  lastTick = time;
  if (playerControl) {
    if (!playerControl->isRetired()) {
      bool conquered = true;
      for (Collective* col : getVillains(VillainType::MAIN))
        conquered &= col->isConquered();
      if (!getVillains(VillainType::MAIN).empty() && conquered && !won) {
        playerControl->onConqueredLand();
        won = true;
      }
    }
  }
  if (musicType == MusicType::PEACEFUL && sunlightInfo.getState() == SunlightState::NIGHT)
    setCurrentMusic(MusicType::NIGHT, true);
  else if (musicType == MusicType::NIGHT && sunlightInfo.getState() == SunlightState::DAY)
    setCurrentMusic(MusicType::PEACEFUL, true);
}

void Game::exitAction() {
  enum Action { SAVE, RETIRE, OPTIONS, ABANDON};
#ifdef RELEASE
  bool canRetire = playerControl && !playerControl->isRetired() && won;
#else
  bool canRetire = playerControl && !playerControl->isRetired();
#endif
  vector<ListElem> elems { "Save the game",
    {"Retire", canRetire ? ListElem::NORMAL : ListElem::INACTIVE} , "Change options", "Abandon the game" };
  auto ind = view->chooseFromList("Would you like to:", elems);
  if (!ind)
    return;
  switch (Action(*ind)) {
    case RETIRE:
      if (view->yesOrNoPrompt("Retire your dungeon and share it online?")) {
        exitInfo = ExitInfo(ExitId::SAVE, SaveType::RETIRED_KEEPER);
        return;
      }
      break;
    case SAVE:
      if (!playerControl || playerControl->isRetired()) {
        exitInfo = ExitInfo(ExitId::SAVE, SaveType::ADVENTURER);
        return;
      } else {
        exitInfo = ExitInfo(ExitId::SAVE, SaveType::KEEPER);
        return;
      }
    case ABANDON:
      if (view->yesOrNoPrompt("Are you sure you want to abandon your game?")) {
        exitInfo = ExitInfo(ExitId::QUIT);
        return;
      }
      break;
    case OPTIONS: options->handle(view, OptionSet::GENERAL); break;
    default: break;
  }
}

void Game::transferCreature(Creature* c, Model* to) {
  Model* from = c->getLevel()->getModel();
  if (from != to)
    to->transferCreature(from->extractCreature(c));
}

void Game::transferAction(const vector<Creature*>& creatures) {
  if (!campaign)
    return;
  if (auto dest = view->chooseSite("Choose destination site:", *campaign)) {
    CHECK(models[*dest]);
    for (Creature* c : creatures)
      transferCreature(c, models[*dest].get());
    wasTransfered = true;
  }
}

Statistics& Game::getStatistics() {
  return *statistics;
}

const Statistics& Game::getStatistics() const {
  return *statistics;
}

Tribe* Game::getTribe(TribeId id) const {
  return tribes[id].get();
}

Collective* Game::getPlayerCollective() const {
  return playerCollective;
}

MusicType Game::getCurrentMusic() const {
  return musicType;
}

void Game::setCurrentMusic(MusicType type, bool now) {
  musicType = type;
  finishCurrentMusic = now;
}

bool Game::changeMusicNow() const {
  return finishCurrentMusic;
}

const SunlightInfo& Game::getSunlightInfo() const {
  return sunlightInfo;
}


void Game::onTechBookRead(Technology* tech) {
  if (playerControl)
    playerControl->onTechBookRead(tech);
}

void Game::onAlarm(Position pos) {
  Model* model = pos.getModel();
  for (auto& col : model->getCollectives())
    if (col->getTerritory().contains(pos))
      col->onAlarm(pos);
  for (Level* l : model->getLevels())
    if (const Creature* c = l->getPlayer()) {
      if (pos == c->getPosition())
        c->playerMessage("An alarm sounds near you.");
      else if (pos.isSameLevel(c->getPosition()))
        c->playerMessage("An alarm sounds in the " + 
            getCardinalName(c->getPosition().getDir(pos).getBearing().getCardinalDir()));
    }
}

void Game::landHeroPlayer() {
  auto handicap = view->getNumber("Choose handicap (your adventurer's strength and dexterity increase)", 0, 20, 5);
  PCreature player = makeAdventurer(handicap.get_value_or(0));
  string advName = options->getStringValue(OptionId::ADVENTURER_NAME);
  if (!advName.empty())
    player->setFirstName(advName);
  Level* target = models[0][0]->getLevels()[0];
  CHECK(target->landCreature(target->getAllPositions(), std::move(player))) << "No place to spawn player";
}

string Game::getGameDisplayName() const {
  return gameDisplayName;
}

string Game::getGameIdentifier() const {
  return gameIdentifier;
}

void Game::onKilledLeader(const Collective* victim, const Creature* leader) {
  if (models.getHeight() == 1 && models.getWidth() == 1 && playerControl && playerControl->isRetired() &&
      playerCollective == victim) {
    if (Creature* c = getPlayer())
      killedKeeper(*c->getFirstName(), leader->getNameAndTitle(), worldName, c->getKills(), c->getPoints());
  }
}

void Game::onTorture(const Creature* who, const Creature* torturer) {
  for (Collective* col : getCollectives())
    if (contains(col->getCreatures(), torturer))
      col->onTorture(who, torturer);
}

void Game::onSurrender(Creature* who, const Creature* to) {
  for (auto& col : collectives)
    if (contains(col->getCreatures(), to))
      col->onSurrender(who);
}

void Game::onAttack(Creature* victim, Creature* attacker) {
  victim->getTribe()->onMemberAttacked(victim, attacker);
}

void Game::onTrapTrigger(Position pos) {
  for (auto& col : collectives)
    if (col->getTerritory().contains(pos))
      col->onTrapTrigger(pos);
}

void Game::onTrapDisarm(Position pos, const Creature* who) {
  for (auto& col : collectives)
    if (col->getTerritory().contains(pos))
      col->onTrapDisarm(who, pos);
}

void Game::onSquareDestroyed(Position pos) {
  for (auto& col : collectives)
    if (col->getTerritory().contains(pos))
      col->onSquareDestroyed(pos);
}

void Game::onEquip(const Creature* c, const Item* it) {
  for (auto& col : collectives)
    if (contains(col->getCreatures(), c))
      col->onEquip(c, it);
}

View* Game::getView() const {
  return view;
}

void Game::conquered(const string& title, vector<const Creature*> kills, int points) {
  string text= "You have conquered this land. You killed " + toString(kills.size()) +
      " innocent beings and scored " + toString(points) +
      " points. Thank you for playing KeeperRL alpha.\n \n";
  for (string stat : statistics->getText())
    text += stat + "\n";
  view->presentText("Victory", text);
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = title;
        c.gameResult = "achieved world domination";
        c.gameWon = true;
        c.turns = getGlobalTime();
        c.gameType = Highscores::Score::KEEPER;
  );
  highscores->add(score);
  highscores->present(view, score);
}

void Game::killedKeeper(const string& title, const string& keeper, const string& land,
    vector<const Creature*> kills, int points) {
  string text= "You have freed this land from the bloody reign of " + keeper + 
      ". You killed " + toString(kills.size()) +
      " enemies and scored " + toString(points) +
      " points. Thank you for playing KeeperRL alpha.\n \n";
  for (string stat : statistics->getText())
    text += stat + "\n";
  view->presentText("Victory", text);
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = title;
        c.gameResult = "freed his land from " + keeper;
        c.gameWon = true;
        c.turns = getGlobalTime();
        c.gameType = Highscores::Score::ADVENTURER;
  );
  highscores->add(score);
  highscores->present(view, score);
}

bool Game::isGameOver() const {
  return !!exitInfo;
}

void Game::gameOver(const Creature* creature, int numKills, const string& enemiesString, int points) {
  string text = "And so dies " + creature->getNameAndTitle();
  if (auto reason = creature->getDeathReason()) {
    text += ", " + *reason;
  }
  text += ". He killed " + toString(numKills) 
      + " " + enemiesString + " and scored " + toString(points) + " points.\n \n";
  for (string stat : statistics->getText())
    text += stat + "\n";
  view->presentText("Game over", text);
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = *creature->getFirstName();
        c.gameResult = creature->getDeathReason().get_value_or("");
        c.gameWon = false;
        c.turns = getGlobalTime();
        c.gameType = (!playerControl || playerControl->isRetired()) ? 
            Highscores::Score::ADVENTURER : Highscores::Score::KEEPER;
  );
  highscores->add(score);
  highscores->present(view, score);
  exitInfo = ExitInfo(ExitId::QUIT);
}

Options* Game::getOptions() {
  return options;
}

void Game::setOptions(Options* o) {
  options = o;
}

void Game::setHighscores(Highscores* h) {
  highscores = h;
}

const string& Game::getWorldName() const {
  return worldName;
}

optional<Position> Game::getDanglingPortal() {
  return danglingPortal;
}

void Game::setDanglingPortal(Position p) {
  danglingPortal = p;
}

void Game::resetDanglingPortal() {
  danglingPortal.reset();
}

const vector<Collective*>& Game::getCollectives() const {
  return collectives;
}

PCreature Game::makeAdventurer(int handicap) {
  MapMemory* levelMemory = new MapMemory();
  PCreature player = CreatureFactory::addInventory(
      PCreature(new Creature(TribeId::ADVENTURER,
      CATTR(
          c.viewId = ViewId::PLAYER;
          c.attr[AttrType::SPEED] = 100;
          c.weight = 90;
          c.size = CreatureSize::LARGE;
          c.attr[AttrType::STRENGTH] = 13 + handicap;
          c.attr[AttrType::DEXTERITY] = 15 + handicap;
          c.barehandedDamage = 5;
          c.humanoid = true;
          c.name = "Adventurer";
          c.firstName = NameGenerator::get(NameGeneratorId::FIRST)->getNext();
          c.skills.insert(SkillId::AMBUSH);), Player::getFactory(levelMemory))), {
      ItemId::FIRST_AID_KIT,
      ItemId::SWORD,
      ItemId::KNIFE,
      ItemId::LEATHER_GLOVES,
      ItemId::LEATHER_ARMOR,
      ItemId::LEATHER_HELM});
  for (int i : Range(Random.get(70, 131)))
    player->take(ItemFactory::fromId(ItemId::GOLD_PIECE));
  return player;
}

void Game::setView(View* v) {
  view = v;
}

void Game::setPlayer(Creature* c) {
  player = c;
}

Creature* Game::getPlayer() const {
  if (player && !player->isDead())
    return player;
  else
    return nullptr;
}

void Game::cancelPlayer(Creature* c) {
  CHECK(c == player);
  player = nullptr;
}
