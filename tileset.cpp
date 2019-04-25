#include "tileset.h"
#include "view_id.h"
#include "tile.h"
#include "view_object.h"
#include "game_config.h"
#include "tile_info.h"

void TileSet::addTile(ViewId id, Tile tile) {
  tiles.insert(make_pair(id, std::move(tile)));
}

void TileSet::addSymbol(ViewId id, Tile tile) {
  symbols.insert(make_pair(std::move(id), std::move(tile)));
}

Color TileSet::getColor(const ViewObject& object) const {
  if (object.hasModifier(ViewObject::Modifier::INVISIBLE))
    return Color::DARK_GRAY;
  if (object.hasModifier(ViewObject::Modifier::HIDDEN))
    return Color::LIGHT_GRAY;
  Color color = symbols.at(object.id()).color;
  if (object.hasModifier(ViewObject::Modifier::PLANNED))
    return Color(color.r / 2, color.g / 2, color.b / 2);
  return color;
}

const vector<TileCoord>& TileSet::getTileCoord(const string& s) const {
  return tileCoords.at(s);
}

const vector<TileCoord>& TileSet::byName(const string& s) {
  return tileCoords.at(s);
}

Tile TileSet::sprite(const string& s) {
  return Tile::byCoord(byName(s));
}

Tile TileSet::empty() {
  return sprite("empty");
}

Tile TileSet::getRoadTile(const string& prefix) {
  return sprite(prefix)
    .addConnection({Dir::E, Dir::W}, byName(prefix + "ew"))
    .addConnection({Dir::W}, byName(prefix + "w"))
    .addConnection({Dir::E}, byName(prefix + "e"))
    .addConnection({Dir::S}, byName(prefix + "s"))
    .addConnection({Dir::N, Dir::S}, byName(prefix + "ns"))
    .addConnection({Dir::N}, byName(prefix + "n"))
    .addConnection({Dir::S, Dir::E}, byName(prefix + "se"))
    .addConnection({Dir::S, Dir::W}, byName(prefix + "sw"))
    .addConnection({Dir::N, Dir::E}, byName(prefix + "ne"))
    .addConnection({Dir::N, Dir::W}, byName(prefix + "nw"))
    .addConnection({Dir::N, Dir::E, Dir::S, Dir::W}, byName(prefix + "nesw"))
    .addConnection({Dir::E, Dir::S, Dir::W}, byName(prefix + "esw"))
    .addConnection({Dir::N, Dir::S, Dir::W}, byName(prefix + "nsw"))
    .addConnection({Dir::N, Dir::E, Dir::S}, byName(prefix + "nes"))
    .addConnection({Dir::N, Dir::E, Dir::W}, byName(prefix + "new"));
}

Tile TileSet::getWallTile(const string& prefix) {
  return sprite(prefix)
    .addConnection({Dir::E}, byName(prefix + "e"))
    .addConnection({Dir::E, Dir::W}, byName(prefix + "ew"))
    .addConnection({Dir::W}, byName(prefix + "w"))
    .addConnection({Dir::S}, byName(prefix + "s"))
    .addConnection({Dir::N, Dir::S}, byName(prefix + "ns"))
    .addConnection({Dir::N}, byName(prefix + "n"))
    .addConnection({Dir::E, Dir::S}, byName(prefix + "es"))
    .addConnection({Dir::S, Dir::W}, byName(prefix + "sw"))
    .addConnection({Dir::N, Dir::E}, byName(prefix + "ne"))
    .addConnection({Dir::N, Dir::W}, byName(prefix + "nw"))
    .addConnection({Dir::N, Dir::E, Dir::S, Dir::W}, byName(prefix + "nesw"))
    .addConnection({Dir::E, Dir::S, Dir::W}, byName(prefix + "esw"))
    .addConnection({Dir::N, Dir::S, Dir::W}, byName(prefix + "nsw"))
    .addConnection({Dir::N, Dir::E, Dir::S}, byName(prefix + "nes"))
    .addConnection({Dir::N, Dir::E, Dir::W}, byName(prefix + "new"))
    .setConnectionId(ViewId("wall"));
}

Tile TileSet::getMountainTile(const string& spriteName, const string& prefix) {
  return sprite(spriteName)
    .addCorner({Dir::S, Dir::W}, {Dir::W}, byName(prefix + "sw_w"))
    .addCorner({Dir::N, Dir::W}, {Dir::W}, byName(prefix + "nw_w"))
    .addCorner({Dir::S, Dir::E}, {Dir::E}, byName(prefix + "se_e"))
    .addCorner({Dir::N, Dir::E}, {Dir::E}, byName(prefix + "ne_e"))
    .addCorner({Dir::N, Dir::W}, {Dir::N}, byName(prefix + "nw_n"))
    .addCorner({Dir::N, Dir::E}, {Dir::N}, byName(prefix + "ne_n"))
    .addCorner({Dir::S, Dir::W}, {Dir::S}, byName(prefix + "sw_s"))
    .addCorner({Dir::S, Dir::E}, {Dir::S}, byName(prefix + "se_s"))
    .addCorner({Dir::N, Dir::W}, {}, byName(prefix + "nw"))
    .addCorner({Dir::N, Dir::E}, {}, byName(prefix + "ne"))
    .addCorner({Dir::S, Dir::W}, {}, byName(prefix + "sw"))
    .addCorner({Dir::S, Dir::E}, {}, byName(prefix + "se"))
    .addCorner({Dir::S, Dir::E, Dir::SE}, {Dir::S, Dir::E}, byName(prefix + "sese_se"))
    .addCorner({Dir::S, Dir::W, Dir::SW}, {Dir::S, Dir::W}, byName(prefix + "swsw_sw"))
    .addCorner({Dir::N, Dir::E, Dir::NE}, {Dir::N, Dir::E}, byName(prefix + "nene_ne"))
    .addCorner({Dir::N, Dir::W, Dir::NW}, {Dir::N, Dir::W}, byName(prefix + "nwnw_nw"))
    .setConnectionId(ViewId("mountain"));
}

