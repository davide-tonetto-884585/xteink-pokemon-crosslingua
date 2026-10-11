#include <PokemonSleepRoom.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using pokemon::PokemonType;
using pokemon::SleepRoomRenderer;
using pokemon::SleepRoomSpec;

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 800;

std::vector<uint8_t> renderScene(const SleepRoomSpec& spec, const pokemon::SleepRoomSprite* sprite = nullptr) {
  SleepRoomRenderer room;
  if (sprite != nullptr) room.setSprite(*sprite);
  EXPECT_TRUE(room.build(spec, kWidth, kHeight));
  std::vector<uint8_t> image(static_cast<size_t>(kWidth) * room.sceneHeight());
  for (int y = 0; y < room.sceneHeight(); ++y) room.renderRow(y, &image[static_cast<size_t>(y) * kWidth]);
  return image;
}

constexpr PokemonType kGen1Types[] = {
    PokemonType::Normal,  PokemonType::Fire,     PokemonType::Water,  PokemonType::Electric, PokemonType::Grass,
    PokemonType::Ice,     PokemonType::Fighting, PokemonType::Poison, PokemonType::Ground,   PokemonType::Flying,
    PokemonType::Psychic, PokemonType::Bug,      PokemonType::Rock,   PokemonType::Ghost,    PokemonType::Dragon,
};

}  // namespace

TEST(PokemonSleepRoom, TierForLevelCoversAllLevels) {
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(1), 1);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(10), 1);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(11), 2);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(35), 3);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(36), 4);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(65), 5);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(66), 6);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(86), 7);
  EXPECT_EQ(pokemon::sleepRoomTierForLevel(100), 7);
  uint8_t previous = 1;
  for (int level = 1; level <= 100; ++level) {
    const uint8_t tier = pokemon::sleepRoomTierForLevel(static_cast<uint8_t>(level));
    EXPECT_GE(tier, previous);
    EXPECT_LE(tier, pokemon::SLEEP_ROOM_TIER_COUNT);
    previous = tier;
  }
  for (uint8_t tier = 1; tier <= pokemon::SLEEP_ROOM_TIER_COUNT; ++tier) {
    EXPECT_EQ(pokemon::sleepRoomTierForLevel(pokemon::sleepRoomTierFirstLevel(tier)), tier);
  }
  EXPECT_EQ(pokemon::sleepRoomTierFirstLevel(0), 0);
  EXPECT_EQ(pokemon::sleepRoomTierFirstLevel(8), 0);
}

TEST(PokemonSleepRoom, EveryTypeAndTierFitsTheDisplayList) {
  for (const PokemonType primary : kGen1Types) {
    for (const PokemonType secondary : {PokemonType::None, PokemonType::Flying, PokemonType::Poison}) {
      for (uint8_t tier = 1; tier <= pokemon::SLEEP_ROOM_TIER_COUNT; ++tier) {
        SleepRoomSpec spec;
        spec.primary = primary;
        spec.secondary = secondary;
        spec.tier = tier;
        spec.badges = 8;
        SleepRoomRenderer room;
        ASSERT_TRUE(room.build(spec, kWidth, kHeight));
        EXPECT_FALSE(room.overflowed()) << "type " << static_cast<int>(primary) << "/" << static_cast<int>(secondary)
                                        << " tier " << static_cast<int>(tier);
        EXPECT_GT(room.primitiveCount(), 0u);
      }
    }
  }
}

TEST(PokemonSleepRoom, RenderingIsDeterministic) {
  SleepRoomSpec spec;
  spec.primary = PokemonType::Fire;
  spec.secondary = PokemonType::Flying;
  spec.tier = 5;
  spec.seed = 42;
  EXPECT_EQ(renderScene(spec), renderScene(spec));
}

TEST(PokemonSleepRoom, HigherTiersAddFurniture) {
  // Tier 1's straw nest is drawn as many short strands, so start from tier 2.
  size_t previous = 0;
  for (uint8_t tier = 2; tier <= pokemon::SLEEP_ROOM_TIER_COUNT; ++tier) {
    SleepRoomSpec spec;
    spec.primary = PokemonType::Electric;
    spec.tier = tier;
    SleepRoomRenderer room;
    ASSERT_TRUE(room.build(spec, kWidth, kHeight));
    if (tier > 2) {
      EXPECT_GT(room.primitiveCount(), previous) << "tier " << static_cast<int>(tier);
    }
    previous = room.primitiveCount();
  }
}

TEST(PokemonSleepRoom, TypesChangeTheRoomAndDualTypesMergeIt) {
  SleepRoomSpec fire;
  fire.primary = PokemonType::Fire;
  fire.tier = 5;
  SleepRoomSpec flying = fire;
  flying.primary = PokemonType::Flying;
  SleepRoomSpec both = fire;
  both.secondary = PokemonType::Flying;
  const auto a = renderScene(fire), b = renderScene(flying), c = renderScene(both);
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
  // The dual-type room keeps the primary type's back wall: the centre of the
  // back wall, beside the window, matches the mono-type room's.
  const size_t probe = static_cast<size_t>(205) * kWidth + 150;
  EXPECT_EQ(a[probe], c[probe]);
}

