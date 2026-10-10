#pragma once

// Procedural "Pokemon asleep in its Poke Ball" sleep-screen scene.
//
// The room behind the ball's window is a one-point-perspective box. Walls,
// floor and ceiling are shaded per pixel with integer-only maths (the X3's
// ESP32-C3 has no FPU), so their textures stay perspective-correct and cost no
// memory. Furniture is a display list of polygons and ellipses with gradient
// paints, projected once in build(). renderRow() then produces one row of 8-bit
// luminance at a time, so the caller can stream the scene to a BMP without a
// full-screen buffer.
//
// What the room contains depends on the Pokemon:
//   - its level picks one of SLEEP_ROOM_TIER_COUNT tiers (more and finer
//     furniture as it grows);
//   - its primary type picks the back wall and the view out of the window, its
//     secondary type the side walls; type furniture alternates between the two.

#include <cstddef>
#include <cstdint>

#include "PokemonSpecies.h"

namespace pokemon {

constexpr uint8_t SLEEP_ROOM_TIER_COUNT = 7;

// 1-based tier for a Pokemon level (1..100).
uint8_t sleepRoomTierForLevel(uint8_t level);
// First level of a 1-based tier; 0 for an invalid tier.
uint8_t sleepRoomTierFirstLevel(uint8_t tier);

// PokeBall: the room is seen through the window of a Poke Ball in the upper
// part of the screen, with the labels underneath. FullScreen: the room fills
// the whole screen and the labels sit on a panel along the bottom edge.
enum class SleepRoomLayout : uint8_t { PokeBall, FullScreen };

struct SleepRoomSpec {
  PokemonType primary = PokemonType::Normal;
  PokemonType secondary = PokemonType::None;
  uint8_t tier = 1;
  uint8_t badges = 0;  // gym badges shown in the badge case (tier 6+)
  uint32_t seed = 1;   // varies textures (brick tones, planks) between screens
  SleepRoomLayout layout = SleepRoomLayout::PokeBall;
};

// The sleeping Pokemon, in one of two forms:
//   - a 4-bit grayscale portrait (the art pack's sleep/NNN.bmp): `gray4` rows
//     of (width + 1) / 2 bytes, high nibble first; values 0..14 are grays from
//     black to white, 15 is transparent;
//   - a 1-bit sprite (the art pack's hero sprites): rows packed MSB first,
//     (width + 7) / 8 bytes each; `ink` marks dark pixels, `opaque` the pixels
//     that belong to the Pokemon.
struct SleepRoomSprite {
  int width = 0;
  int height = 0;
  const uint8_t* gray4 = nullptr;
  const uint8_t* ink = nullptr;
  const uint8_t* opaque = nullptr;
};

constexpr uint8_t SLEEP_ROOM_GRAY4_TRANSPARENT = 15;

// Marks as transparent every white pixel connected to the sprite's border, so
// the white background of an art-pack sprite does not paint a box over the
// room. `opaque` receives (width + 7) / 8 * height bytes.
void sleepRoomSpriteMask(const uint8_t* ink, int width, int height, uint8_t* opaque);

class SleepRoomRenderer {
 public:
  SleepRoomRenderer() = default;
  ~SleepRoomRenderer();
  SleepRoomRenderer(const SleepRoomRenderer&) = delete;
  SleepRoomRenderer& operator=(const SleepRoomRenderer&) = delete;

  // Sets the sleeping Pokemon. Call before build(): the scene places the
  // "z Z" above it. The data must outlive the renderer's use.
  void setSprite(const SleepRoomSprite& sprite);
  // Builds the scene for a screenWidth x screenHeight portrait screen.
  // Returns false if the display list could not be allocated.
  bool build(const SleepRoomSpec& spec, int screenWidth, int screenHeight);

  // Number of rows the scene covers from the top of the screen; rows below
  // are plain white and need not be rendered.
  int sceneHeight() const;
  // Writes screenWidth luminance values (0 = black, 255 = white) for row y.
  void renderRow(int y, uint8_t* out) const;

  // FullScreen layout: the label panel along the bottom edge.
  int panelTop() const { return panelTop_; }
  int panelBottom() const { return panelBottom_; }
  // PokeBall layout: centre of the ball's button, where the caller prints the level.
  int buttonCenterX() const;
  int buttonCenterY() const;
  int buttonRadius() const;
  // Where the sprite stands: its bottom-centre on the bed, in screen pixels.
  int spriteBaseX() const { return spriteBaseX_; }
  int spriteBaseY() const { return spriteBaseY_; }

  // Display-list usage, for tests and logging.
  size_t primitiveCount() const { return primCount_; }
  bool overflowed() const { return overflow_; }

  struct Prim;
  struct Paint;
  struct Vertex {
    int16_t x;  // quarter pixels
    int16_t y;
  };
  static constexpr size_t MAX_PRIMS = 720;
  static constexpr size_t MAX_VERTS = 3200;
  static constexpr size_t MAX_PAINTS = 240;

 private:
  friend class SleepRoomBuilder;

  void release();
  uint8_t shadeRoom(int x, int y, int rowT, int rowSurface) const;
  uint8_t shadeShell(int x, int y, uint8_t room) const;
  uint8_t shadeFrame(int x, int y, uint8_t room) const;
  void drawPrimsRow(int y, uint8_t* out, int x0, int x1, int keyFrom, int keyTo, size_t& index) const;
  void drawSpriteRow(int y, uint8_t* out, int x0, int x1) const;

  SleepRoomSpec spec_{};
  int width_ = 0;
  int height_ = 0;
  int ballCx_ = 0;
  bool fullScreen_ = false;
  int vy_ = 0;      // vanishing point row
  int f_ = 1;       // focal length, px
  int sceneH_ = 0;  // rows the scene covers
  int panelTop_ = 0;
  int panelBottom_ = 0;
  // Sprite placement: drawn at drawW_ x drawH_, its opaque box's bottom-centre
  // on (spriteBaseX_, spriteBaseY_).
  int drawW_ = 0;
  int drawH_ = 0;
  int opaqueLeft_ = 0, opaqueRight_ = 0, opaqueBottom_ = 0, opaqueTop_ = 0;
  int spriteBaseX_ = 0;
  int spriteBaseY_ = 0;
  int spriteKey_ = 0;
  bool hasWindow_ = false;

  Prim* prims_ = nullptr;
  Vertex* verts_ = nullptr;
  Paint* paints_ = nullptr;
  int32_t* sideT_ = nullptr;  // per column: depth (mm) where the ray meets a side wall
  size_t primCount_ = 0;
  size_t vertCount_ = 0;
  size_t paintCount_ = 0;
  bool overflow_ = false;

  SleepRoomSprite sprite_{};
};

}  // namespace pokemon
