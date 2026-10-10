#if defined(CROSSINK_ENABLE_POKEMON)

#include "PokemonSleepScreen.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <PokemonSleepRoom.h>
#include <PokemonSpecies.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "components/pokemon/PokemonArtPath.h"
#include "fontIds.h"
#include "pokemon/PokemonService.h"

namespace pokemon {
namespace {

constexpr int MAX_SPRITE_W = 160;
constexpr int MAX_SPRITE_H = 120;
constexpr int MAX_PORTRAIT = 320;

const char* typeLabel(const PokemonType type) {
  switch (type) {
    case PokemonType::Normal:
      return tr(STR_POKEMON_TYPE_NORMAL);
    case PokemonType::Fire:
      return tr(STR_POKEMON_TYPE_FIRE);
    case PokemonType::Water:
      return tr(STR_POKEMON_TYPE_WATER);
    case PokemonType::Electric:
      return tr(STR_POKEMON_TYPE_ELECTRIC);
    case PokemonType::Grass:
      return tr(STR_POKEMON_TYPE_GRASS);
    case PokemonType::Ice:
      return tr(STR_POKEMON_TYPE_ICE);
    case PokemonType::Fighting:
      return tr(STR_POKEMON_TYPE_FIGHTING);
    case PokemonType::Poison:
      return tr(STR_POKEMON_TYPE_POISON);
    case PokemonType::Ground:
      return tr(STR_POKEMON_TYPE_GROUND);
    case PokemonType::Flying:
      return tr(STR_POKEMON_TYPE_FLYING);
    case PokemonType::Psychic:
      return tr(STR_POKEMON_TYPE_PSYCHIC);
    case PokemonType::Bug:
      return tr(STR_POKEMON_TYPE_BUG);
    case PokemonType::Rock:
      return tr(STR_POKEMON_TYPE_ROCK);
    case PokemonType::Ghost:
      return tr(STR_POKEMON_TYPE_GHOST);
    case PokemonType::Dragon:
      return tr(STR_POKEMON_TYPE_DRAGON);
    case PokemonType::Dark:
      return tr(STR_POKEMON_TYPE_DARK);
    case PokemonType::Steel:
      return tr(STR_POKEMON_TYPE_STEEL);
    case PokemonType::Fairy:
      return tr(STR_POKEMON_TYPE_FAIRY);
    default:
      return "";
  }
}

const char* tierLabel(const uint8_t tier) {
  switch (tier) {
    case 1:
      return tr(STR_POKEMON_ROOM_TIER_1);
    case 2:
      return tr(STR_POKEMON_ROOM_TIER_2);
    case 3:
      return tr(STR_POKEMON_ROOM_TIER_3);
    case 4:
      return tr(STR_POKEMON_ROOM_TIER_4);
    case 5:
      return tr(STR_POKEMON_ROOM_TIER_5);
    case 6:
      return tr(STR_POKEMON_ROOM_TIER_6);
    default:
      return tr(STR_POKEMON_ROOM_TIER_7);
  }
}

// Loads the hero sprite as 1-bit ink plus a background mask. Returns false if
// the art pack is missing; the room is still drawn, just without the sprite.
bool loadSprite(const uint16_t speciesId, int& width, int& height, std::unique_ptr<uint8_t[]>& ink,
                std::unique_ptr<uint8_t[]>& opaque) {
  char path[64]{};
  if (pokemonSpeciesArtPath(speciesId, true, path, sizeof(path)) == nullptr) return false;
  FsFile file;
  if (!Storage.openFileForRead("PKSLP", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      bitmap.getWidth() > MAX_SPRITE_W || bitmap.getHeight() > MAX_SPRITE_H) {
    file.close();
    return false;
  }
  width = bitmap.getWidth();
  height = bitmap.getHeight();
  const int packed = (width + 7) / 8;
  const size_t bytes = static_cast<size_t>(packed) * height;
  ink.reset(new (std::nothrow) uint8_t[bytes]);
  opaque.reset(new (std::nothrow) uint8_t[bytes]);
  std::unique_ptr<uint8_t[]> outRow(new (std::nothrow) uint8_t[(width + 3) / 4 + 1]);
  std::unique_ptr<uint8_t[]> rowBuffer(new (std::nothrow) uint8_t[bitmap.getRowBytes()]);
  if (!ink || !opaque || !outRow || !rowBuffer) {
    file.close();
    return false;
  }
  std::memset(ink.get(), 0, bytes);
  for (int i = 0; i < height; ++i) {
    if (bitmap.readNextRow(outRow.get(), rowBuffer.get()) != BmpReaderError::Ok) {
      file.close();
      return false;
    }
    const int y = bitmap.isTopDown() ? i : height - 1 - i;
    for (int x = 0; x < width; ++x) {
      const uint8_t val = (outRow[x / 4] >> (6 - (x % 4) * 2)) & 0x3;
      if (val < 2) ink[y * packed + x / 8] |= static_cast<uint8_t>(0x80 >> (x & 7));
    }
  }
  file.close();
  sleepRoomSpriteMask(ink.get(), width, height, opaque.get());
  return true;
}

// Loads the art pack's grayscale sleep portrait (sleep/NNN.bmp: a 4-bit
// indexed BMP, palette 0..14 grays, 15 transparent; see
// scripts/generate_pokemon_sleep_art.py). Returns false when the art pack
// predates these portraits; the caller then falls back to the hero sprite.
bool loadPortrait(const uint16_t speciesId, int& width, int& height, std::unique_ptr<uint8_t[]>& gray4) {
  char path[64]{};
  if (pokemonSleepArtPath(speciesId, path, sizeof(path)) == nullptr) return false;
  FsFile file;
  if (!Storage.openFileForRead("PKSLP", path, file)) return false;
  uint8_t header[54];
  const auto le32 = [&header](int o) {
    return static_cast<int32_t>(header[o] | header[o + 1] << 8 | header[o + 2] << 16 |
                                static_cast<uint32_t>(header[o + 3]) << 24);
  };
  bool ok = file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && header[0] == 'B' &&
            header[1] == 'M' && (header[28] | header[29] << 8) == 4 && le32(30) == 0;
  const int32_t offset = ok ? le32(10) : 0;
  width = ok ? le32(18) : 0;
  const int32_t rawHeight = ok ? le32(22) : 0;
  height = rawHeight < 0 ? -rawHeight : rawHeight;
  ok = ok && width > 0 && height > 0 && width <= MAX_PORTRAIT && height <= MAX_PORTRAIT;
  const int rowBytes = (width + 1) / 2;
  const int stride = ((width * 4 + 31) / 32) * 4;
  if (ok) gray4.reset(new (std::nothrow) uint8_t[static_cast<size_t>(rowBytes) * height]);
  std::unique_ptr<uint8_t[]> row(ok ? new (std::nothrow) uint8_t[stride] : nullptr);
  ok = ok && gray4 && row && file.seek(static_cast<uint32_t>(offset));
  for (int i = 0; ok && i < height; ++i) {
    ok = file.read(row.get(), stride) == stride;
    const int y = rawHeight < 0 ? i : height - 1 - i;
    if (ok) std::memcpy(gray4.get() + static_cast<size_t>(y) * rowBytes, row.get(), rowBytes);
  }
  file.close();
  if (!ok) gray4.reset();
  return ok;
}

// A boxed type label centred on cx; returns its width.
int typeChipWidth(const GfxRenderer& r, const char* label) {
  return r.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::BOLD) + 16;
}
void drawTypeChip(const GfxRenderer& r, const int x, const int y, const char* label) {
  const int w = typeChipWidth(r, label);
  const int h = r.getLineHeight(SMALL_FONT_ID) + 6;
  r.drawRect(x, y, w, h, 2, true);
  r.drawText(SMALL_FONT_ID, x + 8, y + 3, label, true, EpdFontFamily::BOLD);
}

// Labels go into the frame buffer; writePokemonSleepImage() then burns every
// black frame-buffer pixel into the image as pure black.
void drawLabels(const GfxRenderer& r, const PokemonRecord& record, const SpeciesData& species, const uint8_t level,
                const uint8_t tier, const SleepRoomRenderer& room, const bool fullScreen) {
  const int screenW = r.getScreenWidth();
  char levelText[8];
  std::snprintf(levelText, sizeof(levelText), "%u", static_cast<unsigned>(level));
  // Name: nickname when set, else the species name.
  const char* name = record.nickname[0] != '\0' ? record.nickname.data() : species.name;
  int y;
  if (fullScreen) {
    // Everything on the panel along the bottom edge: "NAME  Lv. 58" first.
    y = room.panelTop() + 14;
    char levelLabel[16];
    std::snprintf(levelLabel, sizeof(levelLabel), "Lv. %u", static_cast<unsigned>(level));
    const int levelW = r.getTextWidth(UI_10_FONT_ID, levelLabel, EpdFontFamily::BOLD);
    const std::string fitted = r.truncatedText(UI_12_FONT_ID, name, screenW - 80 - levelW, EpdFontFamily::BOLD);
    const int nameW = r.getTextWidth(UI_12_FONT_ID, fitted.c_str(), EpdFontFamily::BOLD);
    const int x = (screenW - nameW - 14 - levelW) / 2;
    r.drawText(UI_12_FONT_ID, x, y, fitted.c_str(), true, EpdFontFamily::BOLD);
    r.drawText(UI_10_FONT_ID, x + nameW + 14,
               y + r.getFontAscenderSize(UI_12_FONT_ID) - r.getFontAscenderSize(UI_10_FONT_ID), levelLabel, true,
               EpdFontFamily::BOLD);
    y += r.getLineHeight(UI_12_FONT_ID) + 6;
  } else {
    // Level inside the ball's button.
    const int bx = room.buttonCenterX(), by = room.buttonCenterY();
    const int lvH = r.getLineHeight(SMALL_FONT_ID);
    const int numH = r.getLineHeight(UI_10_FONT_ID);
    const int stackTop = by - (lvH + numH - 6) / 2;
    r.drawText(SMALL_FONT_ID, bx - r.getTextWidth(SMALL_FONT_ID, "Lv") / 2, stackTop, "Lv");
    r.drawText(UI_10_FONT_ID, bx - r.getTextWidth(UI_10_FONT_ID, levelText, EpdFontFamily::BOLD) / 2,
               stackTop + lvH - 6, levelText, true, EpdFontFamily::BOLD);
    y = room.sceneHeight() + 10;
    const std::string fitted = r.truncatedText(UI_12_FONT_ID, name, screenW - 40, EpdFontFamily::BOLD);
    r.drawCenteredText(UI_12_FONT_ID, y, fitted.c_str(), true, EpdFontFamily::BOLD);
    y += r.getLineHeight(UI_12_FONT_ID) + 10;
  }

  // Type chips.
  const char* primary = typeLabel(species.primaryType);
  const bool dual = species.secondaryType != PokemonType::None && species.secondaryType != species.primaryType;
  const char* secondary = dual ? typeLabel(species.secondaryType) : nullptr;
  const int plusW = r.getTextWidth(SMALL_FONT_ID, "+", EpdFontFamily::BOLD);
  const int chipsW = typeChipWidth(r, primary) + (dual ? 16 + plusW + typeChipWidth(r, secondary) : 0);
  int x = (screenW - chipsW) / 2;
  drawTypeChip(r, x, y, primary);
  if (dual) {
    x += typeChipWidth(r, primary) + 8;
    r.drawText(SMALL_FONT_ID, x, y + 3, "+", true, EpdFontFamily::BOLD);
    x += plusW + 8;
    drawTypeChip(r, x, y, secondary);
  }
  y += r.getLineHeight(SMALL_FONT_ID) + (fullScreen ? 12 : 18);

  r.drawCenteredText(UI_10_FONT_ID, y, tr(STR_POKEMON_SLEEP_RESTING));
  y += r.getLineHeight(UI_10_FONT_ID) + (fullScreen ? 10 : 16);

  // Room tier: name, one box per tier, next unlock.
  const char* tierName = tierLabel(tier);
  char next[48];
  if (tier < SLEEP_ROOM_TIER_COUNT) {
    std::snprintf(next, sizeof(next), tr(STR_POKEMON_ROOM_NEXT),
                  static_cast<unsigned>(sleepRoomTierFirstLevel(static_cast<uint8_t>(tier + 1))));
  } else {
    std::snprintf(next, sizeof(next), "%s", tr(STR_POKEMON_ROOM_MAX));
  }
  constexpr int box = 11, gap = 3;
  const int boxesW = SLEEP_ROOM_TIER_COUNT * box + (SLEEP_ROOM_TIER_COUNT - 1) * gap;
  const int nameW = r.getTextWidth(SMALL_FONT_ID, tierName, EpdFontFamily::BOLD);
  const int nextW = r.getTextWidth(SMALL_FONT_ID, next);
  const int rowW = nameW + 10 + boxesW + 10 + nextW;
  x = (screenW - rowW) / 2;
  const int textH = r.getLineHeight(SMALL_FONT_ID);
  r.drawText(SMALL_FONT_ID, x, y, tierName, true, EpdFontFamily::BOLD);
  x += nameW + 10;
  const int boxY = y + (textH - box) / 2;
  for (uint8_t i = 0; i < SLEEP_ROOM_TIER_COUNT; ++i) {
    if (i < tier) {
      r.fillRect(x, boxY, box, box, true);
    } else {
      r.drawRect(x, boxY, box, box, 2, true);
    }
    x += box + gap;
  }
  r.drawText(SMALL_FONT_ID, x - gap + 10, y, next);
}

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool writeHeader(FsFile& file, const int width, const int height, const int rowStride) {
  uint8_t header[54]{};
  const uint32_t paletteBytes = 256 * 4;
  const uint32_t dataOffset = 54 + paletteBytes;
  const uint32_t imageBytes = static_cast<uint32_t>(rowStride) * static_cast<uint32_t>(height);
  header[0] = 'B';
  header[1] = 'M';
  putLe32(header + 2, dataOffset + imageBytes);
  putLe32(header + 10, dataOffset);
  putLe32(header + 14, 40);
  putLe32(header + 18, static_cast<uint32_t>(width));
  putLe32(header + 22, static_cast<uint32_t>(-height));  // top-down rows
  putLe16(header + 26, 1);
  putLe16(header + 28, 8);
  putLe32(header + 34, imageBytes);
  putLe32(header + 46, 256);
  if (file.write(header, sizeof(header)) != sizeof(header)) return false;
  uint8_t palette[256 * 4];
  for (int i = 0; i < 256; ++i) {
    palette[i * 4] = palette[i * 4 + 1] = palette[i * 4 + 2] = static_cast<uint8_t>(i);
    palette[i * 4 + 3] = 0;
  }
  return file.write(palette, sizeof(palette)) == sizeof(palette);
}

}  // namespace

bool writePokemonSleepImage(const GfxRenderer& renderer, const bool fullScreen) {
  std::unique_ptr<PokemonSnapshot> snapshot(new (std::nothrow) PokemonSnapshot());
  if (!snapshot) return false;
  if (devicePokemonService().loadSnapshot(*snapshot) != ServiceStatus::Ok || snapshot->partyCount == 0) {
    LOG_INF("PKSLP", "No Pokemon party for the sleep screen");
    return false;
  }
  const uint32_t pick = static_cast<uint32_t>(random(static_cast<long>(snapshot->partyCount)));
  const PokemonRecord record = snapshot->party[pick];
  const SpeciesData* species = speciesData(record.speciesId);
  if (species == nullptr) return false;
  const uint8_t level = levelXpProgress(record.totalXp).level;
  const uint8_t badges = static_cast<uint8_t>(__builtin_popcount(snapshot->state.battleProgress & 0xFFu));
  snapshot.reset();

  SleepRoomSpec spec;
  spec.primary = species->primaryType;
  spec.secondary = species->secondaryType;
  spec.tier = sleepRoomTierForLevel(level);
  spec.badges = badges;
  spec.seed = record.recordId;
  spec.layout = fullScreen ? SleepRoomLayout::FullScreen : SleepRoomLayout::PokeBall;

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  std::unique_ptr<SleepRoomRenderer> room(new (std::nothrow) SleepRoomRenderer());
  if (!room) return false;

  // The Pokemon itself: the grayscale portrait when the art pack has it,
  // otherwise the 1-bit hero sprite. Set before build(), which places the
  // "z Z" by it.
  int spriteW = 0, spriteH = 0;
  std::unique_ptr<uint8_t[]> gray4, ink, opaque;
  SleepRoomSprite sprite;
  if (loadPortrait(record.speciesId, spriteW, spriteH, gray4)) {
    sprite.gray4 = gray4.get();
  } else if (loadSprite(record.speciesId, spriteW, spriteH, ink, opaque)) {
    sprite.ink = ink.get();
    sprite.opaque = opaque.get();
  } else {
    LOG_INF("PKSLP", "No art for species %u; drawing the room only", record.speciesId);
  }
  sprite.width = spriteW;
  sprite.height = spriteH;
  room->setSprite(sprite);

  if (!room->build(spec, width, height)) {
    LOG_ERR("PKSLP", "Not enough memory for the sleep room");
    return false;
  }
  if (room->overflowed()) {
    LOG_DBG("PKSLP", "Sleep room display list full; some furniture skipped");
  }

  renderer.clearScreen();
  drawLabels(renderer, record, *species, level, spec.tier, *room, fullScreen);

  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("PKSLP", POKEMON_SLEEP_IMAGE_PATH, file)) {
    LOG_ERR("PKSLP", "Cannot write %s", POKEMON_SLEEP_IMAGE_PATH);
    renderer.clearScreen();
    return false;
  }
  const int rowStride = (width + 3) & ~3;
  std::unique_ptr<uint8_t[]> row(new (std::nothrow) uint8_t[rowStride]);
  bool ok = row != nullptr && writeHeader(file, width, height, rowStride);
  if (ok) std::memset(row.get(), 255, rowStride);
  for (int y = 0; ok && y < height; ++y) {
    if (y < room->sceneHeight()) {
      room->renderRow(y, row.get());
    } else {
      std::memset(row.get(), 255, width);
    }
    for (int x = 0; x < width; ++x) {
      if (renderer.isPixelBlack(x, y)) row[x] = 0;
    }
    ok = file.write(row.get(), rowStride) == static_cast<size_t>(rowStride);
  }
  file.close();
  renderer.clearScreen();
  if (!ok) {
    LOG_ERR("PKSLP", "Failed writing the Pokemon sleep image");
    return false;
  }
  LOG_INF("PKSLP", "Pokemon sleep screen: species %u Lv %u tier %u", record.speciesId, level, spec.tier);
  return true;
}

}  // namespace pokemon

#endif