Tile TileSet::getWaterTile(const string& background, const string& prefix) {
  return empty().addBackground(byName(background))
    .addConnection({Dir::N, Dir::E, Dir::S, Dir::W}, byName("empty"))
    .addConnection({Dir::E, Dir::S, Dir::W}, byName(prefix + "esw"))
    .addConnection({Dir::N, Dir::E, Dir::W}, byName(prefix + "new"))
    .addConnection({Dir::N, Dir::S, Dir::W}, byName(prefix + "nsw"))
    .addConnection({Dir::N, Dir::E, Dir::S}, byName(prefix + "nes"))
    .addConnection({Dir::N, Dir::E}, byName(prefix + "ne"))
    .addConnection({Dir::E, Dir::S}, byName(prefix + "es"))
    .addConnection({Dir::S, Dir::W}, byName(prefix + "sw"))
    .addConnection({Dir::N, Dir::W}, byName(prefix + "nw"))
    .addConnection({Dir::S}, byName(prefix + "s"))
    .addConnection({Dir::N}, byName(prefix + "n"))
    .addConnection({Dir::W}, byName(prefix + "w"))
    .addConnection({Dir::E}, byName(prefix + "e"))
    .addConnection({Dir::N, Dir::S}, byName(prefix + "ns"))
    .addConnection({Dir::E, Dir::W}, byName(prefix + "ew"))
    .addConnection({}, byName(prefix + "all"));
}

Tile TileSet::getExtraBorderTile(const string& prefix) {
  return sprite(prefix)
    .addExtraBorder({Dir::W, Dir::N}, byName(prefix + "wn"))
    .addExtraBorder({Dir::E, Dir::N}, byName(prefix + "en"))
    .addExtraBorder({Dir::E, Dir::S}, byName(prefix + "es"))
    .addExtraBorder({Dir::W, Dir::S}, byName(prefix + "ws"))
    .addExtraBorder({Dir::W, Dir::N, Dir::E}, byName(prefix + "wne"))
    .addExtraBorder({Dir::S, Dir::N, Dir::E}, byName(prefix + "sne"))
    .addExtraBorder({Dir::S, Dir::W, Dir::E}, byName(prefix + "swe"))
    .addExtraBorder({Dir::S, Dir::W, Dir::N}, byName(prefix + "swn"))
    .addExtraBorder({Dir::S, Dir::W, Dir::N, Dir::E}, byName(prefix + "swne"))
    .addExtraBorder({Dir::N}, byName(prefix + "n"))
    .addExtraBorder({Dir::E}, byName(prefix + "e"))
    .addExtraBorder({Dir::S}, byName(prefix + "s"))
    .addExtraBorder({Dir::W}, byName(prefix + "w"));
}

void TileSet::loadModdedTiles(const GameConfig* gameConfig, bool useTiles) {
  vector<TileInfo> tiles;
  while (1) {
    auto error = gameConfig->readObject(tiles, GameConfigId::TILES);
    if (error)
      USER_INFO << *error;
    else
      break;
  }
  for (auto& tile : tiles) {
    if (useTiles) {
      auto spriteName = tile.sprite.value_or(tile.viewId.data());
      USER_CHECK(tileCoords.count(spriteName)) << "Sprite file not found: " << spriteName;
      auto t = [&] {
        if (tile.wallConnections)
          return getWallTile(spriteName);
        if (tile.roadConnections)
          return getRoadTile(spriteName);
        if (tile.mountainSides)
          return getMountainTile(spriteName, *tile.mountainSides);
        if (tile.waterSides)
          return getWaterTile(spriteName, *tile.waterSides);
        if (!tile.extraBorders.empty()) {
          auto ret = getExtraBorderTile(spriteName);
          for (auto& elem : tile.extraBorders)
            ret.addExtraBorderId(elem);
          return ret;
        }
        return sprite(spriteName);
      }();
      if (tile.spriteColor)
        t.setColor(tile.spriteColor->value);
      if (tile.roundShadow)
        t.setRoundShadow();
      if (tile.wallShadow)
        t.setWallShadow();
      if (tile.background)
        t.addBackground(byName(*tile.background));
      if (tile.moveUp)
        t.setMoveUp();
      addTile(tile.viewId, std::move(t));
    }
    addSymbol(tile.viewId, symbol(tile.symbol, tile.color.value, tile.isSymbolFont));
  }
}