TEST(PokemonSleepRoom, OutsideTheBallStaysWhite) {
  SleepRoomSpec spec;
  spec.primary = PokemonType::Water;
  spec.tier = 3;
  const auto image = renderScene(spec);
  EXPECT_EQ(image[0], 255);
  EXPECT_EQ(image[kWidth - 1], 255);
  EXPECT_EQ(image[static_cast<size_t>(40) * kWidth + 5], 255);
  // The ball's outline is black at its rightmost point.
  EXPECT_EQ(image[static_cast<size_t>(300) * kWidth + 240 + 222], 0);
  // Inside the window there is shading, not a flat fill.
  const auto mid = image.begin() + static_cast<long>(200) * kWidth;
  const auto [lo, hi] = std::minmax_element(mid + 120, mid + 360);
  EXPECT_GT(*hi - *lo, 40);
}

TEST(PokemonSleepRoom, SpriteMaskKeepsInteriorWhiteAndDropsBackground) {
  // 16x8 sprite: a hollow 8x6 box at (4,1); its inside is white but enclosed.
  constexpr int w = 16, h = 8, rb = 2;
  std::vector<uint8_t> ink(rb * h, 0), opaque(rb * h, 0);
  auto set = [&](int x, int y) { ink[y * rb + x / 8] |= static_cast<uint8_t>(0x80 >> (x & 7)); };
  for (int x = 4; x < 12; ++x) {
    set(x, 1);
    set(x, 6);
  }
  for (int y = 1; y < 7; ++y) {
    set(4, y);
    set(11, y);
  }
  pokemon::sleepRoomSpriteMask(ink.data(), w, h, opaque.data());
  auto isOpaque = [&](int x, int y) { return (opaque[y * rb + x / 8] >> (7 - (x & 7))) & 1; };
  EXPECT_FALSE(isOpaque(0, 0));
  EXPECT_FALSE(isOpaque(13, 4));
  EXPECT_TRUE(isOpaque(4, 1));
  EXPECT_TRUE(isOpaque(7, 3));  // enclosed white
}

TEST(PokemonSleepRoom, SpriteIsDrawnOnTheBed) {
  SleepRoomSpec spec;
  spec.primary = PokemonType::Normal;
  spec.tier = 2;
  SleepRoomRenderer probe;
  ASSERT_TRUE(probe.build(spec, kWidth, kHeight));
  constexpr int w = 8, h = 8;
  std::vector<uint8_t> ink(h, 0xFF), opaque(h, 0xFF);
  const pokemon::SleepRoomSprite sprite{w, h, ink.data(), opaque.data()};
  const auto plain = renderScene(spec);
  const auto withSprite = renderScene(spec, &sprite);
  EXPECT_NE(plain, withSprite);
  const int x = probe.spriteBaseX();
  const int y = probe.spriteBaseY() - 2;
  EXPECT_LT(withSprite[static_cast<size_t>(y) * kWidth + x], 60);
}

TEST(PokemonSleepRoom, FullScreenLayoutFillsTheScreenWithAPanel) {
  SleepRoomSpec spec;
  spec.primary = PokemonType::Fire;
  spec.tier = 4;
  spec.layout = pokemon::SleepRoomLayout::FullScreen;
  SleepRoomRenderer room;
  ASSERT_TRUE(room.build(spec, kWidth, kHeight));
  EXPECT_EQ(room.sceneHeight(), kHeight);
  ASSERT_LT(room.panelTop(), room.panelBottom());
  EXPECT_LE(room.panelBottom(), kHeight);
  std::vector<uint8_t> row(kWidth);
  // The room reaches the corners (no white margin, unlike the ball layout).
  room.renderRow(5, row.data());
  EXPECT_LT(row[5], 250);
  // The panel's interior is white.
  room.renderRow((room.panelTop() + room.panelBottom()) / 2, row.data());
  EXPECT_GE(row[kWidth / 2], 245);
  EXPECT_FALSE(room.overflowed());
}

TEST(PokemonSleepRoom, GrayPortraitKeepsItsTransparentPixels) {
  // 4x4 portrait: transparent border, a dark 2x2 centre (gray index 0).
  constexpr int w = 4, h = 4, rb = 2;
  std::vector<uint8_t> gray(rb * h, 0xFF);  // all transparent (15)
  gray[1 * rb + 0] = 0xF0;                  // (1,1) = 0, (0,1) = 15
  gray[1 * rb + 1] = 0x0F;                  // (2,1) = 0
  gray[2 * rb + 0] = 0xF0;
  gray[2 * rb + 1] = 0x0F;
  pokemon::SleepRoomSprite sprite;
  sprite.width = w;
  sprite.height = h;
  sprite.gray4 = gray.data();
  SleepRoomSpec spec;
  spec.primary = PokemonType::Normal;
  spec.tier = 2;
  const auto plain = renderScene(spec);
  const auto withSprite = renderScene(spec, &sprite);
  EXPECT_NE(plain, withSprite);
  // Far from the bed nothing changes: transparent pixels never paint.
  for (int y = 0; y < 200; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      ASSERT_EQ(plain[static_cast<size_t>(y) * kWidth + x], withSprite[static_cast<size_t>(y) * kWidth + x]);
    }
  }
}