void TileSet::loadUnicode() {
  addSymbol(ViewId("bridge"), symbol(u8"_", Color::BROWN));
  addSymbol(ViewId("accept_immigrant"), symbol(u8"✓", Color::GREEN, true));
  addSymbol(ViewId("reject_immigrant"), symbol(u8"✘", Color::RED, true));
  addSymbol(ViewId("fog_of_war_corner"), symbol(u8" ", Color::WHITE));
  addSymbol(ViewId("tutorial_entrance"), symbol(u8" ", Color::LIGHT_GREEN));
}

void TileSet::loadTiles() {
  addTile(ViewId("bridge"), sprite("bridge").addOption(Dir::S, byName("bridge2")));
  addTile(ViewId("accept_immigrant"), symbol(u8"✓", Color::GREEN, true));
  addTile(ViewId("reject_immigrant"), symbol(u8"✘", Color::RED, true));
  addTile(ViewId("fog_of_war_corner"), sprite("fogofwarall")
      .addConnection({Dir::NE}, byName("fogofwarcornne"))
      .addConnection({Dir::NW}, byName("fogofwarcornnw"))
      .addConnection({Dir::SE}, byName("fogofwarcornse"))
      .addConnection({Dir::SW}, byName("fogofwarcornsw")));
#ifndef RELEASE
  addTile(ViewId("tutorial_entrance"), symbol(u8"?", Color::YELLOW));
#else
  addTile(ViewId("tutorial_entrance"), sprite("empty"));
#endif
}

Tile TileSet::symbol(const string& s, Color id, bool symbol) {
  return Tile::fromString(s, id, symbol);
}

TileSet::TileSet(const DirectoryPath& defaultDir) : defaultDir(defaultDir) {
}

void TileSet::reload(const GameConfig* config, bool useTiles) {
  tiles.clear();
  textures.clear();
  symbols.clear();
  tileCoords.clear();
  auto reloadDir = [&] (const DirectoryPath& path) {
    loadTilesFromDir(path.subdirectory("orig16"), Vec2(16, 16));
    loadTilesFromDir(path.subdirectory("orig24"), Vec2(24, 24));
    loadTilesFromDir(path.subdirectory("orig30"), Vec2(30, 30));
  };
  reloadDir(defaultDir);
  reloadDir(config->getPath());
  loadUnicode();
  if (useTiles)
    loadTiles();
  loadModdedTiles(config, useTiles);
}

const Tile& TileSet::getTile(ViewId id, bool sprite) const {
  if (sprite && tiles.count(id))
    return tiles.at(id);
  else
    return symbols.at(id);
}

constexpr int textureWidth = 720;

void TileSet::loadTilesFromDir(const DirectoryPath& path, Vec2 size) {
  if (!path.exists())
    return;
  const static string imageSuf = ".png";
  auto files = path.getFiles().filter([](const FilePath& f) { return f.hasSuffix(imageSuf);});
  int rowLength = textureWidth / size.x;
  SDL::SDL_Surface* image = Texture::createSurface(textureWidth, textureWidth);
  SDL::SDL_SetSurfaceBlendMode(image, SDL::SDL_BLENDMODE_NONE);
  CHECK(image) << SDL::SDL_GetError();
  int frameCount = 0;
  vector<pair<string, Vec2>> addedPositions;
  for (int i : All(files)) {
    SDL::SDL_Surface* im = SDL::IMG_Load(files[i].getPath());
    SDL::SDL_SetSurfaceBlendMode(im, SDL::SDL_BLENDMODE_NONE);
    CHECK(im) << files[i] << ": "<< SDL::IMG_GetError();
    USER_CHECK((im->w % size.x == 0) && im->h == size.y) << files[i] << " has wrong size " << im->w << " " << im->h;
    string fileName = files[i].getFileName();
    string spriteName = fileName.substr(0, fileName.size() - imageSuf.size());
    if (tileCoords.count(spriteName))
      tileCoords.erase(spriteName);
    for (int frame : Range(im->w / size.x)) {
      SDL::SDL_Rect dest;
      int posX = frameCount % rowLength;
      int posY = frameCount / rowLength;
      dest.x = size.x * posX;
      dest.y = size.y * posY;
      CHECK(dest.x < textureWidth && dest.y < textureWidth);
      SDL::SDL_Rect src;
      src.x = frame * size.x;
      src.y = 0;
      src.w = size.x;
      src.h = size.y;
      SDL_BlitSurface(im, &src, image, &dest);
      addedPositions.emplace_back(spriteName, Vec2(posX, posY));
      INFO << "Loading tile sprite " << fileName << " at " << posX << "," << posY;
      ++frameCount;
    }
    SDL::SDL_FreeSurface(im);
  }
  textures.push_back(unique<Texture>(image));
  for (auto& pos : addedPositions)
    tileCoords[pos.first].push_back({size, pos.second, textures.back().get()});
  SDL::SDL_FreeSurface(image);
}