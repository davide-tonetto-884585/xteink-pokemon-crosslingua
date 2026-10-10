#if defined(CROSSINK_ENABLE_POKEMON)

#include "PokemonSleepRoom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace pokemon {

// ---------------------------------------------------------------------------
// Tiers

namespace {
constexpr uint8_t TIER_FIRST_LEVEL[SLEEP_ROOM_TIER_COUNT] = {1, 11, 21, 36, 51, 66, 86};
}  // namespace

uint8_t sleepRoomTierForLevel(const uint8_t level) {
  uint8_t tier = 1;
  for (uint8_t i = 0; i < SLEEP_ROOM_TIER_COUNT; ++i) {
    if (level >= TIER_FIRST_LEVEL[i]) tier = static_cast<uint8_t>(i + 1);
  }
  return tier;
}

uint8_t sleepRoomTierFirstLevel(const uint8_t tier) {
  if (tier < 1 || tier > SLEEP_ROOM_TIER_COUNT) return 0;
  return TIER_FIRST_LEVEL[tier - 1];
}

// ---------------------------------------------------------------------------
// Sprite background mask

void sleepRoomSpriteMask(const uint8_t* ink, const int width, const int height, uint8_t* opaque) {
  if (ink == nullptr || opaque == nullptr || width <= 0 || height <= 0) return;
  const int rowBytes = (width + 7) / 8;
  const auto bit = [rowBytes](const uint8_t* bits, int x, int y) {
    return (bits[y * rowBytes + x / 8] >> (7 - (x & 7))) & 1;
  };
  const auto clearBit = [rowBytes](uint8_t* bits, int x, int y) {
    bits[y * rowBytes + x / 8] &= static_cast<uint8_t>(~(0x80 >> (x & 7)));
  };
  // Start fully opaque, then flood the border-connected white area to
  // transparent with alternating sweeps (no queue: the sprite is small and the
  // sweeps converge in a handful of passes).
  std::memset(opaque, 0xFF, static_cast<size_t>(rowBytes) * height);
  const auto isBackground = [&](int x, int y) { return bit(ink, x, y) == 0 && bit(opaque, x, y) == 0; };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if ((x == 0 || y == 0 || x == width - 1 || y == height - 1) && bit(ink, x, y) == 0) clearBit(opaque, x, y);
    }
  }
  bool changed = true;
  for (int pass = 0; changed && pass < 64; ++pass) {
    changed = false;
    const bool forward = (pass & 1) == 0;
    for (int yi = 0; yi < height; ++yi) {
      const int y = forward ? yi : height - 1 - yi;
      for (int xi = 0; xi < width; ++xi) {
        const int x = forward ? xi : width - 1 - xi;
        if (bit(ink, x, y) != 0 || bit(opaque, x, y) == 0) continue;
        if ((x > 0 && isBackground(x - 1, y)) || (x + 1 < width && isBackground(x + 1, y)) ||
            (y > 0 && isBackground(x, y - 1)) || (y + 1 < height && isBackground(x, y + 1))) {
          clearBit(opaque, x, y);
          changed = true;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Display list

struct SleepRoomRenderer::Paint {
  uint8_t kind = 0;  // 1 = linear, 2 = radial
  uint8_t stops = 0;
  int32_t x0 = 0, y0 = 0, x1 = 1, y1 = 1;  // linear: from/to; radial: centre/radii (quarter px)
  int64_t len2 = 1;
  uint8_t pos[4]{};
  uint8_t lum[4]{};
  uint8_t alpha[4]{};
};

struct SleepRoomRenderer::Prim {
  int16_t key = 0;    // painter order: larger keys are farther and drawn first
  uint8_t shape = 0;  // 0 = polygon, 1 = ellipse
  uint8_t lum = 0;
  uint8_t alpha = 255;
  uint8_t nv = 0;
  uint16_t paint = 0xFFFF;  // 0xFFFF = solid lum/alpha
  uint16_t v = 0;
  int16_t yMin = 0, yMax = -1;               // pixel rows
  int16_t ex = 0, ey = 0, erx = 0, ery = 0;  // ellipse, quarter px
  int16_t clipX0 = 0, clipX1 = 0, clipY0 = 0, clipY1 = 0;
};

namespace {

// Scene geometry (pixels and millimetres). Sized for a 480 px wide portrait
// screen; narrower or wider screens keep the same ball, centred.
constexpr int BALL_CY = 300;
constexpr int BALL_R = 222;
constexpr int WIN_R = 188;
constexpr int RIM_W = 12;
constexpr int BALL_SCENE_H = 548;
constexpr int BALL_VY = 256;  // vanishing point row
constexpr int BALL_F = 300;   // focal length (px)
constexpr int H_MM = 2300;    // camera height
constexpr int W_MM = 3200;    // room half width
constexpr int ZB_MM = 7400;   // back wall depth
constexpr int CH_MM = 4400;   // ceiling height
constexpr int BUTTON_R = 25;

constexpr float Hm = H_MM / 1000.0f;
constexpr float Wm = W_MM / 1000.0f;
constexpr float ZBm = ZB_MM / 1000.0f;
constexpr float CHm = CH_MM / 1000.0f;

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}

uint32_t isqrt32(uint32_t v) {
  uint32_t res = 0;
  uint32_t one = 1u << 30;
  while (one > v) one >>= 2;
  while (one != 0) {
    if (v >= res + one) {
      v -= res + one;
      res = (res >> 1) + one;
    } else {
      res >>= 1;
    }
    one >>= 2;
  }
  return res;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// floor(a / b) for b > 0
int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int floormod(int a, int b) { return a - floordiv(a, b) * b; }

uint8_t blend(uint8_t dst, int lum, int alpha) {
  return static_cast<uint8_t>(dst + ((lum - dst) * alpha + (lum >= dst ? 127 : -127)) / 255);
}

int darken(int lum, int amount255) { return lum - lum * amount255 / 255; }

// Triangle wave in [-amp, amp] with the given period.
int triWave(int u, int period, int amp) {
  const int m = floormod(u, period);
  const int half = period / 2;
  return (m < half ? m : period - m) * 2 * amp / half - amp;
}

// ---------------------------------------------------------------------------
// Wall materials (integer, per pixel). u runs along the wall, v is the height,
// both in mm; mpp is millimetres per screen pixel at that depth.

enum class Material : uint8_t {
  Stripes,
  Brick,
  Zigzag,
  Waves,
  Panels,
  Bubbles,
  Clouds,
  Crystal,
  Dojo,
  Strata,
  Spirals,
  Honeycomb,
  Stone,
  Torn,
  Scales,
};

Material materialFor(const PokemonType type) {
  switch (type) {
    case PokemonType::Fire:
      return Material::Brick;
    case PokemonType::Water:
      return Material::Waves;
    case PokemonType::Electric:
      return Material::Zigzag;
    case PokemonType::Grass:
      return Material::Panels;
    case PokemonType::Ice:
      return Material::Crystal;
    case PokemonType::Fighting:
      return Material::Dojo;
    case PokemonType::Poison:
      return Material::Bubbles;
    case PokemonType::Ground:
      return Material::Strata;
    case PokemonType::Flying:
      return Material::Clouds;
    case PokemonType::Psychic:
      return Material::Spirals;
    case PokemonType::Bug:
      return Material::Honeycomb;
    case PokemonType::Rock:
      return Material::Stone;
    case PokemonType::Ghost:
      return Material::Torn;
    case PokemonType::Dragon:
      return Material::Scales;
    default:
      return Material::Stripes;
  }
}

int lineCover(int dist, int halfWidth) { return dist < halfWidth ? 255 : 0; }

int wallMaterial(const Material m, const int u, const int v, const int mpp, const uint32_t seed) {
  const int hw = std::max(mpp * 4 / 5, 6);  // ~1.6 px line
  switch (m) {
    case Material::Brick: {
      const int row = floordiv(v, 200);
      const int off = (row & 1) ? 220 : 0;
      const int uu = u + off + 20000;
      if (floormod(v, 200) < std::max(20, mpp) || floormod(uu, 440) < std::max(20, mpp)) return 199;
      return 97 + static_cast<int>(hash3(row, uu / 440, seed) % 56);
    }
    case Material::Zigzag: {
      const int base = 204;
      const int f = triWave(u, 300, 60);
      const int d = floormod(v - 500 - f + 210, 420) - 210;
      return lineCover(std::abs(d), hw) ? 128 : base;
    }
    case Material::Waves: {
      // Parabolic approximation of a sine, period 600 mm, amplitude 50 mm.
      const int m6 = floormod(u, 600);
      const int half = m6 < 300 ? m6 : m6 - 300;
      const int bump = (half * (300 - half)) * 50 / 22500;
      const int f = m6 < 300 ? bump : -bump;
      const int d = floormod(v - 500 - f + 210, 420) - 210;
      if (std::abs(d) < hw) return 122;
      const int d2 = floormod(v - 600 + f / 2 + 210, 420) - 210;
      if (std::abs(d2) < hw / 2 + 2) return 153;
      return 194;
    }
    case Material::Panels: {
      const int uu = u + 20000;
      const int board = uu / 280;
      int lum = 128 + static_cast<int>(hash3(board, 7, seed) % 36);
      if (uu % 280 < std::max(mpp, 10)) return 64;
      for (int k = 0; k < 3; ++k) {
        const int gx = board * 280 + 30 + static_cast<int>(hash3(board, k, seed) % 220);
        const int wav = triWave(v + k * 300, 1200, 15);
        if (std::abs(uu - gx - wav) < std::max(mpp / 4, 3)) lum -= 25;
      }
      return lum;
    }
    case Material::Bubbles: {
      const int cu = floordiv(u, 260), cv = floordiv(v, 260);
      int lum = 189;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const uint32_t h = hash3(cu + dx + 5000, cv + dy + 5000, seed);
          if ((h & 3) == 0) continue;
          const int ccx = (cu + dx) * 260 + 40 + static_cast<int>(h % 180);
          const int ccy = (cv + dy) * 260 + 40 + static_cast<int>((h >> 8) % 180);
          const int r = 50 + static_cast<int>((h >> 16) % 90);
          const int ddx = u - ccx, ddy = v - ccy;
          const int d2 = ddx * ddx + ddy * ddy;
          const int w = std::max(mpp / 2, 5);
          if (d2 > (r - w) * (r - w) && d2 < (r + w) * (r + w)) return 128;
          if (d2 < r * r) {
            lum = 219;
            if (std::abs(ddx + r * 35 / 100) < mpp && std::abs(ddy - r * 35 / 100) < mpp) return 255;
          }
        }
      }
      return lum;
    }
    case Material::Clouds: {
      const int cu = floordiv(u, 900), cv = floordiv(v, 700);
      int best = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const uint32_t h = hash3(cu + dx + 3000, cv + dy + 3000, seed ^ 0x55u);
          if ((h & 1) == 0) continue;
          const int ccx = (cu + dx) * 900 + 150 + static_cast<int>(h % 600);
          const int ccy = (cv + dy) * 700 + 150 + static_cast<int>((h >> 9) % 400);
          for (int k = 0; k < 3; ++k) {
            const int bx = ccx + (k - 1) * 220;
            const int by = ccy + (k == 1 ? 60 : 0);
            const int rx = 260, ry = 170;
            const int nx = (u - bx) * 256 / rx, ny = (v - by) * 256 / ry;
            const int q2 = nx * nx + ny * ny;
            const int s = clampi((300 * 300 - q2) / ((300 * 300 - 180 * 180) / 255), 0, 255);
            best = std::max(best, s);
          }
        }
      }
      return 179 + (245 - 179) * best / 255;
    }
    case Material::Crystal: {
      const int a = floormod(u + v, 300), b = floormod(u - v, 300);
      if (a < hw || b < hw) return 160;
      const uint32_t h = hash3(floordiv(u, 60), floordiv(v, 60), seed);
      if ((h & 63) == 0) return 255;
      return 214 + static_cast<int>(h % 12);
    }
    case Material::Dojo: {
      const int row = floordiv(v, 200);
      if (floormod(v, 200) < std::max(mpp, 10)) return 70;
      const int lum = 150 + static_cast<int>(hash3(row, floordiv(u + 20000, 1600 + row * 37 % 400), seed) % 30);
      return lum - (floormod(u + row * 530, 1700) < std::max(mpp, 10) ? 60 : 0);
    }
    case Material::Strata: {
      static constexpr uint8_t BANDS[6] = {196, 168, 184, 150, 190, 162};
      const int vv = v + triWave(u, 1400, 60);
      const int band = floordiv(vv, 260);
      if (floormod(vv, 260) < std::max(mpp, 8)) return 110;
      return BANDS[floormod(band, 6)] + static_cast<int>(hash3(floordiv(u, 80), floordiv(vv, 80), seed) % 10);
    }
    case Material::Spirals: {
      const int cu = floordiv(u, 520), cv = floordiv(v, 520);
      const int ddx = u - (cu * 520 + 260), ddy = v - (cv * 520 + 260);
      const int dist = static_cast<int>(isqrt32(static_cast<uint32_t>(ddx * ddx + ddy * ddy)));
      if (dist < 240 && floormod(dist + (ddx > 0 ? 20 : 0), 60) < std::max(hw, 10)) return 150;
      return 206;
    }
    case Material::Honeycomb: {
      // Hexagon distance on a skewed grid, cell size 180 mm.
      const int s = 180;
      const int row = floordiv(v, s * 866 / 1000);
      const int off = (row & 1) ? s / 2 : 0;
      const int col = floordiv(u - off, s);
      const int cx = col * s + off + s / 2;
      const int cy = row * (s * 866 / 1000) + s * 433 / 1000;
      const int px = std::abs(u - cx), py = std::abs(v - cy);
      const int d = std::max(px * 866 / 1000 + py / 2, py);
      if (d > s * 40 / 100) return 120;
      return 196 + static_cast<int>(hash3(row, col, seed) % 18);
    }
    case Material::Stone: {
      const int row = floordiv(v, 450);
      const int off = static_cast<int>(hash3(row, 1, seed) % 500);
      const int uu = u + off + 20000;
      const int col = uu / 700;
      if (floormod(v, 450) < std::max(25, mpp) || uu % 700 < std::max(25, mpp)) return 210;
      const int mottle = static_cast<int>(hash3(floordiv(u, 90), floordiv(v, 90), seed) % 16);
      return 120 + static_cast<int>(hash3(row, col, seed) % 50) + mottle;
    }
    case Material::Torn: {
      const uint32_t h = hash3(floordiv(u, 700), floordiv(v, 500), seed);
      if ((h & 7) == 0) {
        // a torn patch showing plaster
        const int ddx = floormod(u, 700) - 350, ddy = floormod(v, 500) - 250;
        if (std::abs(ddx) + std::abs(ddy) * 3 / 2 + triWave(u + v, 90, 25) < 230) return 205;
      }
      const int m = floormod(u, 260);
      if (m > 100 && m < 160) return 140;
      if (m == 100 || m == 160 || std::abs(m - 130) < 4) return 125;
      return 104;
    }
    case Material::Scales: {
      const int rowH = 140;
      const int row = floordiv(v, rowH);
      const int off = (row & 1) ? 100 : 0;
      const int col = floordiv(u - off, 200);
      const int cx = col * 200 + off + 100;
      const int cy = (row + 1) * rowH;
      const int ddx = u - cx, ddy = v - cy;
      const int dist = static_cast<int>(isqrt32(static_cast<uint32_t>(ddx * ddx + ddy * ddy)));
      if (std::abs(dist - 140) < std::max(mpp / 2 + 3, 8) && ddy < 0) return 70;
      return 150 - clampi(dist - 60, 0, 80) / 2;
    }
    case Material::Stripes:
    default: {
      const int m = floormod(u, 120);
      return m < std::max(12, hw) ? 186 : 212;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Builder: projects furniture into the display list. Uses float maths; it runs
// once per sleep screen, so soft-float cost does not matter here.

namespace {
struct V2 {
  float x;
  float y;
};

struct Stop {
  float pos;
  float lum;  // 0..1
  float alpha = 1.0f;
};

enum Slot : uint8_t {
  FloorLeft,
  FloorRight,
  WallLeft,
  Shelf,
  BigLeft,
  BigRight,
  SideLeft,
  SideRight,
  Ceiling2,
  SLOT_COUNT,
};

struct ShelfInfo {
  float x0, x1, y;
  bool valid = false;
};

}  // namespace

class SleepRoomBuilder {
 public:
  SleepRoomBuilder(SleepRoomRenderer& r, const SleepRoomSpec& spec)
      : r_(r),
        spec_(spec),
        vx_(static_cast<float>(r.width_) / 2.0f),
        vyf_(static_cast<float>(r.vy_)),
        fpx_(static_cast<float>(r.f_)),
        rng_(spec.seed * 2654435761u + 1) {
    resetClip();
  }

  // --- state
  void key(float zMeters) { key_ = static_cast<int16_t>(clampi(static_cast<int>(zMeters * 1000.0f), -1000, 32000)); }
  void keyRaw(int k) { key_ = static_cast<int16_t>(k); }
  void clip(float x0, float y0, float x1, float y1) {
    clipX0_ = static_cast<int16_t>(std::floor(x0));
    clipY0_ = static_cast<int16_t>(std::floor(y0));
    clipX1_ = static_cast<int16_t>(std::ceil(x1));
    clipY1_ = static_cast<int16_t>(std::ceil(y1));
  }
  void resetClip() {
    clipX0_ = 0;
    clipX1_ = static_cast<int16_t>(r_.width_);
    clipY0_ = 0;
    clipY1_ = r_.sceneH_;
  }
  float rand(float lo, float hi) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return lo + (hi - lo) * static_cast<float>(rng_ % 100000u) / 100000.0f;
  }

  // --- projection
  V2 P(float x, float y, float z) const { return {vx_ + fpx_ * x / z, vyf_ - fpx_ * (y - Hm) / z}; }
  float scaleAt(float z) const { return fpx_ / z; }  // px per metre

  // --- paints
  int linear(V2 a, V2 b, const Stop* stops, int n) { return addPaint(1, a.x, a.y, b.x, b.y, stops, n); }
  int radial(V2 c, float rx, float ry, const Stop* stops, int n) {
    return addPaint(2, c.x, c.y, std::max(rx, 0.5f), std::max(ry, 0.5f), stops, n);
  }
  int cylPaint(float xl, float xr, float lum) {
    const Stop s[4] = {{0, lum * 0.55f}, {0.35f, std::min(1.0f, lum * 1.18f)}, {0.6f, lum}, {1, lum * 0.45f}};
    return linear({xl, 0}, {xr, 0}, s, 4);
  }
  int vgrad(float y0, float y1, float l0, float l1) {
    const Stop s[2] = {{0, l0}, {1, l1}};
    return linear({0, y0}, {0, y1}, s, 2);
  }

  // --- primitives (screen space, pixels)
  void poly(const V2* pts, int n, float lum, float alpha = 1.0f, int paint = -1) {
    if (n < 3) return;
    auto& r = r_;
    if (r.primCount_ >= SleepRoomRenderer::MAX_PRIMS || r.vertCount_ + n > SleepRoomRenderer::MAX_VERTS) {
      r.overflow_ = true;
      return;
    }
    float yMin = pts[0].y, yMax = pts[0].y;
    for (int i = 0; i < n; ++i) {
      yMin = std::min(yMin, pts[i].y);
      yMax = std::max(yMax, pts[i].y);
    }
    if (yMax < 0 || yMin > r_.sceneH_) return;
    SleepRoomRenderer::Prim& p = r.prims_[r.primCount_++];
    p = {};
    p.key = key_;
    p.shape = 0;
    p.lum = toLum(lum);
    p.alpha = toLum(alpha);
    p.nv = static_cast<uint8_t>(n);
    p.paint = paint < 0 ? 0xFFFF : static_cast<uint16_t>(paint);
    p.v = static_cast<uint16_t>(r.vertCount_);
    p.yMin = static_cast<int16_t>(std::floor(yMin));
    p.yMax = static_cast<int16_t>(std::ceil(yMax));
    setClip(p);
    for (int i = 0; i < n; ++i) {
      r.verts_[r.vertCount_++] = {q4(pts[i].x), q4(pts[i].y)};
    }
  }
  void ellipse(V2 c, float rx, float ry, float lum, float alpha = 1.0f, int paint = -1) {
    if (rx <= 0.1f || ry <= 0.1f) return;
    auto& r = r_;
    if (r.primCount_ >= SleepRoomRenderer::MAX_PRIMS) {
      r.overflow_ = true;
      return;
    }
    SleepRoomRenderer::Prim& p = r.prims_[r.primCount_++];
    p = {};
    p.key = key_;
    p.shape = 1;
    p.lum = toLum(lum);
    p.alpha = toLum(alpha);
    p.paint = paint < 0 ? 0xFFFF : static_cast<uint16_t>(paint);
    p.ex = q4(c.x);
    p.ey = q4(c.y);
    p.erx = q4(rx);
    p.ery = q4(ry);
    p.yMin = static_cast<int16_t>(std::floor(c.y - ry));
    p.yMax = static_cast<int16_t>(std::ceil(c.y + ry));
    setClip(p);
  }
  void line(V2 a, V2 b, float w, float lum, float alpha = 1.0f) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    const float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    const V2 q[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
    poly(q, 4, lum, alpha);
  }
  void polyline(const V2* pts, int n, float w, float lum, float alpha = 1.0f) {
    for (int i = 0; i + 1 < n; ++i) line(pts[i], pts[i + 1], w, lum, alpha);
  }
  void glow(V2 c, float rx, float ry, float alpha) {
    const Stop s[2] = {{0, 1, alpha}, {1, 1, 0}};
    ellipse(c, rx, ry, 1, 1, radial(c, rx, ry, s, 2));
  }
  void softShadow(V2 c, float rx, float ry, float alpha) {
    const Stop s[3] = {{0, 0, alpha}, {0.55f, 0, alpha * 0.8f}, {1, 0, 0}};
    ellipse(c, rx, ry, 0, 1, radial(c, rx, ry, s, 3));
  }
  void sphere(V2 c, float r, float lum) {
    const Stop s[3] = {{0, std::min(1.0f, lum * 1.35f)}, {0.6f, lum}, {1, lum * 0.5f}};
    ellipse(c, r, r, lum, 1, radial({c.x - r * 0.3f, c.y - r * 0.35f}, r * 1.3f, r * 1.3f, s, 3));
  }

  // --- world helpers (metres)
  void quad3(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float dx,
             float dy, float dz, float lum, float alpha = 1.0f, int paint = -1) {
    const V2 q[4] = {P(ax, ay, az), P(bx, by, bz), P(cx, cy, cz), P(dx, dy, dz)};
    poly(q, 4, lum, alpha, paint);
  }
  void floorEllipse(float x, float z, float rx, float rz, float lum, float alpha = 1.0f, int paint = -1) {
    const V2 c = P(x, 0.002f, z);
    const float y1 = P(x, 0.002f, z - rz).y, y2 = P(x, 0.002f, z + rz).y;
    ellipse({c.x, (y1 + y2) / 2}, scaleAt(z) * rx, std::fabs(y1 - y2) / 2, lum, alpha, paint);
  }
  void floorGlow(float x, float z, float rx, float rz, float alpha) {
    const V2 c = P(x, 0.002f, z);
    const float y1 = P(x, 0.002f, z - rz).y, y2 = P(x, 0.002f, z + rz).y;
    glow({c.x, (y1 + y2) / 2}, scaleAt(z) * rx, std::fabs(y1 - y2) / 2, alpha);
  }
  void shadowEllipse(float x, float z, float rx, float rz, float alpha = 0.5f) {
    const V2 c = P(x, 0, z);
    const float y1 = P(x, 0, z - rz).y, y2 = P(x, 0, z + rz).y;
    softShadow({c.x, (y1 + y2) / 2}, scaleAt(z) * rx * 1.25f, std::fabs(y1 - y2) / 2 * 1.4f + 2, alpha);
  }
  void box(float x0, float x1, float y0, float y1, float z0, float z1, float lum, bool shadow = true) {
    if (shadow && y0 <= 0.02f) {
      shadowEllipse((x0 + x1) / 2, (z0 + z1) / 2, (x1 - x0) / 2 + 0.1f, (z1 - z0) / 2 + 0.1f, 0.5f);
    }
    if (x0 > 0) quad3(x0, y0, z0, x0, y1, z0, x0, y1, z1, x0, y0, z1, lum * 0.62f);
    if (x1 < 0) quad3(x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1, lum * 0.62f);
    if (y1 < Hm) {
      quad3(x0, y1, z0, x1, y1, z0, x1, y1, z1, x0, y1, z1, std::min(1.0f, lum * 1.1f));
    } else if (y0 > Hm) {
      quad3(x0, y0, z0, x1, y0, z0, x1, y0, z1, x0, y0, z1, lum * 0.5f);
    }
    const V2 top = P(x0, y1, z0), bottom = P(x0, y0, z0);
    const int paint = vgrad(top.y, bottom.y, std::min(1.0f, lum * 0.95f), lum * 0.7f);
    quad3(x0, y0, z0, x1, y0, z0, x1, y1, z0, x0, y1, z0, lum * 0.86f, 1.0f, paint);
    // crisp edge so furniture reads after dithering
    const V2 a = P(x0, y1, z0), b = P(x1, y1, z0), c = P(x1, y0, z0), d = P(x0, y0, z0);
    const V2 e[5] = {a, b, c, d, a};
    polyline(e, 5, 0.8f, lum * 0.35f);
  }
  void cylinder(float x, float z, float r, float y0, float y1, float lum, float topLum = -1, bool shadow = true) {
    if (shadow && y0 <= 0.02f) shadowEllipse(x, z, r * 1.2f, r * 1.2f, 0.5f);
    const V2 b = P(x, y0, z), t = P(x, y1, z);
    const float rx = scaleAt(z) * r;
    const float ry0 = std::fabs(P(x, y0, z - r).y - P(x, y0, z + r).y) / 2;
    const float ry1 = std::fabs(P(x, y1, z - r).y - P(x, y1, z + r).y) / 2;
    V2 pts[26];
    int n = 0;
    // top half-ellipse (back edge hidden when looking down) from left to right
    for (int i = 0; i <= 12; ++i) {
      const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
      pts[n++] = {t.x - rx * std::cos(a), t.y + (y1 < Hm ? 1.0f : -1.0f) * ry1 * std::sin(a) * 0.0f};
    }
    n = 0;
    pts[n++] = {t.x - rx, t.y};
    pts[n++] = {t.x + rx, t.y};
    for (int i = 0; i <= 10; ++i) {
      const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 10.0f;
      pts[n++] = {b.x + rx * std::cos(a), b.y + ry0 * std::sin(a)};
    }
    poly(pts, n, lum, 1.0f, cylPaint(b.x - rx, b.x + rx, lum));
    if (y1 < Hm) {
      ellipse(t, rx, ry1, topLum >= 0 ? topLum : std::min(1.0f, lum * 1.1f));
    }
  }

  // --- billboards: local centimetres, origin at base centre, y up negative
  struct BB {
    V2 o;
    float s;  // px per cm
  };
  BB bb(float x, float y, float z) const { return {P(x, y, z), fpx_ / z / 100.0f}; }
  static V2 L(const BB& b, float lx, float ly) { return {b.o.x + lx * b.s, b.o.y + ly * b.s}; }
  void bbPoly(const BB& b, const V2* local, int n, float lum, float alpha = 1.0f, int paint = -1, float rotDeg = 0) {
    V2 pts[48];
    n = std::min(n, 48);
    const float ca = std::cos(rotDeg * static_cast<float>(M_PI) / 180.0f);
    const float sa = std::sin(rotDeg * static_cast<float>(M_PI) / 180.0f);
    for (int i = 0; i < n; ++i) {
      const float lx = local[i].x * ca - local[i].y * sa;
      const float ly = local[i].x * sa + local[i].y * ca;
      pts[i] = L(b, lx, ly);
    }
    poly(pts, n, lum, alpha, paint);
  }
  void bbEllipse(const BB& b, float lx, float ly, float rx, float ry, float lum, float alpha = 1.0f, int paint = -1) {
    ellipse(L(b, lx, ly), rx * b.s, ry * b.s, lum, alpha, paint);
  }
  void bbLine(const BB& b, float x0, float y0, float x1, float y1, float w, float lum, float alpha = 1.0f) {
    line(L(b, x0, y0), L(b, x1, y1), std::max(0.8f, w * b.s), lum, alpha);
  }
  // A leaf: a pointed lens from the base along `angle` (degrees, 0 = up).
  void bbLeaf(const BB& b, float angle, float len, float width, float lum, bool vein = true) {
    V2 pts[18];
    int n = 0;
    for (int i = 0; i <= 8; ++i) {
      const float t = static_cast<float>(i) / 8.0f;
      pts[n++] = {-width * std::sin(t * static_cast<float>(M_PI)) * (1.0f - t * 0.3f), -len * t};
    }
    for (int i = 7; i >= 1; --i) {
      const float t = static_cast<float>(i) / 8.0f;
      pts[n++] = {width * std::sin(t * static_cast<float>(M_PI)) * (1.0f - t * 0.3f), -len * t};
    }
    bbPoly(b, pts, n, lum, 1.0f, -1, angle);
    if (vein) {
      const float a = angle * static_cast<float>(M_PI) / 180.0f;
      bbLine(b, 0, 0, std::sin(a) * len * 0.85f, -std::cos(a) * len * 0.85f, 1.2f, std::min(1.0f, lum + 0.3f), 0.7f);
    }
  }
  // A flame of height h standing on (cx, baseY): a rounded base narrowing to a
  // slightly leaning tip.
  void bbFlame(const BB& b, float cx, float baseY, float h, float lum, float alpha = 1.0f) {
    static constexpr float SHAPE[13][2] = {
        {0, 0},       {-.26f, -.06f}, {-.36f, -.22f}, {-.32f, -.42f}, {-.2f, -.62f}, {-.06f, -.82f}, {.04f, -1.0f},
        {.08f, -.8f}, {.22f, -.64f},  {.34f, -.44f},  {.36f, -.24f},  {.28f, -.08f}, {.12f, -.01f}};
    V2 pts[13];
    for (int i = 0; i < 13; ++i) pts[i] = {cx + SHAPE[i][0] * h, baseY + SHAPE[i][1] * h};
    bbPoly(b, pts, 13, lum, alpha);
    V2 core[13];
    for (int i = 0; i < 13; ++i) core[i] = {cx + SHAPE[i][0] * h * .45f, baseY + SHAPE[i][1] * h * .55f};
    bbPoly(b, core, 13, std::max(0.0f, lum - .25f), alpha * .6f);
  }

  // --- room composition
  void build();

  SleepRoomRenderer& r_;
  const SleepRoomSpec& spec_;
  ShelfInfo shelf_;
  bool taken_[SLOT_COUNT]{};
  bool shelfLeftUsed_ = false;
  bool shelfRightUsed_ = false;
  float bedTop_ = 0.12f;

 private:
  static uint8_t toLum(float v) { return static_cast<uint8_t>(clampi(static_cast<int>(v * 255.0f + 0.5f), 0, 255)); }
  static int16_t q4(float v) {
    return static_cast<int16_t>(clampi(static_cast<int>(std::lround(v * 4.0f)), -32000, 32000));
  }
  void setClip(SleepRoomRenderer::Prim& p) const {
    p.clipX0 = clipX0_;
    p.clipX1 = clipX1_;
    p.clipY0 = clipY0_;
    p.clipY1 = clipY1_;
  }
  int addPaint(uint8_t kind, float x0, float y0, float x1, float y1, const Stop* stops, int n) {
    auto& r = r_;
    if (r.paintCount_ >= SleepRoomRenderer::MAX_PAINTS) {
      r.overflow_ = true;
      return -1;
    }
    SleepRoomRenderer::Paint& p = r.paints_[r.paintCount_];
    p = {};
    p.kind = kind;
    p.stops = static_cast<uint8_t>(std::min(n, 4));
    p.x0 = q4(x0);
    p.y0 = q4(y0);
    p.x1 = q4(x1);
    p.y1 = q4(y1);
    if (kind == 1) {
      const int64_t dx = p.x1 - p.x0, dy = p.y1 - p.y0;
      p.len2 = std::max<int64_t>(1, dx * dx + dy * dy);
    } else {
      p.x1 = std::max<int32_t>(1, p.x1);
      p.y1 = std::max<int32_t>(1, p.y1);
    }
    for (int i = 0; i < p.stops; ++i) {
      p.pos[i] = toLum(stops[i].pos);
      p.lum[i] = toLum(stops[i].lum);
      p.alpha[i] = toLum(stops[i].alpha);
    }
    return static_cast<int>(r.paintCount_++);
  }

  float vx_;
  float vyf_;
  float fpx_;
  uint32_t rng_;
  int16_t key_ = 0;
  int16_t clipX0_ = 0, clipX1_ = 0, clipY0_ = 0, clipY1_ = 0;
};

// ---------------------------------------------------------------------------
// Furniture library

namespace {

using B = SleepRoomBuilder;

float slotX(Slot s) { return s == FloorLeft ? -2.25f : 2.25f; }
constexpr float SMALL_Z = 5.2f;

// Window view content, local coordinates 0..200 x -200..0 (y up negative).
struct ViewMap {
  float x, y, w, h;
  V2 operator()(float lx, float ly) const { return {x + lx * w / 200.0f, y + (200.0f + ly) * h / 200.0f}; }
};

void viewPoly(B& b, const ViewMap& m, const float* xy, int n, float lum, float alpha = 1.0f) {
  V2 pts[24];
  n = std::min(n, 24);
  for (int i = 0; i < n; ++i) pts[i] = m(xy[i * 2], xy[i * 2 + 1]);
  b.poly(pts, n, lum, alpha);
}
void viewEllipse(B& b, const ViewMap& m, float cx, float cy, float rx, float ry, float lum, float alpha = 1.0f) {
  b.ellipse(m(cx, cy), rx * m.w / 200.0f, ry * m.h / 200.0f, lum, alpha);
}
void viewRect(B& b, const ViewMap& m, float x, float y, float w, float h, float lum) {
  const float q[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
  viewPoly(b, m, q, 4, lum);
}
void viewSky(B& b, const ViewMap& m, float top, float bottom) {
  const float q[8] = {0, -200, 200, -200, 200, 0, 0, 0};
  V2 pts[4];
  for (int i = 0; i < 4; ++i) pts[i] = m(q[i * 2], q[i * 2 + 1]);
  b.poly(pts, 4, top, 1.0f, b.vgrad(m(0, -200).y, m(0, 0).y, top, bottom));
}
void viewCloud(B& b, const ViewMap& m, float cx, float cy, float s) {
  viewEllipse(b, m, cx, cy, 34 * s, 12 * s, 0.97f);
  viewEllipse(b, m, cx + 16 * s, cy - 8 * s, 22 * s, 12 * s, 1.0f);
  viewEllipse(b, m, cx - 18 * s, cy - 4 * s, 16 * s, 9 * s, 0.98f);
}

void drawView(B& b, const ViewMap& m, PokemonType type) {
  switch (type) {
    case PokemonType::Fire: {
      viewSky(b, m, 0.6f, 0.94f);
      viewEllipse(b, m, 100, -150, 30, 14, 0.6f, 0.9f);
      viewEllipse(b, m, 116, -172, 24, 10, 0.67f, 0.9f);
      const float volcano[12] = {0, -40, 60, -40, 90, -120, 110, -120, 140, -40, 200, -40};
      viewPoly(b, m, volcano, 6, 0.33f);
      const float lava[8] = {90, -120, 110, -120, 104, -98, 96, -98};
      viewPoly(b, m, lava, 4, 1.0f);
      viewRect(b, m, 0, -40, 200, 40, 0.23f);
      break;
    }
    case PokemonType::Water: {
      viewSky(b, m, 0.6f, 0.94f);
      viewEllipse(b, m, 140, -110, 18, 18, 0.96f);
      const float sea[8] = {0, -80, 200, -80, 200, 0, 0, 0};
      V2 pts[4];
      for (int i = 0; i < 4; ++i) pts[i] = m(sea[i * 2], sea[i * 2 + 1]);
      b.poly(pts, 4, 0.4f, 1.0f, b.vgrad(m(0, -80).y, m(0, 0).y, 0.47f, 0.2f));
      const float waves[12] = {20, -60, 90, -50, 150, -66, 50, -30, 130, -20, 10, -15};
      for (int i = 0; i < 6; ++i) {
        b.line(m(waves[i * 2], waves[i * 2 + 1]), m(waves[i * 2] + 20, waves[i * 2 + 1]), 1.5f, 0.93f);
      }
      break;
    }
    case PokemonType::Grass:
    case PokemonType::Bug: {
      viewSky(b, m, 0.6f, 0.94f);
      viewEllipse(b, m, 50, 0, 120, 60, 0.47f);
      viewEllipse(b, m, 170, 0, 110, 45, 0.4f);
      viewRect(b, m, 40, -110, 8, 60, 0.2f);
      viewEllipse(b, m, 44, -120, 30, 30, 0.27f);
      viewEllipse(b, m, 30, -105, 20, 20, 0.33f);
      viewEllipse(b, m, 150, -62, 16, 16, 0.29f);
      if (type == PokemonType::Bug) {
        viewEllipse(b, m, 120, -140, 4, 4, 1.0f);
        viewEllipse(b, m, 90, -120, 3, 3, 1.0f);
      }
      break;
    }
    case PokemonType::Electric: {
      viewRect(b, m, 0, -200, 200, 200, 0.18f);
      static constexpr float bld[18] = {0, 30, 70, 32, 24, 110, 60, 40, 60, 104, 22, 130, 130, 34, 80, 168, 32, 100};
      for (int i = 0; i < 6; ++i) viewRect(b, m, bld[i * 3], -bld[i * 3 + 2], bld[i * 3 + 1], bld[i * 3 + 2], 0.29f);
      static constexpr float win[18] = {38,  -95, 46,  -80, 110, -120, 116, -100, 110,
                                        -70, 140, -60, 175, -90, 185,  -70, 70,   -45};
      for (int i = 0; i < 9; ++i) viewRect(b, m, win[i * 2], win[i * 2 + 1], 4, 5, 0.87f);
      const V2 bolt[4] = {m(120, -200), m(104, -160), m(116, -160), m(96, -120)};
      b.polyline(bolt, 4, 2.0f, 1.0f);
      break;
    }
    case PokemonType::Poison:
    case PokemonType::Ghost: {
      viewRect(b, m, 0, -200, 200, 200, type == PokemonType::Ghost ? 0.25f : 0.42f);
      if (type == PokemonType::Ghost) {
        viewEllipse(b, m, 150, -150, 18, 18, 0.95f);
        viewEllipse(b, m, 158, -156, 16, 16, 0.25f);
      }
      viewRect(b, m, 0, -40, 200, 40, 0.24f);
      const V2 t1[3] = {m(24, -40), m(26, -110), m(32, -40)};
      b.poly(t1, 3, 0.15f);
      const V2 t2[3] = {m(150, -40), m(154, -100), m(160, -40)};
      b.poly(t2, 3, 0.15f);
      b.line(m(26, -90), m(10, -105), 2, 0.15f);
      b.line(m(26, -80), m(44, -98), 2, 0.15f);
      viewEllipse(b, m, 100, -50, 110, 20, 0.6f, 0.5f);
      break;
    }
    case PokemonType::Ice: {
      viewSky(b, m, 0.55f, 0.9f);
      const float mtn[10] = {0, -30, 60, -140, 110, -60, 160, -120, 200, -30};
      viewPoly(b, m, mtn, 5, 0.75f);
      const float snow1[8] = {60, -140, 74, -110, 60, -116, 46, -108};
      viewPoly(b, m, snow1, 4, 1.0f);
      viewRect(b, m, 0, -30, 200, 30, 0.95f);
      for (int i = 0; i < 9; ++i) viewEllipse(b, m, 15.0f + i * 21, -170.0f + (i % 3) * 40, 2, 2, 1.0f);
      break;
    }
    case PokemonType::Ground:
    case PokemonType::Fighting: {
      viewSky(b, m, 0.62f, 0.95f);
      viewEllipse(b, m, 150, -150, 16, 16, 1.0f);
      viewEllipse(b, m, 40, 0, 140, 50, type == PokemonType::Ground ? 0.72f : 0.45f);
      viewEllipse(b, m, 170, 0, 120, 70, type == PokemonType::Ground ? 0.62f : 0.35f);
      if (type == PokemonType::Ground) {
        viewRect(b, m, 120, -95, 8, 50, 0.3f);
        viewRect(b, m, 110, -80, 10, 6, 0.3f);
        viewRect(b, m, 110, -90, 4, 14, 0.3f);
      }
      break;
    }
    case PokemonType::Psychic: {
      viewRect(b, m, 0, -200, 200, 200, 0.15f);
      viewEllipse(b, m, 140, -140, 22, 22, 0.92f);
      for (int i = 0; i < 14; ++i) {
        viewEllipse(b, m, 10.0f + (i * 37) % 190, -190.0f + (i * 53) % 150, 1.6f, 1.6f, 1.0f);
      }
      viewRect(b, m, 0, -30, 200, 30, 0.22f);
      break;
    }
    case PokemonType::Rock:
    case PokemonType::Dragon: {
      viewSky(b, m, 0.58f, 0.92f);
      const float cliff[12] = {0, 0, 0, -130, 40, -150, 70, -90, 120, -60, 200, -40};
      viewPoly(b, m, cliff, 6, 0.4f);
      viewRect(b, m, 0, -40, 200, 40, 0.38f);
      if (type == PokemonType::Dragon) {
        viewRect(b, m, 140, -110, 30, 60, 0.3f);
        const float roof[6] = {136, -110, 155, -140, 174, -110};
        viewPoly(b, m, roof, 3, 0.25f);
        viewCloud(b, m, 60, -170, 0.8f);
      }
      break;
    }
    case PokemonType::Flying: {
      viewSky(b, m, 0.6f, 0.94f);
      viewCloud(b, m, 60, -125, 1.2f);
      viewCloud(b, m, 150, -60, 1.0f);
      const V2 bird[3] = {m(120, -150), m(126, -156), m(132, -150)};
      b.polyline(bird, 3, 1.5f, 0.2f);
      const V2 bird2[3] = {m(132, -150), m(138, -156), m(144, -150)};
      b.polyline(bird2, 3, 1.5f, 0.2f);
      break;
    }
    default: {  // Normal: meadow
      viewSky(b, m, 0.62f, 0.95f);
      viewCloud(b, m, 60, -140, 1.0f);
      viewEllipse(b, m, 60, 0, 140, 50, 0.62f);
      viewEllipse(b, m, 180, 0, 110, 40, 0.55f);
      for (int i = 0; i < 6; ++i) viewEllipse(b, m, 20.0f + i * 30, -12.0f - (i % 2) * 6, 3, 3, 0.95f);
      break;
    }
  }
}

void backWindow(B& b, PokemonType view, bool curtains) {
  const float x0 = -1.0f, x1 = 1.0f, y0 = 1.4f, y1 = 3.4f;
  b.key(ZBm);
  const V2 tl = b.P(x0, y1, ZBm), br = b.P(x1, y0, ZBm);
  b.quad3(x0 - .12f, y0 - .12f, ZBm, x1 + .12f, y0 - .12f, ZBm, x1 + .12f, y1 + .12f, ZBm, x0 - .12f, y1 + .12f, ZBm,
          0.92f);
  b.clip(tl.x, tl.y, br.x, br.y);
  drawView(b, {tl.x, tl.y, br.x - tl.x, br.y - tl.y}, view);
  b.resetClip();
  const float w = br.x - tl.x, h = br.y - tl.y;
  const V2 frame[5] = {tl, {br.x, tl.y}, br, {tl.x, br.y}, tl};
  b.polyline(frame, 5, 4, 0.95f);
  b.line({tl.x + w / 2, tl.y}, {tl.x + w / 2, br.y}, 3, 0.95f);
  b.line({tl.x, tl.y + h / 2}, {br.x, tl.y + h / 2}, 3, 0.95f);
  const V2 glare[4] = {
      {tl.x + 6, br.y - 4}, {tl.x + 22, br.y - 4}, {tl.x + w * .55f, tl.y + 4}, {tl.x + w * .4f, tl.y + 4}};
  b.poly(glare, 4, 1.0f, 0.25f);
  b.box(x0 - .2f, x1 + .2f, y0 - .14f, y0 - .04f, ZBm - .25f, ZBm, 0.85f, false);
  if (curtains) {
    for (int side = -1; side <= 1; side += 2) {
      const float xa = side < 0 ? x0 - .45f : x1 + .05f;
      const float xb = xa + .4f;
      const V2 q[4] = {b.P(xa, y1 + .35f, ZBm - .05f), b.P(xb, y1 + .35f, ZBm - .05f),
                       b.P(xb - .05f * static_cast<float>(side), y0 - .4f, ZBm - .05f), b.P(xa, y0 - .4f, ZBm - .05f)};
      const Stop s[4] = {{0, 0.33f}, {0.3f, 0.6f}, {0.6f, 0.33f}, {1, 0.55f}};
      b.poly(q, 4, 0.45f, 1.0f, b.linear(q[0], q[1], s, 4));
    }
    b.box(x0 - .6f, x1 + .6f, y1 + .35f, y1 + .42f, ZBm - .15f, ZBm, 0.3f, false);
  }
}

void picture(B& b, PokemonType view, float x0 = -2.95f, float x1 = -1.65f, float y0 = 2.0f, float y1 = 3.0f) {
  b.key(ZBm - .02f);
  const V2 tl = b.P(x0, y1, ZBm), br = b.P(x1, y0, ZBm);
  const V2 sh[4] = {{tl.x + 3, tl.y + 4}, {br.x + 3, tl.y + 4}, {br.x + 3, br.y + 4}, {tl.x + 3, br.y + 4}};
  b.poly(sh, 4, 0, 0.3f);
  const V2 fr[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
  b.poly(fr, 4, 0.35f, 1.0f, b.vgrad(tl.y, br.y, 0.55f, 0.28f));
  b.clip(tl.x + 4, tl.y + 4, br.x - 4, br.y - 4);
  drawView(b, {tl.x + 4, tl.y + 4, br.x - tl.x - 8, br.y - tl.y - 8}, view);
  b.resetClip();
}

void sideFrame(B& b, Slot slot, float z0, float z1, float y0, float y1, float lum) {
  const float x = slot == SideLeft ? -Wm + .02f : Wm - .02f;
  b.key((z0 + z1) / 2);
  const float shift = slot == SideLeft ? 3.0f : -3.0f;
  const V2 q[4] = {b.P(x, y0, z0), b.P(x, y0, z1), b.P(x, y1, z1), b.P(x, y1, z0)};
  const V2 s[4] = {{q[0].x + shift, q[0].y + 3},
                   {q[1].x + shift, q[1].y + 3},
                   {q[2].x + shift, q[2].y + 3},
                   {q[3].x + shift, q[3].y + 3}};
  b.poly(s, 4, 0, 0.35f);
  b.poly(q, 4, lum);
  const V2 e[5] = {q[0], q[1], q[2], q[3], q[0]};
  b.polyline(e, 5, 1, 0.07f);
}

// ---- Electric
void elBattery(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .32f, 0, .75f, .5f, .7f);
  b.cylinder(x, z, .12f, .75f, .85f, .75f, -1, false);
  b.quad3(x - .3f, .55f, z - .31f, x + .3f, .55f, z - .31f, x + .3f, .62f, z - .31f, x - .3f, .62f, z - .31f, .2f);
  const auto bb = b.bb(x, .38f, z - .33f);
  const V2 bolt[6] = {{4, -24}, {-6, -6}, {0, -6}, {-4, 10}, {8, -10}, {2, -10}};
  b.bbPoly(bb, bolt, 6, 1.0f);
}
void elPoster(B& b, Slot s) {
  if (s == WallLeft) {
    b.key(ZBm - .02f);
    const V2 tl = b.P(-2.8f, 3.1f, ZBm), br = b.P(-1.8f, 1.7f, ZBm);
    const float w = br.x - tl.x, h = br.y - tl.y;
    const V2 sh[4] = {{tl.x + 2, tl.y + 2}, {br.x + 2, tl.y + 2}, {br.x + 2, br.y + 2}, {tl.x + 2, br.y + 2}};
    b.poly(sh, 4, 0, .3f);
    const V2 q[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    b.poly(q, 4, .9f, 1, b.vgrad(tl.y, br.y, .95f, .8f));
    const V2 bolt[6] = {{tl.x + w * .62f, tl.y + h * .1f},  {tl.x + w * .25f, tl.y + h * .55f},
                        {tl.x + w * .48f, tl.y + h * .55f}, {tl.x + w * .36f, tl.y + h * .92f},
                        {tl.x + w * .78f, tl.y + h * .42f}, {tl.x + w * .54f, tl.y + h * .42f}};
    b.poly(bolt, 6, .13f);
  } else {
    sideFrame(b, s, 5.8f, 7.2f, 1.4f, 3.1f, .2f);
    const float x = s == SideLeft ? -Wm + .03f : Wm - .03f;
    const V2 bolt[6] = {b.P(x, 2.9f, 6.1f), b.P(x, 2.1f, 6.6f),   b.P(x, 2.45f, 6.6f),
                        b.P(x, 1.6f, 7.0f), b.P(x, 2.35f, 6.45f), b.P(x, 2.0f, 6.45f)};
    b.poly(bolt, 6, 1.0f);
  }
}
void elPlasma(B& b, const ShelfInfo& sh, bool left) {
  const float x = left ? sh.x0 + .35f : sh.x1 - .4f, z = ZBm - .18f;
  b.key(z);
  b.cylinder(x, z, .1f, sh.y, sh.y + .12f, .25f, -1, false);
  const V2 c = b.P(x, sh.y + .32f, z);
  const float r = b.scaleAt(z) * .22f;
  const Stop st[2] = {{0, .6f}, {1, .15f}};
  b.ellipse(c, r, r, .4f, 1, b.radial(c, r, r, st, 2));
  for (int k = 0; k < 7; ++k) {
    const float a = static_cast<float>(k) * 0.9f + b.rand(0, .5f);
    b.line(c, {c.x + r * .95f * std::cos(a), c.y + r * .95f * std::sin(a)}, .8f, 1.0f);
  }
  b.ellipse(c, 2.2f, 2.2f, 1.0f);
}
void elTv(B& b, Slot s) {
  const float x0 = s == BigLeft ? -3.0f : 1.5f, x1 = x0 + 1.5f, z0 = ZBm - .5f;
  b.key(ZBm - .25f);
  b.box(x0, x1, 0, .55f, z0, ZBm, .38f);
  b.box(x0 + .1f, x1 - .1f, .6f, 1.5f, ZBm - .3f, ZBm - .2f, .12f, false);
  const V2 tl = b.P(x0 + .16f, 1.44f, ZBm - .3f), br = b.P(x1 - .16f, .66f, ZBm - .3f);
  const V2 q[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
  b.poly(q, 4, .35f, 1, b.vgrad(tl.y, br.y, .5f, .2f));
  const V2 gl[3] = {tl, {tl.x + (br.x - tl.x) * .5f, tl.y}, {tl.x, br.y}};
  b.poly(gl, 3, 1, .18f);
  b.cylinder((x0 + x1) / 2, z0 + .25f, .05f, .55f, .6f, .2f, -1, false);
}
void elTesla(B& b, Slot s) {
  const float x = slotX(s), z = 5.3f;
  b.key(z);
  b.cylinder(x, z, .28f, 0, .25f, .3f, .45f);
  b.cylinder(x, z, .1f, .25f, 1.3f, .75f, -1, false);
  for (int k = 0; k < 6; ++k) {
    b.cylinder(x, z, .11f, .35f + k * .15f, .37f + k * .15f, .4f, -1, false);
  }
  const V2 c = b.P(x, 1.45f, z);
  const float r = b.scaleAt(z) * .22f;
  b.ellipse(c, r, r * .45f, .75f, 1, b.cylPaint(c.x - r, c.x + r, .75f));
  const V2 arc[4] = {{c.x + r, c.y}, {c.x + r + 10, c.y - 8}, {c.x + r + 6, c.y - 10}, {c.x + r + 16, c.y - 20}};
  b.polyline(arc, 4, 1.4f, 1.0f);
  b.glow({c.x + 10, c.y - 10}, 26, 26, .35f);
}
void elNeon(B& b, Slot s) {
  const float x = s == SideLeft ? -Wm + .02f : Wm - .02f;
  b.key(6.2f);
  const V2 p[4] = {b.P(x, 3.2f, 5.4f), b.P(x, 2.5f, 6.2f), b.P(x, 2.75f, 6.2f), b.P(x, 1.9f, 7.0f)};
  for (int i = 0; i < 3; ++i) {
    const V2 mid = {(p[i].x + p[i + 1].x) / 2, (p[i].y + p[i + 1].y) / 2};
    b.glow(mid, 16, 18, .3f);
  }
  b.polyline(p, 4, 2.4f, 1.0f);
}

// ---- Water
void waShellLamp(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .25f, 0, .08f, .35f, .5f);
  b.cylinder(x, z, .03f, .08f, 1.4f, .3f, -1, false);
  const V2 c = b.P(x, 1.4f, z);
  b.glow({c.x, c.y - 10}, 40, 40, .4f);
  const auto bb = b.bb(x, 1.4f, z);
  V2 shell[14];
  for (int i = 0; i <= 12; ++i) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
    shell[i] = {-60 * std::cos(a), -60 * std::sin(a)};
  }
  const V2 cTop = B::L(bb, 0, -60), cBot = B::L(bb, 0, 0);
  b.bbPoly(bb, shell, 13, .9f, 1, b.vgrad(cTop.y, cBot.y, .95f, .7f));
  for (int k = 1; k < 8; ++k) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(k) / 8.0f;
    b.bbLine(bb, 0, 0, 58 * std::cos(a), -58 * std::sin(a), 2, .33f);
  }
}
void waFishbowl(B& b, const ShelfInfo& sh, bool left) {
  const float x = left ? sh.x0 + .45f : sh.x1 - .45f, z = ZBm - .18f;
  b.key(z);
  const V2 c = b.P(x, sh.y + .28f, z);
  const float r = b.scaleAt(z) * .27f;
  b.softShadow({c.x, b.P(x, sh.y, z).y}, r, 3, .4f);
  b.ellipse(c, r, r, .93f);
  b.clip(c.x - r, c.y - r * .35f, c.x + r, c.y + r);
  b.ellipse(c, r, r, .55f, 1, b.vgrad(c.y - r * .35f, c.y + r, .7f, .4f));
  b.resetClip();
  const V2 fish[4] = {{c.x - 5, c.y + 2}, {c.x, c.y - 2}, {c.x + 4, c.y + 2}, {c.x, c.y + 5}};
  b.poly(fish, 4, .07f);
  b.ellipse({c.x - r * .4f, c.y - r * .45f}, r * .2f, r * .12f, 1, .8f);
}
void waPorthole(B& b, Slot s) {
  const float x = s == SideLeft ? -Wm + .02f : Wm - .02f;
  const float zc = 6.6f, yc = 2.4f, rr = .55f;
  b.key(zc);
  V2 ring[24], inner[24];
  for (int i = 0; i < 24; ++i) {
    const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 12.0f;
    ring[i] = b.P(x, yc + rr * std::sin(a), zc + rr * std::cos(a));
    inner[i] = b.P(x, yc + rr * .78f * std::sin(a), zc + rr * .78f * std::cos(a));
  }
  b.poly(ring, 24, .55f);
  b.poly(inner, 24, .35f, 1, b.vgrad(inner[18].y, inner[6].y, .47f, .2f));
  for (int k = 0; k < 8; ++k) {
    const float a = static_cast<float>(k) * static_cast<float>(M_PI) / 4.0f;
    b.ellipse(b.P(x, yc + rr * .9f * std::sin(a), zc + rr * .9f * std::cos(a)), 1.3f, 1.3f, .07f);
  }
  b.ellipse(b.P(x, yc - .1f, zc + .1f), 2.5f, 2.5f, 1, .8f);
  b.ellipse(b.P(x, yc + .15f, zc - .05f), 1.8f, 1.8f, 1, .8f);
}
void waLifebuoy(B& b, Slot s) {
  V2 c;
  float r, sx = 1.0f;
  if (s == WallLeft) {
    b.key(ZBm - .02f);
    c = b.P(-2.3f, 2.5f, ZBm);
    r = b.scaleAt(ZBm) * .42f;
  } else {
    b.key(6.8f);
    c = b.P(Wm - .02f, 2.5f, 6.8f);
    r = b.scaleAt(6.8f) * .42f;
    sx = .55f;
  }
  V2 outer[24], inner[24];
  for (int i = 0; i < 24; ++i) {
    const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 12.0f;
    outer[i] = {c.x + r * 1.25f * std::cos(a) * sx, c.y + r * 1.25f * std::sin(a)};
    inner[i] = {c.x + r * .75f * std::cos(a) * sx, c.y + r * .75f * std::sin(a)};
  }
  b.ellipse({c.x + 2, c.y + 3}, r * 1.25f * sx, r * 1.25f, 0, .3f);
  b.poly(outer, 24, .93f);
  for (int q = 0; q < 4; ++q) {
    const float a0 = static_cast<float>(q) * static_cast<float>(M_PI) / 2.0f + .35f;
    V2 seg[8];
    for (int i = 0; i < 4; ++i) {
      const float a = a0 + static_cast<float>(i) * .26f;
      seg[i] = {c.x + r * 1.25f * std::cos(a) * sx, c.y + r * 1.25f * std::sin(a)};
      seg[7 - i] = {c.x + r * .75f * std::cos(a) * sx, c.y + r * .75f * std::sin(a)};
    }
    b.poly(seg, 8, .27f);
  }
  b.poly(inner, 24, .55f);
}
void waAquarium(B& b, Slot s) {
  const float x0 = s == BigLeft ? -3.0f : 1.4f, x1 = x0 + 1.6f, z0 = ZBm - .6f;
  b.key(ZBm - .3f);
  b.box(x0, x1, 0, .7f, z0, ZBm, .3f);
  b.quad3(x0, 1.62f, z0, x1, 1.62f, z0, x1, 1.62f, ZBm, x0, 1.62f, ZBm, .25f);
  const V2 tl = b.P(x0 + .03f, 1.6f, z0), br = b.P(x1 - .03f, .72f, z0);
  const float w = br.x - tl.x, h = br.y - tl.y;
  const V2 q[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
  b.poly(q, 4, .45f, 1, b.vgrad(tl.y, br.y, .6f, .25f));
  V2 sand[11];
  sand[0] = {tl.x, br.y};
  for (int k = 0; k <= 8; ++k) sand[k + 1] = {tl.x + w * static_cast<float>(k) / 8.0f, br.y - b.rand(3, 8)};
  sand[10] = {br.x, br.y};
  b.poly(sand, 11, .33f);
  for (int k = 0; k < 4; ++k) {
    const float bx = tl.x + w * (.15f + .22f * static_cast<float>(k));
    const V2 weed[4] = {{bx, br.y - 3}, {bx - 5, br.y - 18}, {bx + 2, br.y - 32}, {bx - 2, br.y - 50}};
    b.polyline(weed, 4, 2.4f, .2f);
  }
  const float fishes[6] = {.35f, .4f, .65f, .62f, .5f, .25f};
  for (int k = 0; k < 3; ++k) {
    const float px = tl.x + w * fishes[k * 2], py = tl.y + h * fishes[k * 2 + 1];
    const float d = k == 1 ? -1.0f : 1.0f;
    const V2 f[6] = {{px, py},
                     {px + 6 * d, py - 4},
                     {px + 12 * d, py},
                     {px + 6 * d, py + 4},
                     {px - 4 * d, py - 4},
                     {px - 4 * d, py + 4}};
    b.poly(f, 4, .1f);
    const V2 tail[3] = {f[0], f[4], f[5]};
    b.poly(tail, 3, .1f);
  }
  for (int k = 0; k < 5; ++k) b.ellipse({tl.x + w * .78f, tl.y + h * (.15f + k * .15f)}, 1.4f, 1.4f, 1, .7f);
  const V2 gl[4] = {{tl.x + 4, br.y}, {tl.x + 14, br.y}, {tl.x + w * .45f, tl.y}, {tl.x + w * .35f, tl.y}};
  b.poly(gl, 4, 1, .2f);
  const V2 fr[5] = {tl, {br.x, tl.y}, br, {tl.x, br.y}, tl};
  b.polyline(fr, 5, 1.5f, .15f);
}
void waCoral(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .28f, 0, .45f, .6f, .3f);
  const auto bb = b.bb(x, .45f, z);
  const float br[16] = {-14, 90, .87f, 9, 6, 110, .73f, 8, 16, 70, .93f, 7, -4, 60, .6f, 6};
  for (int k = 0; k < 4; ++k) {
    const float a = br[k * 4], h = br[k * 4 + 1];
    const V2 p[4] = {{0, 0}, {a, -h * .4f}, {a * 2, -h * .7f}, {a * 2 + 10, -h}};
    V2 sp[4];
    for (int i = 0; i < 4; ++i) sp[i] = B::L(bb, p[i].x, p[i].y);
    b.polyline(sp, 4, br[k * 4 + 3] * bb.s, br[k * 4 + 2]);
  }
}

// ---- Fire
void fiCandles(B& b, Slot s, const ShelfInfo* sh, bool left) {
  if (sh != nullptr) {
    const float z = ZBm - .18f;
    b.key(z);
    const float base = left ? sh->x0 + .25f : sh->x1 - .65f;
    const float hh[3] = {.35f, .5f, .28f};
    for (int i = 0; i < 3; ++i) {
      const float x = base + i * .2f;
      b.cylinder(x, z, .06f, sh->y, sh->y + hh[i], .92f, -1, false);
      const V2 c = b.P(x, sh->y + hh[i], z);
      b.glow({c.x, c.y - 4}, 14, 14, .5f);
      const auto bb = b.bb(x, sh->y + hh[i], z);
      b.bbFlame(bb, 0, -2, 14, 1.0f);
    }
    return;
  }
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .25f, 0, 1.1f, .25f, .4f);
  const float dx[2] = {-.1f, .1f}, hh[2] = {.35f, .25f};
  for (int i = 0; i < 2; ++i) {
    b.cylinder(x + dx[i], z, .05f, 1.1f, 1.1f + hh[i], .92f, -1, false);
    const V2 c = b.P(x + dx[i], 1.1f + hh[i], z);
    b.glow({c.x, c.y - 4}, 16, 16, .5f);
    b.bbFlame(b.bb(x + dx[i], 1.1f + hh[i], z), 0, -2, 14, 1.0f);
  }
}
void fiFireplace(B& b, Slot s) {
  const float x0 = s == BigLeft ? -3.05f : 1.35f, x1 = x0 + 1.7f, z0 = ZBm - .45f;
  b.key(ZBm - .2f);
  b.box(x0, x1, 0, 1.55f, z0, ZBm, .8f);
  for (int k = 0; k < 10; ++k) {
    const float yy = b.rand(.1f, 1.45f), xx = b.rand(x0 + .05f, x1 - .3f);
    b.quad3(xx, yy, z0 - .001f, xx + .25f, yy, z0 - .001f, xx + .25f, yy + .12f, z0 - .001f, xx, yy + .12f, z0 - .001f,
            b.rand(.45f, .7f), .7f);
  }
  const V2 a = b.P(x0 + .3f, 0, z0), c = b.P(x1 - .3f, 1.0f, z0);
  const float w = c.x - a.x, h = a.y - c.y;
  V2 arch[16];
  int n = 0;
  arch[n++] = {a.x, a.y};
  for (int i = 0; i <= 12; ++i) {
    const float t = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
    arch[n++] = {a.x + w / 2 - w / 2 * std::cos(t), c.y + w / 2 - w / 2 * std::sin(t)};
  }
  arch[n++] = {c.x, a.y};
  b.poly(arch, n, .05f);
  const float cx = (a.x + c.x) / 2;
  b.glow({cx, a.y - h * .3f}, w * .7f, h * .6f, .75f);
  const auto bb = b.bb((x0 + x1) / 2, 0.05f, z0);
  b.bbFlame(bb, -14, 0, 36, 1.0f);
  b.bbFlame(bb, 6, 0, 46, 1.0f);
  b.bbFlame(bb, 20, 0, 30, .95f);
  const V2 lg[4] = {
      {cx - w * .32f, a.y - 6}, {cx + w * .32f, a.y - 6}, {cx + w * .32f, a.y - 1}, {cx - w * .32f, a.y - 1}};
  b.poly(lg, 4, .2f);
  b.box(x0 - .12f, x1 + .12f, 1.55f, 1.7f, z0 - .12f, ZBm, .5f, false);
  b.key(30.0f);
  b.floorGlow((x0 + x1) / 2, z0 - .7f, 1.3f, .8f, .35f);
}
void fiVolcano(B& b, Slot) { picture(b, PokemonType::Fire); }
void fiBrazier(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .06f, 0, .8f, .25f);
  b.cylinder(x, z, .35f, .8f, 1.0f, .3f, .2f, false);
  const V2 c = b.P(x, 1.0f, z);
  b.glow({c.x, c.y - 8}, 30, 30, .5f);
  const auto bb = b.bb(x, 1.0f, z);
  b.bbFlame(bb, -10, 0, 40, 1.0f);
  b.bbFlame(bb, 8, 0, 55, 1.0f);
}
void fiSconces(B& b, Slot s) {
  const float x = s == SideLeft ? -Wm + .02f : Wm - .02f;
  const float sgn = s == SideLeft ? 1.0f : -1.0f;
  b.key(6.2f);
  for (const float zc : {5.4f, 6.9f}) {
    const V2 c = b.P(x, 2.6f, zc);
    b.glow({c.x, c.y - 6}, 34, 34, .45f);
    const V2 arm[4] = {{c.x, c.y + 6}, {c.x + 8 * sgn, c.y + 2}, {c.x + 8 * sgn, c.y - 12}, {c.x + 5 * sgn, c.y - 12}};
    b.poly(arm, 4, .13f);
    const V2 f[5] = {{c.x + 7 * sgn, c.y - 12},
                     {c.x + 3 * sgn, c.y - 18},
                     {c.x + 7 * sgn, c.y - 27},
                     {c.x + 11 * sgn, c.y - 18},
                     {c.x + 7 * sgn, c.y - 12}};
    b.poly(f, 5, 1.0f);
  }
}

// ---- Flying
void flFeathers(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .22f, 0, .6f, .85f, .2f);
  const auto bb = b.bb(x, .6f, z);
  const float fe[8] = {-18, 110, -4, 140};
  const float fe2[4] = {12, 120, 24, 90};
  for (int k = 0; k < 4; ++k) {
    const float ang = k < 2 ? fe[k * 2] : fe2[(k - 2) * 2];
    const float ln = k < 2 ? fe[k * 2 + 1] : fe2[(k - 2) * 2 + 1];
    const float a = ang * static_cast<float>(M_PI) / 180.0f;
    b.bbLine(bb, 0, 0, std::sin(a) * ln, -std::cos(a) * ln, 2, .13f);
    const auto tip = B::BB{B::L(bb, std::sin(a) * ln * .35f, -std::cos(a) * ln * .35f), bb.s};
    b.bbLeaf(tip, ang, ln * .68f, 13, .93f, false);
  }
}
void flChimes(B& b, Slot) {
  const float x = -1.6f, z = 4.6f;
  b.key(z);
  const V2 c = b.P(x, 3.3f, z), top = b.P(x, CHm, z);
  b.line(top, c, 1, .13f);
  b.ellipse(c, 16, 4, .6f, 1, b.cylPaint(c.x - 16, c.x + 16, .6f));
  const int lens[5] = {30, 40, 50, 38, 26};
  for (int k = 0; k < 5; ++k) {
    const float px = c.x - 12 + static_cast<float>(k) * 6;
    b.line({px, c.y + 2}, {px, c.y + 8}, .5f, .33f);
    const V2 rod[4] = {
        {px - 1.5f, c.y + 8}, {px + 1.5f, c.y + 8}, {px + 1.5f, c.y + 8 + lens[k]}, {px - 1.5f, c.y + 8 + lens[k]}};
    b.poly(rod, 4, .85f, 1, b.cylPaint(px - 1.5f, px + 1.5f, .85f));
  }
}
void flBirdcage(B& b, Slot s) {
  const float x = slotX(s), z = 5.3f;
  b.key(z);
  b.cylinder(x, z, .05f, 0, 1.0f, .25f);
  b.cylinder(x, z, .3f, .95f, 1.02f, .25f, -1, false);
  const auto bb = b.bb(x, 1.02f, z);
  for (int dx = -30; dx <= 30; dx += 10) {
    const V2 p[4] = {B::L(bb, static_cast<float>(dx), 0), B::L(bb, dx * 1.02f, -40), B::L(bb, dx * .6f, -70),
                     B::L(bb, 0, -80)};
    b.polyline(p, 4, std::max(.8f, 1.6f * bb.s), .13f);
  }
  b.bbLine(bb, -30, 0, 30, 0, 3, .13f);
  b.bbLine(bb, -12, -30, 12, -30, 2, .33f);
  b.bbEllipse(bb, 0, -38, 8, 6, .4f);
  b.bbEllipse(bb, 6, -44, 4, 4, .4f);
}
void flPerch(B& b, Slot s) {
  const float x = s == BigLeft ? -2.4f : 2.4f, z = ZBm - .7f;
  b.key(z);
  b.cylinder(x, z, .55f, 0, .5f, .55f, .35f);
  const auto bb = b.bb(x, .5f, z);
  const float limbs[16] = {-8, 0, -60, -130, -4, -50, 70, -150, 0, 0, 6, -210, 2, -120, -50, -200};
  for (int k = 0; k < 4; ++k) b.bbLine(bb, limbs[k * 4], limbs[k * 4 + 1], limbs[k * 4 + 2], limbs[k * 4 + 3], 9, .16f);
  const float leaves[15] = {70, -160, .35f, 8, -220, .29f, -52, -206, .42f, 40, -200, .47f, -62, -136, .33f};
  for (int k = 0; k < 5; ++k) b.bbEllipse(bb, leaves[k * 3], leaves[k * 3 + 1], 34, 22, leaves[k * 3 + 2]);
  const V2 nest[4] = {{-90, -140}, {-60, -158}, {-30, -140}, {-60, -130}};
  b.bbPoly(bb, nest, 4, .53f);
}
void flKite(B& b, Slot) { picture(b, PokemonType::Flying); }

// ---- Grass
void grPlant(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .32f, 0, .6f, .62f, .2f);
  const auto bb = b.bb(x, .55f, z);
  const float lv[14] = {-55, 120, -30, 150, -8, 170, 15, 150, 40, 130, 62, 100, -75, 90};
  const float lum[7] = {.3f, .42f, .35f, .48f, .3f, .4f, .45f};
  for (int k = 0; k < 7; ++k) b.bbLeaf(bb, lv[k * 2], lv[k * 2 + 1], lv[k * 2 + 1] * .28f, lum[k]);
}
void grTerrarium(B& b, const ShelfInfo& sh, bool left) {
  const float x = left ? sh.x0 + .7f : sh.x1 - .5f, z = ZBm - .18f;
  b.key(z);
  b.box(x - .25f, x + .25f, sh.y, sh.y + .45f, z - .15f, z + .15f, .85f, false);
  const V2 tl = b.P(x - .23f, sh.y + .43f, z - .15f), br = b.P(x + .23f, sh.y + .02f, z - .15f);
  const V2 q[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
  b.poly(q, 4, .73f);
  const V2 soil[4] = {{tl.x, br.y - 4}, {br.x, br.y - 4}, br, {tl.x, br.y}};
  b.poly(soil, 4, .27f);
  for (int k = 0; k < 4; ++k) {
    const float px = tl.x + (br.x - tl.x) * (.2f + .18f * static_cast<float>(k));
    const V2 sp[3] = {{px - 3, br.y - 3}, {px, br.y - 14}, {px + 3, br.y - 3}};
    b.poly(sp, 3, .2f);
  }
}
void grVines(B& b, Slot) {
  const float xs[4] = {-2.6f, -1.6f, 1.8f, 2.7f};
  b.key(4.4f);
  for (int k = 0; k < 4; ++k) {
    const float z = 4.5f + b.rand(0, 1.8f), ln = b.rand(.8f, 1.6f);
    V2 ps[11];
    for (int i = 0; i <= 10; ++i) {
      const float t = static_cast<float>(i) / 10.0f;
      ps[i] = b.P(xs[k] + std::sin(t * 4) * .06f, CHm - t * ln, z);
    }
    b.polyline(ps, 11, 1.2f, .13f);
    for (int i = 1; i <= 10; ++i) {
      const float d = (i % 2) ? 1.0f : -1.0f;
      const auto bb = B::BB{ps[i], 1.0f};
      b.bbLeaf(bb, 60 * d, 9, 3, b.rand(.3f, .55f), false);
    }
  }
}
void grCluster(B& b, Slot s) {
  const float base = s == BigLeft ? -2.4f : 2.4f;
  const float dir = s == BigLeft ? 1.0f : -1.0f;
  const float set[6] = {-.2f, 0, 1.2f, .5f, .2f, .9f};
  for (int k = 0; k < 2; ++k) {
    const float x = base + set[k * 3] * dir, z = ZBm - .7f + set[k * 3 + 1], sc = set[k * 3 + 2];
    b.key(z - set[k * 3 + 1] * .01f);
    b.cylinder(x, z, .38f * sc, 0, .7f * sc, .75f, .2f);
    auto bb = b.bb(x, .65f * sc, z);
    bb.s *= sc;
    const float lv[10] = {-50, 95, -25, 120, 0, 140, 22, 115, 48, 90};
    const float lum[5] = {.3f, .4f, .33f, .45f, .3f};
    for (int i = 0; i < 5; ++i) b.bbLeaf(bb, lv[i * 2], lv[i * 2 + 1], lv[i * 2 + 1] * .32f, lum[i]);
  }
}
void grFlowers(B& b, Slot) { picture(b, PokemonType::Grass); }
void grBonsai(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.box(x - .45f, x + .45f, 0, .45f, z - .3f, z + .3f, .3f);
  b.box(x - .3f, x + .3f, .45f, .62f, z - .18f, z + .18f, .75f, false);
  const auto bb = b.bb(x, .62f, z);
  const V2 trunk[5] = {B::L(bb, 0, 0), B::L(bb, 8, -30), B::L(bb, -4, -60), B::L(bb, 6, -80), B::L(bb, 5, -100)};
  b.polyline(trunk, 5, 10 * bb.s, .13f);
  b.bbEllipse(bb, -25, -62, 32, 16, .23f);
  b.bbEllipse(bb, 22, -90, 36, 18, .33f);
  b.bbEllipse(bb, 0, -110, 24, 12, .27f);
}

// ---- Poison
void poMushrooms(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .34f, 0, .4f, .4f, .2f);
  const auto bb = b.bb(x, .4f, z);
  const float m[12] = {-14, 50, 26, .25f, 14, 70, 32, .4f, -2, 36, 18, .55f};
  for (int k = 0; k < 3; ++k) {
    const float dx = m[k * 4], h = m[k * 4 + 1], r = m[k * 4 + 2];
    b.bbLine(bb, dx, 0, dx, -h, 9, .87f);
    V2 cap[12];
    for (int i = 0; i <= 10; ++i) {
      const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 10.0f;
      cap[i] = {dx - r * std::cos(a), -h - r * .8f * std::sin(a)};
    }
    b.bbPoly(bb, cap, 11, m[k * 4 + 3]);
    b.bbEllipse(bb, dx - r / 2, -h - r / 3, 4, 4, .93f);
    b.bbEllipse(bb, dx + r / 3, -h - r / 2, 3, 3, .93f);
  }
}
void poFlask(B& b, const ShelfInfo& sh, bool left) {
  const float z = ZBm - .18f;
  b.key(z);
  const float xs[2] = {left ? sh.x0 + .3f : sh.x1 - .55f, left ? sh.x0 + .55f : sh.x1 - .3f};
  const float hs[2] = {.45f, .32f}, ls[2] = {.3f, .55f};
  for (int k = 0; k < 2; ++k) {
    const auto bb = b.bb(xs[k], sh.y, z);
    const float h = hs[k] * 100;
    const V2 glass[8] = {{-6, -h}, {-6, -h * .55f}, {-22, -2}, {-20, 0}, {20, 0}, {22, -2}, {6, -h * .55f}, {6, -h}};
    b.bbPoly(bb, glass, 8, .91f);
    const V2 liquid[4] = {{-14, -16}, {14, -16}, {21, -2}, {-21, -2}};
    b.bbPoly(bb, liquid, 4, ls[k]);
    b.bbEllipse(bb, -4, -24, 3, 3, .47f);
  }
}
void poAlembic(B& b, Slot s) {
  const float x0 = s == BigLeft ? -3.0f : 1.5f, x1 = x0 + 1.5f, z0 = ZBm - .7f;
  b.key(ZBm - .3f);
  b.box(x0, x1, .75f, .85f, z0, ZBm - .1f, .45f);
  b.box(x0 + .08f, x0 + .14f, 0, .75f, z0 + .05f, z0 + .11f, .3f, false);
  b.box(x1 - .14f, x1 - .08f, 0, .75f, z0 + .05f, z0 + .11f, .3f, false);
  b.shadowEllipse((x0 + x1) / 2, z0 + .3f, .8f, .4f, .4f);
  const auto bb = b.bb((x0 + x1) / 2, .85f, z0 + .3f);
  b.bbEllipse(bb, -40, 16, 18, 18, 1, 0);
  b.glow(B::L(bb, -40, 10), 18 * bb.s, 12 * bb.s, .5f);
  b.bbEllipse(bb, -40, -30, 28, 28, .9f);
  const V2 liq[6] = {{-66, -24}, {-14, -24}, {-20, -10}, {-40, -2}, {-60, -10}, {-66, -24}};
  b.bbPoly(bb, liq, 6, .33f);
  const V2 tube[4] = {B::L(bb, -40, -58), B::L(bb, -40, -80), B::L(bb, -20, -92), B::L(bb, 50, -70)};
  b.polyline(tube, 4, 4 * bb.s, .2f);
  const V2 flask[6] = {{44, -72}, {56, -72}, {56, -40}, {72, 0}, {28, 0}, {44, -40}};
  b.bbPoly(bb, flask, 6, .93f);
  const V2 fl2[4] = {{36, -18}, {64, -18}, {72, 0}, {28, 0}};
  b.bbPoly(bb, fl2, 4, .13f);
  b.bbEllipse(bb, -48, -36, 4, 4, 1);
}
void poCauldron(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.shadowEllipse(x, z, .45f, .4f);
  const auto bb = b.bb(x, .45f, z);
  V2 pot[14];
  for (int i = 0; i <= 12; ++i) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
    pot[i] = {48 * std::cos(a), -8 + 52 * std::sin(a)};
  }
  const V2 l = B::L(bb, -48, 0), rr = B::L(bb, 48, 0);
  b.bbPoly(bb, pot, 13, .3f, 1, b.cylPaint(l.x, rr.x, .3f));
  b.bbEllipse(bb, 0, -8, 50, 12, .2f);
  b.bbEllipse(bb, 0, -8, 42, 8, .53f);
  b.bbEllipse(bb, -12, -12, 6, 6, .73f);
  b.bbEllipse(bb, 10, -10, 4, 4, .73f);
  const V2 steam[4] = {B::L(bb, -10, -24), B::L(bb, -20, -44), B::L(bb, -4, -58), B::L(bb, -6, -84)};
  b.polyline(steam, 4, 4 * bb.s, .6f, .6f);
}

// ---- Normal
void noBasket(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .38f, 0, .42f, .55f, .3f);
  for (int k = 0; k < 4; ++k) {
    const float y = .08f + static_cast<float>(k) * .09f;
    const V2 a = b.P(x - .38f, y, z), c = b.P(x + .38f, y, z);
    b.line(a, c, 1, .33f, .7f);
  }
  const auto bb = b.bb(x, .42f, z);
  b.bbEllipse(bb, -10, -10, 16, 14, .8f);
  b.bbEllipse(bb, 14, -8, 12, 12, .6f);
}
void noClock(B& b, Slot) {
  b.key(ZBm - .02f);
  const auto bb = b.bb(-2.3f, 2.2f, ZBm);
  const V2 house[5] = {{-26, 0}, {26, 0}, {26, -50}, {0, -72}, {-26, -50}};
  b.bbPoly(bb, house, 5, .4f);
  b.bbEllipse(bb, 0, -30, 15, 15, .93f);
  b.bbLine(bb, 0, -30, 0, -40, 2, .13f);
  b.bbLine(bb, 0, -30, 7, -30, 2, .13f);
  b.bbLine(bb, 0, 0, 0, 45, 1.5f, .2f);
  b.bbEllipse(bb, 0, 50, 7, 7, .6f);
}
void noYarn(B& b, Slot s) {
  const float x = slotX(s), z = 5.4f;
  b.key(z);
  b.shadowEllipse(x, z, .3f, .3f);
  const V2 c = b.P(x, .25f, z);
  const float r = b.scaleAt(z) * .25f;
  b.sphere(c, r, .7f);
  for (int k = 0; k < 4; ++k) {
    const float a = static_cast<float>(k) * .8f;
    b.line({c.x - r * std::cos(a), c.y - r * std::sin(a)}, {c.x + r * std::cos(a), c.y + r * std::sin(a)}, 1, .4f, .6f);
  }
  const V2 thread[3] = {{c.x + r, c.y + 2}, {c.x + r * 2, c.y + r * .6f}, {c.x + r * 3, c.y + r * .5f}};
  b.polyline(thread, 3, 1, .4f);
}
void noArmchair(B& b, Slot s) {
  const float x0 = s == BigLeft ? -2.9f : 1.8f, x1 = x0 + 1.1f, z0 = ZBm - 1.1f;
  b.key(z0 + .3f);
  b.box(x0 + .1f, x1 - .1f, 0, 1.3f, z0 + .7f, z0 + .9f, .62f);
  b.box(x0, x1, 0, .5f, z0, z0 + .9f, .5f);
  b.box(x0, x0 + .18f, 0, .75f, z0, z0 + .9f, .66f, false);
  b.box(x1 - .18f, x1, 0, .75f, z0, z0 + .9f, .66f, false);
}

// ---- Ice
void icCrystal(B& b, const ShelfInfo& sh, bool left) {
  const float x = left ? sh.x0 + .4f : sh.x1 - .4f, z = ZBm - .18f;
  b.key(z);
  const auto bb = b.bb(x, sh.y, z);
  const float sp[12] = {-10, 40, -10, 0, 60, .9f, 10, 50, 8, -20, 35, .75f};
  for (int k = 0; k < 2; ++k) {
    const float bx = sp[k * 6], h = sp[k * 6 + 1], w = 10;
    const V2 c[5] = {{bx - w, 0}, {bx + w, 0}, {bx + w, -h}, {bx, -h - 14}, {bx - w, -h}};
    b.bbPoly(bb, c, 5, sp[k * 6 + 5], 1, -1, sp[k * 6 + 3]);
  }
  b.glow(B::L(bb, 0, -30), 20 * bb.s * 3, 20 * bb.s * 3, .3f);
}
void icStalactites(B& b, Slot) {
  b.key(4.8f);
  for (int k = 0; k < 7; ++k) {
    const float x = -2.6f + static_cast<float>(k) * .85f + b.rand(-.2f, .2f), z = b.rand(4.4f, 6.8f);
    const float ln = b.rand(.3f, .8f);
    const V2 t[3] = {b.P(x - .08f, CHm, z), b.P(x + .08f, CHm, z), b.P(x, CHm - ln, z)};
    b.poly(t, 3, .88f);
    const V2 hl[3] = {t[0], {t[0].x + 1, t[0].y}, t[2]};
    b.poly(hl, 3, 1.0f);
  }
}
void icSnowman(B& b, Slot s) {
  const float x = slotX(s), z = 5.4f;
  b.key(z);
  b.shadowEllipse(x, z, .4f, .35f);
  b.sphere(b.P(x, .32f, z), b.scaleAt(z) * .34f, .92f);
  b.sphere(b.P(x, .82f, z), b.scaleAt(z) * .24f, .95f);
  const V2 head = b.P(x, .82f, z);
  const float r = b.scaleAt(z) * .24f;
  b.ellipse({head.x - r * .3f, head.y - r * .2f}, 1.5f, 1.5f, .1f);
  b.ellipse({head.x + r * .3f, head.y - r * .2f}, 1.5f, 1.5f, .1f);
  const V2 nose[3] = {{head.x, head.y}, {head.x + r * .8f, head.y + 2}, {head.x, head.y + 3}};
  b.poly(nose, 3, .45f);
  b.line(b.P(x - .2f, .58f, z), b.P(x + .2f, .58f, z), 3, .3f);
}
void icFrostPic(B& b, Slot) { picture(b, PokemonType::Ice); }
void icIceBlock(B& b, Slot s) {
  const float x0 = s == BigLeft ? -2.9f : 1.7f;
  b.key(ZBm - .4f);
  b.box(x0, x0 + 1.2f, 0, .9f, ZBm - .9f, ZBm - .1f, .85f);
  const V2 a = b.P(x0 + .2f, .75f, ZBm - .9f), c = b.P(x0 + .6f, .2f, ZBm - .9f);
  b.line(a, c, 2, 1.0f, .8f);
}

// ---- Fighting
void fgBag(B& b, Slot s) {
  const float x = slotX(s) * .95f, z = 5.4f;
  b.key(z);
  b.line(b.P(x, CHm, z), b.P(x, 2.05f, z), 1.4f, .2f);
  b.shadowEllipse(x, z, .35f, .3f, .3f);
  const V2 c = b.P(x, 1.4f, z);
  const float rx = b.scaleAt(z) * .28f, top = b.P(x, 2.0f, z).y, bot = b.P(x, .8f, z).y;
  const V2 bag[4] = {{c.x - rx, top}, {c.x + rx, top}, {c.x + rx, bot}, {c.x - rx, bot}};
  b.poly(bag, 4, .3f, 1, b.cylPaint(c.x - rx, c.x + rx, .35f));
  b.ellipse({c.x, top}, rx, rx * .3f, .45f);
  b.ellipse({c.x, bot}, rx, rx * .3f, .25f);
  b.line({c.x - rx, top + (bot - top) * .2f}, {c.x + rx, top + (bot - top) * .2f}, 2, .7f);
}
void fgDumbbells(B& b, Slot s) {
  const float x = slotX(s), z = 5.6f;
  b.key(z);
  for (int k = 0; k < 2; ++k) {
    const float zz = z + static_cast<float>(k) * .35f;
    b.shadowEllipse(x, zz, .35f, .12f, .35f);
    b.line(b.P(x - .25f, .1f, zz), b.P(x + .25f, .1f, zz), 2.5f, .35f);
    b.sphere(b.P(x - .27f, .1f, zz), b.scaleAt(zz) * .1f, .3f);
    b.sphere(b.P(x + .27f, .1f, zz), b.scaleAt(zz) * .1f, .3f);
  }
}
void fgBelt(B& b, Slot) {
  b.key(ZBm - .02f);
  const auto bb = b.bb(-2.3f, 2.5f, ZBm);
  const V2 frame[4] = {{-55, -30}, {55, -30}, {55, 30}, {-55, 30}};
  b.bbPoly(bb, frame, 4, .35f);
  const V2 belt[4] = {{-48, -6}, {48, -6}, {48, 6}, {-48, 6}};
  b.bbPoly(bb, belt, 4, .1f);
  b.bbEllipse(bb, 0, 0, 16, 14, .85f);
  b.bbEllipse(bb, 0, 0, 9, 8, .55f);
}
void fgDummy(B& b, Slot s) {
  const float x = s == BigLeft ? -2.4f : 2.4f, z = ZBm - .8f;
  b.key(z);
  b.cylinder(x, z, .25f, 0, 1.6f, .5f, .55f);
  b.box(x - .55f, x + .55f, 1.1f, 1.2f, z - .06f, z + .06f, .45f, false);
  b.box(x - .4f, x + .4f, .7f, .8f, z - .06f, z + .06f, .45f, false);
}
void fgTatamiPic(B& b, Slot) { picture(b, PokemonType::Fighting); }

// ---- Ground
void grdPots(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  const float d[6] = {-.15f, .28f, .55f, .2f, .2f, .38f};
  for (int k = 0; k < 2; ++k) {
    const float xx = x + d[k * 3], r = d[k * 3 + 1], h = d[k * 3 + 2];
    b.shadowEllipse(xx, z, r, r * .8f);
    const V2 c = b.P(xx, h * .45f, z);
    const float rx = b.scaleAt(z) * r;
    b.ellipse(c, rx, (b.P(xx, 0, z).y - b.P(xx, h, z).y) * .48f, .55f, 1, b.cylPaint(c.x - rx, c.x + rx, .6f));
    b.cylinder(xx, z, r * .45f, h * .8f, h, .5f, .15f, false);
  }
}
void grdCactus(B& b, Slot s) {
  const float x = slotX(s), z = 5.4f;
  b.key(z);
  b.cylinder(x, z, .26f, 0, .35f, .6f, .3f);
  const auto bb = b.bb(x, .35f, z);
  const V2 body[4] = {{-12, 0}, {12, 0}, {12, -95}, {-12, -95}};
  b.bbPoly(bb, body, 4, .35f, 1, b.cylPaint(B::L(bb, -12, 0).x, B::L(bb, 12, 0).x, .4f));
  b.bbEllipse(bb, 0, -95, 12, 8, .38f);
  const V2 arm[4] = {{12, -50}, {30, -50}, {30, -78}, {22, -78}};
  b.bbPoly(bb, arm, 4, .33f);
  const V2 arm2[4] = {{-12, -38}, {-28, -38}, {-28, -62}, {-20, -62}};
  b.bbPoly(bb, arm2, 4, .3f);
}
void grdBone(B& b, Slot) {
  b.key(ZBm - .02f);
  const auto bb = b.bb(-2.3f, 2.45f, ZBm);
  b.bbLine(bb, -40, 0, 40, 0, 10, .9f);
  for (const float ex : {-44.0f, 44.0f}) {
    b.bbEllipse(bb, ex, -6, 8, 8, .9f);
    b.bbEllipse(bb, ex, 6, 8, 8, .9f);
  }
}
void grdSand(B& b, Slot s) {
  const float x = s == BigLeft ? -2.3f : 2.3f;
  b.key(ZBm - .6f);
  b.shadowEllipse(x, ZBm - .8f, .9f, .5f, .3f);
  const V2 c = b.P(x, 0, ZBm - .8f);
  const float rx = b.scaleAt(ZBm - .8f) * .9f;
  V2 dune[14];
  for (int i = 0; i <= 12; ++i) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
    dune[i] = {c.x - rx * std::cos(a), c.y - rx * .45f * std::sin(a)};
  }
  b.poly(dune, 13, .7f, 1, b.vgrad(c.y - rx * .45f, c.y, .85f, .6f));
}

// ---- Psychic
void psOrb(B& b, Slot s) {
  const float x = slotX(s), z = 5.3f;
  b.key(z);
  b.cylinder(x, z, .22f, 0, .9f, .3f, .45f);
  const V2 c = b.P(x, 1.15f, z);
  const float r = b.scaleAt(z) * .25f;
  b.glow(c, r * 2.2f, r * 2.2f, .35f);
  const Stop st[3] = {{0, .98f}, {.5f, .7f}, {1, .35f}};
  b.ellipse(c, r, r, .7f, 1, b.radial({c.x - r * .3f, c.y - r * .3f}, r * 1.3f, r * 1.3f, st, 3));
}
void psSpoons(B& b, const ShelfInfo& sh, bool left) {
  const float z = ZBm - .18f;
  b.key(z);
  for (int k = 0; k < 3; ++k) {
    const float x = (left ? sh.x0 + .25f : sh.x1 - .6f) + static_cast<float>(k) * .14f;
    const auto bb = b.bb(x, sh.y, z);
    b.bbLine(bb, 0, 0, static_cast<float>(k * 6 - 6), -30, 3, .75f);
    b.bbEllipse(bb, static_cast<float>(k * 10 - 10), -36, 5, 8, .8f);
  }
}
void psBooks(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  float y = 0;
  for (int k = 0; k < 5; ++k) {
    const float h = b.rand(.08f, .14f), w = b.rand(.45f, .6f), off = b.rand(-.06f, .06f);
    b.box(x - w / 2 + off, x + w / 2 + off, y, y + h, z - .2f, z + .2f, b.rand(.3f, .85f), k == 0);
    y += h;
  }
}
void psStarsPic(B& b, Slot) { picture(b, PokemonType::Psychic); }
void psCards(B& b, Slot sl) {
  const float x = sl == SideLeft ? -Wm + .02f : Wm - .02f;
  b.key(6.2f);
  for (int k = 0; k < 3; ++k) {
    const float zc = 5.6f + static_cast<float>(k) * .5f, yc = 2.3f + (k == 1 ? .3f : 0.0f);
    const V2 q[4] = {b.P(x, yc - .3f, zc - .18f), b.P(x, yc - .3f, zc + .18f), b.P(x, yc + .3f, zc + .18f),
                     b.P(x, yc + .3f, zc - .18f)};
    b.poly(q, 4, .95f);
    const V2 e[5] = {q[0], q[1], q[2], q[3], q[0]};
    b.polyline(e, 5, .8f, .2f);
    b.ellipse(b.P(x, yc, zc), 2.5f, 3.5f, .2f);
  }
}

// ---- Bug
void bgJar(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .24f, 0, .55f, .82f, .7f);
  b.cylinder(x, z, .2f, .55f, .63f, .35f, -1, false);
  const V2 c = b.P(x, .28f, z);
  b.glow(c, 22, 26, .4f);
  for (int k = 0; k < 4; ++k) b.ellipse({c.x + b.rand(-8, 8), c.y + b.rand(-12, 10)}, 1.6f, 1.6f, 1.0f);
}
void bgLeaf(B& b, Slot s) {
  const float x = s == BigLeft ? -2.4f : 2.4f, z = ZBm - .7f;
  b.key(z);
  b.cylinder(x, z, .3f, 0, .4f, .45f, .25f);
  const auto bb = b.bb(x, .4f, z);
  b.bbLeaf(bb, -20, 150, 42, .35f);
  b.bbLeaf(bb, 25, 125, 36, .45f);
}
void bgNet(B& b, Slot s) {
  const float x = slotX(s), z = 5.5f;
  b.key(z);
  b.line(b.P(x - .1f, 0, z), b.P(x + .1f, 1.8f, z), 2.2f, .3f);
  const V2 c = b.P(x + .15f, 2.05f, z);
  const float r = b.scaleAt(z) * .25f;
  b.ellipse(c, r, r, .9f, .45f);
  V2 ring[17];
  for (int i = 0; i <= 16; ++i) {
    const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 8.0f;
    ring[i] = {c.x + r * std::cos(a), c.y + r * std::sin(a)};
  }
  b.polyline(ring, 17, 1.5f, .25f);
}
void bgLog(B& b, Slot s) {
  const float x = slotX(s), z = 5.6f;
  b.key(z);
  b.cylinder(x, z, .32f, 0, .45f, .38f, .72f);
  const V2 top = b.P(x, .45f, z);
  const float rx = b.scaleAt(z) * .32f;
  b.ellipse(top, rx * .6f, rx * .15f, .6f, .6f);
}
void bgHivePic(B& b, Slot) { picture(b, PokemonType::Bug); }

// ---- Rock
void rkFossil(B& b, Slot) {
  b.key(ZBm - .02f);
  const auto bb = b.bb(-2.3f, 2.5f, ZBm);
  b.bbEllipse(bb, 0, 0, 42, 42, .62f);
  V2 sp[26];
  for (int i = 0; i < 26; ++i) {
    const float a = static_cast<float>(i) * .55f, rr = 4.0f + static_cast<float>(i) * 1.45f;
    sp[i] = B::L(bb, rr * std::cos(a), rr * std::sin(a));
  }
  b.polyline(sp, 26, 2.2f * bb.s * 2, .25f);
}
void rkMinerals(B& b, Slot s) {
  const float x = slotX(s), z = 5.4f;
  b.key(z);
  b.shadowEllipse(x, z, .4f, .3f);
  const auto bb = b.bb(x, 0, z);
  const float cr[15] = {-20, 50, -15, .55f, 0, 0, 75, 5, .8f, 0, 20, 45, 20, .4f, 0};
  for (int k = 0; k < 3; ++k) {
    const float bx = cr[k * 5], h = cr[k * 5 + 1];
    const V2 c[5] = {{bx - 11, 0}, {bx + 11, 0}, {bx + 11, -h}, {bx, -h - 16}, {bx - 11, -h}};
    b.bbPoly(bb, c, 5, cr[k * 5 + 3], 1, -1, cr[k * 5 + 2]);
  }
}
void rkBench(B& b, Slot s) {
  const float x0 = s == BigLeft ? -3.0f : 1.6f;
  b.key(ZBm - .5f);
  b.box(x0, x0 + .25f, 0, .45f, ZBm - .8f, ZBm - .2f, .55f);
  b.box(x0 + 1.15f, x0 + 1.4f, 0, .45f, ZBm - .8f, ZBm - .2f, .55f);
  b.box(x0 - .05f, x0 + 1.45f, .45f, .6f, ZBm - .85f, ZBm - .15f, .62f, false);
}
void rkBoulder(B& b, Slot s) {
  const float x = slotX(s), z = 5.5f;
  b.key(z);
  b.shadowEllipse(x, z, .5f, .4f);
  const V2 c = b.P(x, .3f, z);
  b.sphere(c, b.scaleAt(z) * .4f, .5f);
}
void rkCliffPic(B& b, Slot) { picture(b, PokemonType::Rock); }

// ---- Ghost
void ghCandelabra(B& b, Slot s) {
  const float x = slotX(s), z = SMALL_Z;
  b.key(z);
  b.cylinder(x, z, .2f, 0, .1f, .2f, .35f);
  b.cylinder(x, z, .04f, .1f, 1.3f, .25f, -1, false);
  b.line(b.P(x - .3f, 1.3f, z), b.P(x + .3f, 1.3f, z), 2, .2f);
  for (const float dx : {-.3f, 0.0f, .3f}) {
    b.cylinder(x + dx, z, .04f, 1.3f, 1.5f, .9f, -1, false);
    const V2 c = b.P(x + dx, 1.5f, z);
    b.glow({c.x, c.y - 4}, 14, 14, .45f);
    b.bbFlame(b.bb(x + dx, 1.5f, z), 0, -1, 12, 1.0f);
  }
}
void ghMirror(B& b, Slot) {
  b.key(ZBm - .02f);
  const V2 c = b.P(-2.3f, 2.5f, ZBm);
  const float rx = b.scaleAt(ZBm) * .45f, ry = rx * 1.4f;
  b.ellipse({c.x + 2, c.y + 3}, rx * 1.15f, ry * 1.12f, 0, .3f);
  b.ellipse(c, rx * 1.15f, ry * 1.12f, .3f);
  b.ellipse(c, rx, ry, .7f, 1, b.vgrad(c.y - ry, c.y + ry, .85f, .45f));
  b.line({c.x - rx * .4f, c.y + ry * .2f}, {c.x + rx * .1f, c.y - ry * .5f}, 2, 1, .5f);
}
void ghCobwebs(B& b, Slot) {
  b.key(ZBm);
  for (const float sx : {-1.0f, 1.0f}) {
    const V2 corner = b.P(sx * Wm, CHm, ZBm);
    for (int k = 0; k < 5; ++k) {
      const float a = static_cast<float>(k) * .38f;
      b.line(corner, {corner.x - sx * 40 * std::cos(a), corner.y + 40 * std::sin(a)}, .7f, .95f, .8f);
    }
    for (int ring = 1; ring <= 3; ++ring) {
      V2 arc[5];
      for (int k = 0; k < 5; ++k) {
        const float a = static_cast<float>(k) * .38f;
        arc[k] = {corner.x - sx * 12 * ring * std::cos(a), corner.y + 12 * ring * std::sin(a)};
      }
      b.polyline(arc, 5, .7f, .95f, .8f);
    }
  }
}
void ghLantern(B& b, Slot) {
  const float x = 1.6f, z = 4.8f;
  b.key(z);
  const V2 c = b.P(x, 3.0f, z);
  b.glow(c, 40, 40, .45f);
  b.line(b.P(x, CHm, z), {c.x, c.y - 12}, 1, .2f);
  const V2 body[4] = {{c.x - 7, c.y - 10}, {c.x + 7, c.y - 10}, {c.x + 9, c.y + 10}, {c.x - 9, c.y + 10}};
  b.poly(body, 4, .95f);
  b.ellipse(c, 4, 6, 1.0f);
}
void ghMoonPic(B& b, Slot) { picture(b, PokemonType::Ghost); }

// ---- Dragon
void drTreasure(B& b, Slot s) {
  const float x = s == BigLeft ? -2.3f : 2.3f, z = ZBm - .9f;
  b.key(z);
  b.shadowEllipse(x, z, 1.0f, .5f, .4f);
  const V2 c = b.P(x, 0, z);
  const float rx = b.scaleAt(z) * .9f;
  V2 pile[14];
  for (int i = 0; i <= 12; ++i) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
    pile[i] = {c.x - rx * std::cos(a), c.y - rx * .5f * std::sin(a)};
  }
  b.poly(pile, 13, .75f, 1, b.vgrad(c.y - rx * .5f, c.y, .9f, .55f));
  for (int k = 0; k < 16; ++k) {
    const float a = b.rand(.2f, 2.9f), rr = b.rand(.1f, .9f);
    b.ellipse({c.x - rx * rr * std::cos(a), c.y - rx * .5f * rr * std::sin(a)}, 3, 1.6f, b.rand(.6f, 1.0f));
  }
  b.box(x + .2f, x + .75f, 0, .4f, z - .2f, z + .2f, .4f, false);
  b.box(x + .2f, x + .75f, .4f, .48f, z - .2f, z + .2f, .7f, false);
}
void drBanner(B& b, Slot s) {
  const float x = s == SideLeft ? -Wm + .02f : Wm - .02f;
  b.key(6.4f);
  const V2 q[5] = {b.P(x, 3.6f, 6.0f), b.P(x, 3.6f, 6.8f), b.P(x, 1.6f, 6.8f), b.P(x, 1.9f, 6.4f), b.P(x, 1.6f, 6.0f)};
  b.poly(q, 5, .25f, 1, b.vgrad(q[0].y, q[2].y, .35f, .15f));
  const V2 e[3] = {b.P(x, 3.0f, 6.2f), b.P(x, 2.6f, 6.4f), b.P(x, 3.0f, 6.6f)};
  b.polyline(e, 3, 2, .85f);
  b.ellipse(b.P(x, 2.4f, 6.4f), 3, 4, .85f);
}
void drEgg(B& b, Slot s) {
  const float x = slotX(s), z = 5.4f;
  b.key(z);
  b.cylinder(x, z, .35f, 0, .18f, .35f, .55f);
  const V2 c = b.P(x, .45f, z);
  const float r = b.scaleAt(z) * .22f;
  const Stop st[3] = {{0, .98f}, {.6f, .82f}, {1, .5f}};
  b.ellipse(c, r, r * 1.35f, .85f, 1, b.radial({c.x - r * .3f, c.y - r * .4f}, r * 1.5f, r * 1.8f, st, 3));
  for (int k = 0; k < 4; ++k) b.ellipse({c.x + b.rand(-r * .6f, r * .6f), c.y + b.rand(-r, r * .8f)}, 2.5f, 2, .45f);
}
void drScale(B& b, Slot) {
  b.key(ZBm - .02f);
  const auto bb = b.bb(-2.3f, 2.1f, ZBm);
  const V2 shield[6] = {{-36, -90}, {36, -90}, {36, -40}, {0, 0}, {-36, -40}, {-36, -90}};
  b.bbPoly(bb, shield, 6, .4f, 1, b.vgrad(B::L(bb, 0, -90).y, B::L(bb, 0, 0).y, .6f, .25f));
  b.bbLine(bb, 0, -84, 0, -10, 3, .85f);
  b.bbLine(bb, -30, -60, 30, -60, 3, .85f);
}
void drCastlePic(B& b, Slot) { picture(b, PokemonType::Dragon); }

// ---- Type tables

using SlotFn = void (*)(B&, Slot);
using ShelfFn = void (*)(B&, const ShelfInfo&, bool);

struct TypeItem {
  SlotFn fn = nullptr;
  ShelfFn shelfFn = nullptr;
  Slot slots[2] = {SLOT_COUNT, SLOT_COUNT};
};

void fiCandlesSlot(B& b, Slot s) { fiCandles(b, s, nullptr, false); }
void fiCandlesShelf(B& b, const ShelfInfo& sh, bool left) { fiCandles(b, Shelf, &sh, left); }

constexpr size_t TYPE_ITEMS_MAX = 6;

struct TypeItems {
  TypeItem items[TYPE_ITEMS_MAX];
  uint8_t count;
};

TypeItems typeItems(PokemonType type) {
  switch (type) {
    case PokemonType::Electric:
      return {{{elBattery, nullptr, {FloorRight, FloorLeft}},
               {elPoster, nullptr, {WallLeft, SideLeft}},
               {nullptr, elPlasma, {Shelf, SLOT_COUNT}},
               {elTv, nullptr, {BigLeft, BigRight}},
               {elNeon, nullptr, {SideRight, SideLeft}},
               {elTesla, nullptr, {FloorLeft, FloorRight}}},
              6};
    case PokemonType::Water:
      return {{{waShellLamp, nullptr, {FloorLeft, FloorRight}},
               {nullptr, waFishbowl, {Shelf, SLOT_COUNT}},
               {waPorthole, nullptr, {SideLeft, SideRight}},
               {waAquarium, nullptr, {BigLeft, BigRight}},
               {waLifebuoy, nullptr, {WallLeft, SideRight}},
               {waCoral, nullptr, {FloorRight, FloorLeft}}},
              6};
    case PokemonType::Fire:
      return {{{fiCandlesSlot, fiCandlesShelf, {Shelf, FloorRight}},
               {fiVolcano, nullptr, {WallLeft, SLOT_COUNT}},
               {fiFireplace, nullptr, {BigLeft, BigRight}},
               {fiBrazier, nullptr, {FloorRight, FloorLeft}},
               {fiSconces, nullptr, {SideLeft, SideRight}}},
              5};
    case PokemonType::Flying:
      return {{{flFeathers, nullptr, {FloorLeft, FloorRight}},
               {flChimes, nullptr, {Ceiling2, SLOT_COUNT}},
               {flBirdcage, nullptr, {FloorRight, FloorLeft}},
               {flPerch, nullptr, {BigRight, BigLeft}},
               {flKite, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Grass:
      return {{{grPlant, nullptr, {FloorLeft, FloorRight}},
               {nullptr, grTerrarium, {Shelf, SLOT_COUNT}},
               {grVines, nullptr, {Ceiling2, SLOT_COUNT}},
               {grCluster, nullptr, {BigRight, BigLeft}},
               {grFlowers, nullptr, {WallLeft, SLOT_COUNT}},
               {grBonsai, nullptr, {FloorRight, FloorLeft}}},
              6};
    case PokemonType::Poison:
      return {{{poMushrooms, nullptr, {FloorRight, FloorLeft}},
               {nullptr, poFlask, {Shelf, SLOT_COUNT}},
               {poAlembic, nullptr, {BigLeft, BigRight}},
               {poCauldron, nullptr, {FloorLeft, FloorRight}}},
              4};
    case PokemonType::Ice:
      return {{{icSnowman, nullptr, {FloorLeft, FloorRight}},
               {nullptr, icCrystal, {Shelf, SLOT_COUNT}},
               {icStalactites, nullptr, {Ceiling2, SLOT_COUNT}},
               {icIceBlock, nullptr, {BigLeft, BigRight}},
               {icFrostPic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Fighting:
      return {{{fgBag, nullptr, {FloorLeft, FloorRight}},
               {fgBelt, nullptr, {WallLeft, SLOT_COUNT}},
               {fgDumbbells, nullptr, {FloorRight, FloorLeft}},
               {fgDummy, nullptr, {BigLeft, BigRight}},
               {fgTatamiPic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Ground:
      return {{{grdPots, nullptr, {FloorRight, FloorLeft}},
               {grdBone, nullptr, {WallLeft, SLOT_COUNT}},
               {grdCactus, nullptr, {FloorLeft, FloorRight}},
               {grdSand, nullptr, {BigLeft, BigRight}}},
              4};
    case PokemonType::Psychic:
      return {{{psOrb, nullptr, {FloorRight, FloorLeft}},
               {nullptr, psSpoons, {Shelf, SLOT_COUNT}},
               {psBooks, nullptr, {FloorLeft, FloorRight}},
               {psCards, nullptr, {SideLeft, SideRight}},
               {psStarsPic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Bug:
      return {{{bgJar, nullptr, {FloorRight, FloorLeft}},
               {bgNet, nullptr, {FloorLeft, FloorRight}},
               {bgLeaf, nullptr, {BigLeft, BigRight}},
               {bgLog, nullptr, {FloorRight, FloorLeft}},
               {bgHivePic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Rock:
      return {{{rkMinerals, nullptr, {FloorRight, FloorLeft}},
               {rkFossil, nullptr, {WallLeft, SLOT_COUNT}},
               {rkBench, nullptr, {BigLeft, BigRight}},
               {rkBoulder, nullptr, {FloorLeft, FloorRight}},
               {rkCliffPic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Ghost:
      return {{{ghCandelabra, nullptr, {FloorLeft, FloorRight}},
               {ghMirror, nullptr, {WallLeft, SLOT_COUNT}},
               {ghCobwebs, nullptr, {Ceiling2, SLOT_COUNT}},
               {ghLantern, nullptr, {Ceiling2, SLOT_COUNT}},
               {ghMoonPic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    case PokemonType::Dragon:
      return {{{drEgg, nullptr, {FloorRight, FloorLeft}},
               {drScale, nullptr, {WallLeft, SLOT_COUNT}},
               {drTreasure, nullptr, {BigLeft, BigRight}},
               {drBanner, nullptr, {SideLeft, SideRight}},
               {drCastlePic, nullptr, {WallLeft, SLOT_COUNT}}},
              5};
    default:  // Normal and anything outside Gen 1
      return {{{noYarn, nullptr, {FloorRight, FloorLeft}},
               {noClock, nullptr, {WallLeft, SLOT_COUNT}},
               {noBasket, nullptr, {FloorLeft, FloorRight}},
               {noArmchair, nullptr, {BigLeft, BigRight}}},
              4};
  }
}

PokemonType normalizedType(PokemonType t) {
  switch (t) {
    case PokemonType::None:
    case PokemonType::Dark:
    case PokemonType::Steel:
    case PokemonType::Fairy:
      return PokemonType::Normal;
    default:
      return t;
  }
}

// ---- Common furniture

float bed(B& b, uint8_t tier) {
  const float z = 4.7f;
  b.key(z + .01f);
  if (tier == 1) {
    b.shadowEllipse(0, z, 1.05f, .75f, .55f);
    b.floorEllipse(0, z, .95f, .65f, .42f);
    for (int k = 0; k < 220; ++k) {
      const float a = b.rand(0, 6.283f), rr = b.rand(.55f, 1.0f);
      const float x = rr * std::cos(a), zz = z + rr * .68f * std::sin(a), y = b.rand(0, .22f) * rr;
      b.line(b.P(x, y, zz), b.P(x + b.rand(-.15f, .15f), y + b.rand(-.05f, .05f), zz + b.rand(-.1f, .1f)), 1.0f,
             b.rand(.35f, .85f));
    }
    return .12f;
  }
  if (tier == 2) {
    b.shadowEllipse(0, z, 1.05f, .7f, .55f);
    b.cylinder(0, z, .9f, 0, .22f, .6f, .78f, false);
    const V2 c = b.P(0, .22f, z);
    const Stop st[2] = {{0, .92f}, {1, .7f}};
    b.ellipse(c, b.scaleAt(z) * .85f, 10, .8f, 1, b.radial(c, b.scaleAt(z) * .85f, 10, st, 2));
    return .22f;
  }
  if (tier <= 4) {
    b.shadowEllipse(0, z, 1.15f, .8f, .55f);
    b.cylinder(0, z, 1.0f, 0, .4f, .5f, .62f, false);
    const V2 c = b.P(0, .4f, z);
    const float rx = b.scaleAt(z);
    const Stop st[2] = {{0, .9f}, {1, .62f}};
    b.ellipse({c.x, c.y + 2}, rx * .78f, 11, .8f, 1, b.radial({c.x, c.y + 2}, rx * .78f, 11, st, 2));
    V2 rim[25];
    for (int i = 0; i <= 24; ++i) {
      const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 12.0f;
      rim[i] = {c.x + rx * .9f * std::cos(a), c.y + 14 * std::sin(a)};
    }
    b.polyline(rim, 25, 5, .72f);
    return .3f;
  }
  const float zf = 3.9f, zb = 6.3f;
  b.box(-1.1f, 1.1f, 0, .35f, zf, zb, .35f);
  b.box(-1.15f, 1.15f, 0, tier < 7 ? 1.4f : 1.75f, zb, zb + .15f, .32f, false);
  b.box(-1.0f, 1.0f, .35f, .62f, zf + .05f, zb, .88f, false);
  b.box(-.8f, .8f, .62f, .78f, zb - .6f, zb - .1f, .95f, false);
  b.box(-1.04f, 1.04f, .3f, .66f, zf, zf + 1.1f, .62f, false);
  return .66f;
}

void rug(B& b, uint8_t tier) {
  const float z = 4.7f;
  b.key(30.0f);
  if (tier <= 3) {
    b.floorEllipse(0, z, 1.7f, 1.2f, .32f);
    // dashed inner ring
    for (int k = 0; k < 24; k += 2) {
      const float a0 = static_cast<float>(k) * static_cast<float>(M_PI) / 12.0f;
      const float a1 = a0 + static_cast<float>(M_PI) / 14.0f;
      b.line(b.P(1.5f * std::cos(a0), .003f, z + 1.04f * std::sin(a0)),
             b.P(1.5f * std::cos(a1), .003f, z + 1.04f * std::sin(a1)), 2, .7f);
    }
  } else {
    const float rl = tier == 6 ? .3f : .25f;
    b.quad3(-1.9f, .003f, 3.4f, 1.9f, .003f, 3.4f, 1.9f, .003f, 6.4f, -1.9f, .003f, 6.4f, rl);
    const V2 e[5] = {b.P(-1.7f, .004f, 3.6f), b.P(1.7f, .004f, 3.6f), b.P(1.7f, .004f, 6.2f), b.P(-1.7f, .004f, 6.2f),
                     b.P(-1.7f, .004f, 3.6f)};
    b.polyline(e, 5, 2, .75f);
    for (const float dx : {-1.0f, 1.0f}) {
      b.quad3(-.25f + dx, .005f, 4.65f, dx, .005f, 4.9f, .25f + dx, .005f, 4.65f, dx, .005f, 4.4f, .7f);
    }
  }
  if (tier >= 7) {
    b.key(29.0f);
    b.quad3(-.7f, .006f, 5.8f, .7f, .006f, 5.8f, .7f, .006f, ZBm, -.7f, .006f, ZBm, .22f);
    const V2 e[4] = {b.P(-.6f, .007f, ZBm), b.P(-.6f, .007f, 5.9f), b.P(.6f, .007f, 5.9f), b.P(.6f, .007f, ZBm)};
    b.polyline(e, 4, 1.5f, .8f);
  }
}

void ceilingLight(B& b, uint8_t tier) {
  if (tier <= 3) {
    const float z = 5.4f;
    b.key(z);
    const V2 c = b.P(0, 3.3f, z);
    b.line(b.P(0, CHm, z), {c.x, c.y - 14}, 1.2f, .07f);
    b.glow(c, 38, 38, .55f);
    const V2 sock[4] = {{c.x - 4, c.y - 14}, {c.x + 4, c.y - 14}, {c.x + 4, c.y - 6}, {c.x - 4, c.y - 6}};
    b.poly(sock, 4, .2f);
    const Stop st[2] = {{0, 1}, {1, .85f}};
    b.ellipse(c, 8, 8, 1, 1, b.radial(c, 8, 8, st, 2));
  } else if (tier <= 5) {
    const float z = 5.6f;
    b.key(z);
    const V2 c = b.P(0, 3.2f, z);
    b.line(b.P(0, CHm, z), {c.x, c.y - 16}, 1.2f, .07f);
    b.glow({c.x, c.y + 22}, 60, 40, .45f);
    V2 dome[14];
    for (int i = 0; i <= 12; ++i) {
      const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 12.0f;
      dome[i] = {c.x - 26 * std::cos(a), c.y + 4 - 30 * std::sin(a)};
    }
    b.poly(dome, 13, .55f, 1, b.cylPaint(c.x - 26, c.x + 26, .55f));
    b.ellipse({c.x, c.y + 4}, 26, 5, 1.0f);
  } else {
    const float z = 5.6f;
    b.key(z);
    const V2 c = b.P(0, 3.1f, z);
    b.line(b.P(0, CHm, z), {c.x, c.y - 10}, 1.5f, .07f);
    b.glow(c, 80, 50, .5f);
    V2 ring[25], ring2[25];
    for (int i = 0; i <= 24; ++i) {
      const float a = static_cast<float>(i) * static_cast<float>(M_PI) / 12.0f;
      ring[i] = {c.x + 44 * std::cos(a), c.y + 9 * std::sin(a)};
      ring2[i] = {c.x + 24 * std::cos(a), c.y - 10 + 5 * std::sin(a)};
    }
    b.polyline(ring, 25, 2.5f, .13f);
    b.polyline(ring2, 25, 2, .13f);
    for (int k = 0; k < 7; ++k) {
      const float a = static_cast<float>(k) * 2.0f * static_cast<float>(M_PI) / 7.0f;
      const float px = c.x + 44 * std::cos(a), py = c.y + 9 * std::sin(a);
      const V2 cnd[4] = {{px - 1.5f, py - 9}, {px + 1.5f, py - 9}, {px + 1.5f, py}, {px - 1.5f, py}};
      b.poly(cnd, 4, .93f);
      b.ellipse({px, py - 12}, 2, 3.5f, 1.0f);
      const V2 drop[4] = {{px, py + 2}, {px - 2, py + 8}, {px, py + 12}, {px + 2, py + 8}};
      b.poly(drop, 4, .87f);
    }
  }
}

ShelfInfo wallShelf(B& b) {
  const float x0 = 1.55f, x1 = 2.95f, y = 2.2f;
  b.key(ZBm - .01f);
  const V2 s[4] = {b.P(x0, y - .1f, ZBm), b.P(x1, y - .1f, ZBm), b.P(x1, y - .3f, ZBm), b.P(x0, y - .3f, ZBm)};
  b.poly(s, 4, 0, .3f);
  b.box(x0, x1, y - .08f, y, ZBm - .35f, ZBm, .45f, false);
  return {x0, x1, y, true};
}

void bookcase(B& b) {
  const float x0 = 1.6f, x1 = 3.0f, z0 = ZBm - .55f;
  b.key(ZBm - .3f);
  b.box(x0, x1, 0, 2.6f, z0, ZBm, .4f);
  for (int k = 0; k < 4; ++k) {
    const float y = .15f + static_cast<float>(k) * .6f;
    b.quad3(x0 + .08f, y, z0, x1 - .08f, y, z0, x1 - .08f, y + .52f, z0, x0 + .08f, y + .52f, z0, .12f);
    float xx = x0 + .1f;
    while (xx < x1 - .14f) {
      const float bw = b.rand(.06f, .12f), bh = b.rand(.32f, .5f);
      b.quad3(xx, y, z0 + .05f, xx + bw, y, z0 + .05f, xx + bw, y + bh, z0 + .05f, xx, y + bh, z0 + .05f,
              b.rand(.3f, .85f));
      xx += bw + .005f;
    }
    b.quad3(x0 + .05f, y - .05f, z0, x1 - .05f, y - .05f, z0, x1 - .05f, y, z0, x0 + .05f, y, z0, .55f);
  }
}

void trophy(B& b, const ShelfInfo& sh) {
  const float x = sh.x1 - .25f, z = ZBm - .18f;
  b.key(z - .01f);
  const auto bb = b.bb(x, sh.y, z);
  const V2 base[4] = {{-12, -10}, {12, -10}, {12, 0}, {-12, 0}};
  b.bbPoly(bb, base, 4, .13f);
  const V2 stem[4] = {{-4, -22}, {4, -22}, {4, -10}, {-4, -10}};
  b.bbPoly(bb, stem, 4, .4f);
  V2 cup[12];
  for (int i = 0; i <= 10; ++i) {
    const float a = static_cast<float>(M_PI) * static_cast<float>(i) / 10.0f;
    cup[i] = {18 * std::cos(a), -52 + 30 * std::sin(a)};
  }
  const V2 l = B::L(bb, -18, 0), rr = B::L(bb, 18, 0);
  b.bbPoly(bb, cup, 11, .85f, 1, b.cylPaint(l.x, rr.x, .85f));
  b.bbLine(bb, -18, -48, -24, -36, 2.5f, .2f);
  b.bbLine(bb, 18, -48, 24, -36, 2.5f, .2f);
}

void badgeCase(B& b, uint8_t earned) {
  const float x = Wm - .02f, z0 = 5.6f, z1 = 7.0f, y0 = 1.7f, y1 = 2.6f;
  b.key(6.6f);
  const V2 q[4] = {b.P(x, y0, z0), b.P(x, y0, z1), b.P(x, y1, z1), b.P(x, y1, z0)};
  const V2 s[4] = {
      {q[0].x - 3, q[0].y + 3}, {q[1].x - 3, q[1].y + 3}, {q[2].x - 3, q[2].y + 3}, {q[3].x - 3, q[3].y + 3}};
  b.poly(s, 4, 0, .35f);
  b.poly(q, 4, .22f);
  for (int i = 0; i < 8; ++i) {
    const float zz = z0 + .18f + static_cast<float>(i % 4) * .34f;
    const float yy = y1 - .25f - static_cast<float>(i / 4) * .42f;
    const V2 c = b.P(x, yy, zz);
    const float r = b.scaleAt(zz) * .1f;
    if (i < earned) {
      const Stop st[2] = {{0, 1}, {1, .55f}};
      b.ellipse(c, r * .55f, r, .8f, 1, b.radial(c, r * .55f, r, st, 2));
    } else {
      b.ellipse(c, r * .55f, r, .47f, .5f);
    }
  }
}

void columns(B& b) {
  for (const float x : {-Wm + .35f, Wm - .35f}) {
    const float z = ZBm - .4f;
    b.key(z);
    b.cylinder(x, z, .26f, .3f, CHm - .35f, .85f, -1, false);
    b.box(x - .38f, x + .38f, 0, .3f, z - .38f, z + .38f, .8f);
    b.box(x - .36f, x + .36f, CHm - .35f, CHm - .2f, z - .36f, z + .36f, .8f, false);
  }
}

void pedestal(B& b) {
  const float x = -1.95f, z = 6.3f;
  b.key(z);
  b.box(x - .32f, x + .32f, 0, 1.0f, z - .32f, z + .32f, .85f);
  b.box(x - .38f, x + .38f, 1.0f, 1.08f, z - .38f, z + .38f, .75f, false);
  const auto bb = b.bb(x, 1.08f, z);
  b.glow(B::L(bb, 0, -40), 55 * bb.s, 55 * bb.s, .45f);
  const V2 crown[7] = {{-30, -10}, {-34, -46}, {-16, -30}, {0, -56}, {16, -30}, {34, -46}, {30, -10}};
  const V2 l = B::L(bb, -34, 0), r = B::L(bb, 34, 0);
  b.bbPoly(bb, crown, 7, .8f, 1, b.cylPaint(l.x, r.x, .8f));
  const V2 cushion[4] = {{-26, 0}, {26, 0}, {20, -10}, {-20, -10}};
  b.bbPoly(bb, cushion, 4, .27f);
  b.bbEllipse(bb, 0, -26, 5, 5, 1);
}

void sleepZ(B& b, float x, float y, float size) {
  // A white "Z" with a dark outline.
  const V2 z[10] = {{x, y},
                    {x + size, y},
                    {x + size, y + size * .2f},
                    {x + size * .35f, y + size * .8f},
                    {x + size, y + size * .8f},
                    {x + size, y + size},
                    {x, y + size},
                    {x, y + size * .8f},
                    {x + size * .65f, y + size * .2f},
                    {x, y + size * .2f}};
  V2 outline[10];
  for (int i = 0; i < 10; ++i) outline[i] = z[i];
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      for (int i = 0; i < 10; ++i) outline[i] = {z[i].x + dx * 1.5f, z[i].y + dy * 1.5f};
      b.poly(outline, 10, .05f);
    }
  }
  b.poly(z, 10, 1.0f);
}

}  // namespace

void SleepRoomBuilder::build() {
  const uint8_t tier = clampi(spec_.tier, 1, SLEEP_ROOM_TIER_COUNT);
  const PokemonType primary = normalizedType(spec_.primary);
  const PokemonType secondary = spec_.secondary == PokemonType::None ? primary : normalizedType(spec_.secondary);
  const bool dual = secondary != primary;

  if (tier >= 3) backWindow(*this, primary, tier >= 6);
  if (tier >= 2) rug(*this, tier);
  ceilingLight(*this, tier);
  if (tier >= 4) {
    bookcase(*this);
    taken_[BigRight] = true;
    shelf_ = {1.6f, 3.0f, 2.6f, true};
  } else if (tier >= 3) {
    shelf_ = wallShelf(*this);
  }
  if (tier >= 5 && shelf_.valid) {
    trophy(*this, shelf_);
    shelfRightUsed_ = true;
  }
  if (tier >= 6) {
    badgeCase(*this, spec_.badges);
    taken_[SideRight] = true;
  }
  if (tier >= 7) {
    columns(*this);
    pedestal(*this);
    taken_[FloorLeft] = true;
  }
  bedTop_ = bed(*this, tier);

  // Type furniture, alternating primary/secondary for dual types.
  const TypeItems lists[2] = {typeItems(primary), typeItems(secondary)};
  uint8_t next[2] = {0, 0};
  static constexpr uint8_t BUDGET[SLEEP_ROOM_TIER_COUNT] = {2, 3, 4, 5, 6, 7, 7};
  uint8_t placed = 0;
  int turn = 0;
  const int listCount = dual ? 2 : 1;
  while (placed < BUDGET[tier - 1]) {
    bool any = false;
    for (int i = 0; i < listCount; ++i) any = any || next[i] < lists[i].count;
    if (!any) break;
    const int li = turn % listCount;
    ++turn;
    if (next[li] >= lists[li].count) continue;
    const TypeItem& item = lists[li].items[next[li]++];
    for (const Slot slot : item.slots) {
      if (slot == SLOT_COUNT) break;
      if (slot == Shelf) {
        if (!shelf_.valid || item.shelfFn == nullptr) continue;
        const bool left = !shelfLeftUsed_;
        if (!left && shelfRightUsed_) continue;
        item.shelfFn(*this, shelf_, left);
        (left ? shelfLeftUsed_ : shelfRightUsed_) = true;
      } else {
        if (taken_[slot] || item.fn == nullptr) continue;
        item.fn(*this, slot);
        taken_[slot] = true;
      }
      ++placed;
      break;
    }
  }

  // Sprite contact shadow on the bed, then the "z Z" by the top-right of the
  // sprite's visible part.
  const float sz = 4.7f;
  const V2 c = P(0, bedTop_, sz);
  float shownW = 100, shownH = 90;
  if (r_.drawW_ > 0 && r_.sprite_.width > 0) {
    shownW = static_cast<float>((r_.opaqueRight_ - r_.opaqueLeft_ + 1) * r_.drawW_) / r_.sprite_.width;
    shownH = static_cast<float>((r_.opaqueBottom_ - r_.opaqueTop_ + 1) * r_.drawH_) / r_.sprite_.height;
  }
  keyRaw(4705);
  softShadow({c.x, c.y + 2}, std::max(scaleAt(sz) * .75f, shownW * .55f), 10 + shownW * .04f, .5f);
  keyRaw(-500);
  const float zs = r_.fullScreen_ ? 1.5f : 1.0f;
  const float zx = c.x + shownW * .42f, zy = c.y - shownH;
  sleepZ(*this, zx, zy - 6 * zs, 10 * zs);
  sleepZ(*this, zx + 14 * zs, zy - 26 * zs, 14 * zs);

  r_.spriteBaseX_ = static_cast<int>(std::lround(c.x));
  r_.spriteBaseY_ = static_cast<int>(std::lround(c.y));
  r_.spriteKey_ = 4700;
  r_.hasWindow_ = tier >= 3;
}

// ---------------------------------------------------------------------------
// Renderer

SleepRoomRenderer::~SleepRoomRenderer() { release(); }

void SleepRoomRenderer::release() {
  delete[] prims_;
  delete[] verts_;
  delete[] paints_;
  delete[] sideT_;
  sideT_ = nullptr;
  prims_ = nullptr;
  verts_ = nullptr;
  paints_ = nullptr;
  primCount_ = vertCount_ = paintCount_ = 0;
}

void SleepRoomRenderer::setSprite(const SleepRoomSprite& sprite) {
  sprite_ = sprite;
  drawW_ = drawH_ = 0;
  const bool gray = sprite.gray4 != nullptr;
  if (sprite.width <= 0 || sprite.height <= 0 || (!gray && (sprite.ink == nullptr || sprite.opaque == nullptr))) {
    sprite_ = {};
    return;
  }
  // Bounding box of the Pokemon itself, so it stands on the bed whatever
  // margin the artwork keeps around it.
  opaqueLeft_ = sprite.width;
  opaqueTop_ = sprite.height;
  opaqueRight_ = opaqueBottom_ = -1;
  for (int y = 0; y < sprite.height; ++y) {
    for (int x = 0; x < sprite.width; ++x) {
      bool solid;
      if (gray) {
        const uint8_t byte = sprite.gray4[y * ((sprite.width + 1) / 2) + x / 2];
        solid = ((x & 1) ? (byte & 0x0F) : (byte >> 4)) != SLEEP_ROOM_GRAY4_TRANSPARENT;
      } else {
        solid = (sprite.opaque[y * ((sprite.width + 7) / 8) + x / 8] >> (7 - (x & 7))) & 1;
      }
      if (!solid) continue;
      opaqueLeft_ = std::min(opaqueLeft_, x);
      opaqueRight_ = std::max(opaqueRight_, x);
      opaqueTop_ = std::min(opaqueTop_, y);
      opaqueBottom_ = std::max(opaqueBottom_, y);
    }
  }
  if (opaqueRight_ < 0) {
    sprite_ = {};
    return;
  }
}

bool SleepRoomRenderer::build(const SleepRoomSpec& spec, const int screenWidth, const int screenHeight) {
  release();
  spec_ = spec;
  spec_.tier = static_cast<uint8_t>(clampi(spec.tier, 1, SLEEP_ROOM_TIER_COUNT));
  width_ = screenWidth;
  height_ = screenHeight;
  ballCx_ = screenWidth / 2;
  fullScreen_ = spec.layout == SleepRoomLayout::FullScreen;
  if (fullScreen_) {
    // Closer camera: the room fills the screen and the bed sits above the
    // label panel.
    f_ = std::max(1, screenWidth * 7 / 8);
    vy_ = screenHeight * 34 / 100;
    sceneH_ = screenHeight;
    panelBottom_ = screenHeight - 14;
    panelTop_ = screenHeight - 168;
  } else {
    f_ = BALL_F;
    vy_ = BALL_VY;
    sceneH_ = BALL_SCENE_H;
    panelTop_ = panelBottom_ = 0;
  }
  // On-screen size of the sleeping Pokemon: portraits are drawn large, 1-bit
  // sprites at a whole multiple of their size so their pixels stay crisp.
  if (sprite_.width > 0) {
    if (sprite_.gray4 != nullptr) {
      const int target = fullScreen_ ? screenWidth * 5 / 8 : 170;
      drawH_ = target;
      drawW_ = sprite_.width * target / sprite_.height;
    } else {
      const int scale = fullScreen_ ? 2 : 1;
      drawW_ = sprite_.width * scale;
      drawH_ = sprite_.height * scale;
    }
  }
  overflow_ = false;
  prims_ = new (std::nothrow) Prim[MAX_PRIMS];
  verts_ = new (std::nothrow) Vertex[MAX_VERTS];
  paints_ = new (std::nothrow) Paint[MAX_PAINTS];
  sideT_ = new (std::nothrow) int32_t[screenWidth > 0 ? screenWidth : 1];
  if (prims_ == nullptr || verts_ == nullptr || paints_ == nullptr || sideT_ == nullptr) {
    release();
    return false;
  }
  for (int x = 0; x < screenWidth; ++x) {
    const int hx = std::abs(2 * x + 1 - 2 * ballCx_);
    sideT_[x] = hx == 0 ? INT32_MAX : W_MM * 2 * f_ / hx;
  }
  SleepRoomBuilder builder(*this, spec_);
  builder.build();
  // Painter order: farthest first. Stable insertion sort keeps each item's own
  // drawing order intact.
  for (size_t i = 1; i < primCount_; ++i) {
    const Prim p = prims_[i];
    size_t j = i;
    while (j > 0 && prims_[j - 1].key < p.key) {
      prims_[j] = prims_[j - 1];
      --j;
    }
    prims_[j] = p;
  }
  return true;
}

int SleepRoomRenderer::sceneHeight() const { return sceneH_; }
int SleepRoomRenderer::buttonCenterX() const { return ballCx_; }
int SleepRoomRenderer::buttonCenterY() const { return BALL_CY + BALL_R - 16; }
int SleepRoomRenderer::buttonRadius() const { return BUTTON_R; }

uint8_t SleepRoomRenderer::shadeRoom(const int x, const int y, const int rowT, const int rowSurface) const {
  // Ray through the pixel centre, in half-pixel units. The floor/ceiling depth
  // depends only on the row and the side-wall depth only on the column, so
  // both divisions are hoisted out of the per-pixel path.
  const int hx = 2 * x + 1 - 2 * ballCx_;
  const int hy = 2 * vy_ - (2 * y + 1);
  const int F2 = 2 * f_;
  int t = rowT;
  int surface = rowSurface;  // 0 back, 1 floor, 2 ceiling, 3 left, 4 right
  const int ts = sideT_[x];
  if (ts < t) {
    t = ts;
    surface = hx < 0 ? 3 : 4;
  }
  const int wx = t * hx / F2;         // mm
  const int wy = H_MM + t * hy / F2;  // mm
  const int wz = t;
  const int mpp = std::max(1, t / f_);
  const uint8_t tier = spec_.tier;
  const uint32_t seed = spec_.seed;
  int lum = 0;
  int ao = 0;  // distance (mm) to the nearest room edge on this surface

  if (surface == 1) {
    const int u = wx + W_MM;
    if (tier == 1) {
      // straw
      const uint32_t h = hash3(floordiv(u + triWave(wz, 900, 60), 26), floordiv(wz, 160), seed);
      const uint32_t h2 = hash3(floordiv(u, 400), floordiv(wz, 400), seed ^ 0x99u);
      lum = 104 + static_cast<int>(h % 46) + static_cast<int>(h2 % 24) - 12;
    } else if (tier >= 6) {
      const int tx = floordiv(u, 800), tz = floordiv(wz, 800);
      const bool dark = ((tx + tz) & 1) != 0;
      const int gx = floormod(u, 800), gz = floormod(wz, 800);
      const int grout = std::max(8, mpp * 2 / 3);
      if (gx < grout || gz < grout) {
        lum = 153;
      } else {
        lum = dark ? 107 : 230;
        const uint32_t h = hash3(tx, tz, seed);
        const int vein = floormod(gx * 3 + gz * 2 + static_cast<int>(h % 1000), 1100);
        if (vein < std::max(12, mpp)) lum += dark ? 40 : -45;
      }
    } else {
      int width = 240, lo = 148, hi = 184, gap = 77;
      if (tier == 2) {
        width = 420;
        lo = 107;
        hi = 143;
        gap = 46;
      } else if (tier == 5) {
        width = 220;
        lo = 77;
        hi = 107;
        gap = 36;
      }
      const int strip = u / width;
      const int off = static_cast<int>(hash3(strip, 3, seed) % 1800);
      const int seg = (wz + off) / 2000;
      lum = lo + static_cast<int>(hash3(strip, seg, seed) % (hi - lo));
      lum += triWave(wz + strip * 311, 700, 5);
      const int gw = std::max(8, mpp * 7 / 10);
      if (u % width < gw || (wz + off) % 2000 < gw) lum = gap;
    }
    if (hasWindow_) {
      // light pool from the window
      if (wz > 4000 && wz < 6600) {
        const int halfW = 1100 + (6200 - wz) * 400 / 1800;
        const int edgeX = halfW - std::abs(wx);
        const int edgeZ = std::min(wz - 4400, 6200 - wz);
        const int soft = std::min(edgeX, edgeZ);
        if (soft > -300) {
          const int k = clampi((soft + 300) * 255 / 600, 0, 255);
          lum = blend(static_cast<uint8_t>(clampi(lum, 0, 255)), 255, k * 56 / 255);
        }
      }
    }
    ao = std::min({wx + W_MM, W_MM - wx, ZB_MM - wz});
  } else if (surface == 2) {
    lum = 140 * 3 / 4;
    if (tier >= 5) {
      for (const int bz : {2500, 4000, 5500, 7000}) {
        if (wz >= bz && wz < bz + 220) {
          lum = (wz - bz < std::max(10, mpp)) ? 80 : 120;
        }
      }
    }
    ao = std::min({wx + W_MM, W_MM - wx, ZB_MM - wz});
  } else {
    // walls
    const bool back = surface == 0;
    const PokemonType type =
        back || spec_.secondary == PokemonType::None ? normalizedType(spec_.primary) : normalizedType(spec_.secondary);
    const int u = back ? wx : wz;
    const int v = wy;
    lum = wallMaterial(materialFor(type), u, v, mpp, seed + (back ? 0 : 77));
    if (tier >= 6 && v < 1150) {
      if (v >= 1050) {
        lum = 209;
      } else {
        lum = 128;
        const int start = back ? -W_MM : 1000;
        const int pu = floormod(u - start - 150, 700);
        const int lw = std::max(mpp, 8);
        if (v > 250 && v < 900 && pu < 500 && (pu < lw || pu > 500 - lw || v < 250 + lw || v > 900 - lw)) lum = 82;
      }
    }
    if (tier >= 7 && v > CH_MM - 300) lum = (v < CH_MM - 250) ? 115 : 230;
    if (v < 120) lum = tier >= 6 ? 224 : 77;
    if (!back) {
      // side walls darken toward the viewer
      const int k = clampi((ZB_MM - wz) * 255 / (ZB_MM - 1000), 0, 255);
      lum = darken(lum, 13 + k * 102 / 255);
      ao = std::min({wy, CH_MM - wy, ZB_MM - wz});
    } else {
      ao = std::min({wy, CH_MM - wy, W_MM - std::abs(wx)});
    }
  }
  if (surface == 2) lum = darken(lum, 64);
  if (ao < 380) lum = darken(lum, (380 - std::max(ao, 0)) * 100 / 380);
  return static_cast<uint8_t>(clampi(lum, 0, 255));
}

uint8_t SleepRoomRenderer::shadeShell(const int x, const int y, uint8_t room) const {
  // Distances in half pixels; compared squared so that most pixels need no
  // square root (the ESP32-C3 has no FPU and a slow software isqrt path).
  const int dx2 = 2 * x + 1 - 2 * ballCx_;
  const int dy2 = 2 * y + 1 - 2 * BALL_CY;
  const int d2 = dx2 * dx2 + dy2 * dy2;
  constexpr int R2 = 4 * BALL_R * BALL_R;
  constexpr int W2 = 4 * WIN_R * WIN_R;
  int lum = room;

  if (d2 > R2) {
    lum = 255;
    if (y > BALL_CY + BALL_R - 24) {
      // ground shadow under the ball
      const int sx = (2 * x + 1 - 2 * (ballCx_ + 8)) * 256 / (2 * 175);
      const int sy = (2 * y + 1 - 2 * (BALL_CY + BALL_R + 2)) * 256 / (2 * 16);
      const int q2 = sx * sx + sy * sy;
      if (q2 < 256 * 256) {
        const int q = static_cast<int>(isqrt32(static_cast<uint32_t>(q2)));
        lum = darken(lum, (256 - q) * 85 / 256);
      }
    }
  } else if (d2 > W2) {
    if (d2 <= 4 * (WIN_R + RIM_W) * (WIN_R + RIM_W)) {
      // inner rim, lit from below
      lum = 17 + clampi((y - (BALL_CY - WIN_R)) * 172 / (2 * WIN_R), 0, 172);
    } else if (std::abs(dy2) <= 30) {
      lum = (y < BALL_CY - 11) ? 90 : 20;
    } else if (y < BALL_CY) {
      const int gx = x - (ballCx_ - 90), gy = y - (BALL_CY - 160);
      const int t = static_cast<int>(isqrt32(static_cast<uint32_t>(gx * gx + gy * gy))) * 255 / 330;
      lum = t < 115 ? 154 - (154 - 90) * t / 115 : 90 - (90 - 30) * clampi(t - 115, 0, 140) / 140;
      // specular highlight
      const int hx = (x - (ballCx_ - 112)) * 256 / 44, hy = (y - (BALL_CY - 150)) * 256 / 24;
      const int hq2 = hx * hx + hy * hy;
      if (hq2 < 256 * 256) {
        const int hq = static_cast<int>(isqrt32(static_cast<uint32_t>(hq2)));
        lum = blend(static_cast<uint8_t>(lum), 255, (256 - hq) * 150 / 256);
      }
    } else {
      const int gx = x - (ballCx_ - 70), gy = y - (BALL_CY + 30);
      const int t = static_cast<int>(isqrt32(static_cast<uint32_t>(gx * gx + gy * gy))) * 255 / 330;
      lum = t < 140 ? 255 - 25 * t / 140 : 230 - 90 * clampi(t - 140, 0, 115) / 115;
    }
  } else {
    // inside the window: vignette from 60% of the radius outwards
    constexpr int V0 = W2 * 36 / 100;
    if (d2 > V0) lum = darken(lum, (d2 - V0) / ((W2 - V0) / 100));
    // a faint reflection on the glass, upper left
    if (dx2 < 0 && dy2 < 0 && dy2 * 2 < dx2 / 3 && dx2 * 3 < dy2) {
      constexpr int G = 2 * (WIN_R - 20);
      if (d2 > (G - 10) * (G - 10) && d2 < (G + 10) * (G + 10)) {
        const int ringDist = std::abs(static_cast<int>(isqrt32(static_cast<uint32_t>(d2))) - G);
        if (ringDist < 10) lum = blend(static_cast<uint8_t>(lum), 255, 46 - ringDist * 4);
      }
    }
  }
  // outlines
  if (d2 >= (2 * BALL_R - 4) * (2 * BALL_R - 4) && d2 <= (2 * BALL_R + 4) * (2 * BALL_R + 4)) lum = 0;
  if (d2 >= (2 * WIN_R - 3) * (2 * WIN_R - 3) && d2 <= (2 * WIN_R + 3) * (2 * WIN_R + 3)) lum = 0;
  // button
  const int by2 = 2 * y + 1 - 2 * buttonCenterY();
  constexpr int BO = 2 * (BUTTON_R + 8);
  if (std::abs(by2) <= BO && std::abs(dx2) <= BO) {
    const int bd2 = dx2 * dx2 + by2 * by2;
    if (bd2 <= BO * BO) {
      if (bd2 > (2 * BUTTON_R + 2) * (2 * BUTTON_R + 2)) {
        lum = 20;
      } else if (bd2 >= (2 * BUTTON_R - 2) * (2 * BUTTON_R - 2)) {
        lum = 0;
      } else {
        const int gx = x - (ballCx_ - BUTTON_R * 24 / 100), gy = y - (buttonCenterY() - BUTTON_R * 36 / 100);
        const int t = static_cast<int>(isqrt32(static_cast<uint32_t>(gx * gx + gy * gy))) * 255 / (BUTTON_R * 8 / 5);
        lum = t < 153 ? 255 - 31 * t / 153 : 224 - 86 * clampi(t - 153, 0, 102) / 102;
      }
    }
  }
  return static_cast<uint8_t>(clampi(lum, 0, 255));
}

namespace {
inline void evalStops(const SleepRoomRenderer::Paint& p, int t, int& lum, int& alpha);
}

void SleepRoomRenderer::drawPrimsRow(const int y, uint8_t* out, const int x0, const int x1, const int keyFrom,
                                     const int keyTo, size_t& index) const {
  const int y4 = y * 4 + 2;
  for (; index < primCount_; ++index) {
    const Prim& p = prims_[index];
    if (p.key <= keyTo) return;
    (void)keyFrom;
    if (y < p.yMin || y > p.yMax || y < p.clipY0 || y >= p.clipY1) continue;
    const int cx0 = std::max<int>(x0, p.clipX0);
    const int cx1 = std::min<int>(x1, p.clipX1);
    if (cx0 >= cx1) continue;
    int spans[16];
    int ns = 0;
    if (p.shape == 1) {
      const int dy = y4 - p.ey;
      const int ry = p.ery;
      if (dy * dy >= ry * ry) continue;
      const int hx =
          static_cast<int>(static_cast<int64_t>(p.erx) * isqrt32(static_cast<uint32_t>(ry * ry - dy * dy)) / ry);
      spans[ns++] = p.ex - hx;
      spans[ns++] = p.ex + hx;
    } else {
      const Vertex* v = verts_ + p.v;
      for (int i = 0; i < p.nv && ns < 16; ++i) {
        const Vertex& a = v[i];
        const Vertex& b = v[(i + 1) % p.nv];
        if ((a.y <= y4) == (b.y <= y4)) continue;
        spans[ns++] = a.x + (y4 - a.y) * (b.x - a.x) / (b.y - a.y);
      }
      ns &= ~1;
      for (int i = 1; i < ns; ++i) {
        const int k = spans[i];
        int j = i;
        while (j > 0 && spans[j - 1] > k) {
          spans[j] = spans[j - 1];
          --j;
        }
        spans[j] = k;
      }
    }
    for (int s = 0; s + 1 < ns; s += 2) {
      // pixel x covered when its centre (x*4+2) lies in [a, b)
      int sx = (spans[s] - 2 + 3) >= 0 ? (spans[s] - 2 + 3) / 4 : -((-(spans[s] - 2 + 3) + 3) / 4);
      int ex = (spans[s + 1] - 2 + 3) >= 0 ? (spans[s + 1] - 2 + 3) / 4 : -((-(spans[s + 1] - 2 + 3) + 3) / 4);
      sx = std::max(sx, cx0);
      ex = std::min(ex, cx1);
      if (sx >= ex) continue;
      if (p.paint == 0xFFFF) {
        for (int x = sx; x < ex; ++x) out[x] = blend(out[x], p.lum, p.alpha);
      } else {
        // Gradients advance in 16.16 fixed point along the span, so a pixel
        // costs additions rather than 64-bit divisions.
        const Paint& pt = paints_[p.paint];
        int lum, alpha;
        if (pt.kind == 1) {
          const int64_t dx = pt.x1 - pt.x0, dy = pt.y1 - pt.y0;
          const int64_t num = static_cast<int64_t>(sx * 4 + 2 - pt.x0) * dx + static_cast<int64_t>(y4 - pt.y0) * dy;
          int64_t tF = num * (255LL << 16) / pt.len2;
          const int64_t step = 4 * dx * (255LL << 16) / pt.len2;
          for (int x = sx; x < ex; ++x, tF += step) {
            evalStops(pt, clampi(static_cast<int>(tF >> 16), 0, 255), lum, alpha);
            alpha = alpha * p.alpha / 255;
            if (alpha > 0) out[x] = blend(out[x], lum, alpha);
          }
        } else {
          const int ny = (y4 - pt.y0) * 256 / pt.y1;
          const int ny2 = ny * ny;
          int64_t nxF = static_cast<int64_t>(sx * 4 + 2 - pt.x0) * (256LL << 16) / pt.x1;
          const int64_t step = (4LL * 256 << 16) / pt.x1;
          for (int x = sx; x < ex; ++x, nxF += step) {
            const int nx = static_cast<int>(nxF >> 16);
            const uint32_t q2 = static_cast<uint32_t>(nx * nx + ny2);
            const int t = q2 >= 255u * 255u ? 255 : static_cast<int>(isqrt32(q2));
            evalStops(pt, t, lum, alpha);
            alpha = alpha * p.alpha / 255;
            if (alpha > 0) out[x] = blend(out[x], lum, alpha);
          }
        }
      }
    }
  }
}

namespace {
inline void evalStops(const SleepRoomRenderer::Paint& p, const int t, int& lum, int& alpha) {
  if (p.stops == 0) {
    lum = 0;
    alpha = 0;
    return;
  }
  if (t <= p.pos[0]) {
    lum = p.lum[0];
    alpha = p.alpha[0];
    return;
  }
  for (int i = 1; i < p.stops; ++i) {
    if (t <= p.pos[i]) {
      const int span = std::max(1, p.pos[i] - p.pos[i - 1]);
      const int f = (t - p.pos[i - 1]) * 256 / span;
      lum = p.lum[i - 1] + (p.lum[i] - p.lum[i - 1]) * f / 256;
      alpha = p.alpha[i - 1] + (p.alpha[i] - p.alpha[i - 1]) * f / 256;
      return;
    }
  }
  lum = p.lum[p.stops - 1];
  alpha = p.alpha[p.stops - 1];
}
}  // namespace

void SleepRoomRenderer::drawSpriteRow(const int y, uint8_t* out, const int x0, const int x1) const {
  if (drawW_ <= 0 || drawH_ <= 0 || sprite_.width <= 0) return;
  const int w = sprite_.width, h = sprite_.height;
  // Bottom-centre of the opaque box sits on the bed, a little sunk into it.
  const int centreX = (opaqueLeft_ + opaqueRight_ + 1) * drawW_ / (2 * w);
  const int left = spriteBaseX_ - centreX;
  const int top = spriteBaseY_ - (opaqueBottom_ + 1) * drawH_ / h + std::max(2, drawH_ / 40);
  const int dy = y - top;
  if (dy < 0 || dy >= drawH_) return;
  // 8.8 fixed-point source position of this row.
  const int syF = dy * h * 256 / drawH_;
  const int sy = std::min(h - 1, syF >> 8);
  const int fromX = std::max(left, x0), toX = std::min(left + drawW_, x1);
  if (sprite_.gray4 != nullptr) {
    const int rowBytes = (w + 1) / 2;
    const auto at = [&](int sx, int yy) -> int {
      const uint8_t byte = sprite_.gray4[yy * rowBytes + sx / 2];
      return (sx & 1) ? (byte & 0x0F) : (byte >> 4);
    };
    const int sy2 = std::min(h - 1, sy + 1);
    const int fy = syF & 0xFF;
    for (int x = fromX; x < toX; ++x) {
      const int sxF = (x - left) * w * 256 / drawW_;
      const int sx = std::min(w - 1, sxF >> 8);
      if (at(sx, sy) == SLEEP_ROOM_GRAY4_TRANSPARENT) continue;
      // Bilinear blend of the opaque neighbours keeps the artwork smooth when
      // it is scaled.
      const int sx2 = std::min(w - 1, sx + 1);
      const int fx = sxF & 0xFF;
      const int v[4] = {at(sx, sy), at(sx2, sy), at(sx, sy2), at(sx2, sy2)};
      const int wt[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
      int sum = 0, weight = 0;
      for (int i = 0; i < 4; ++i) {
        if (v[i] == SLEEP_ROOM_GRAY4_TRANSPARENT) continue;
        sum += v[i] * wt[i];
        weight += wt[i];
      }
      out[x] = static_cast<uint8_t>(weight > 0 ? sum * 255 / (weight * 14) : 0);
    }
    return;
  }
  const int rowBytes = (w + 7) / 8;
  const uint8_t* ink = sprite_.ink + sy * rowBytes;
  const uint8_t* opaque = sprite_.opaque + sy * rowBytes;
  for (int x = fromX; x < toX; ++x) {
    const int sx = std::min(w - 1, (x - left) * w / drawW_);
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (sx & 7));
    if ((opaque[sx / 8] & mask) == 0) continue;
    out[x] = (ink[sx / 8] & mask) ? 24 : 238;
  }
}

uint8_t SleepRoomRenderer::shadeFrame(const int x, const int y, uint8_t room) const {
  // Full-screen layout: a soft vignette towards the edges, then the label
  // panel (white, rounded, outlined, with a drop shadow).
  int lum = room;
  const int edge = std::min({x, width_ - 1 - x, y, sceneH_ - 1 - y});
  if (edge < 70) lum = darken(lum, (70 - edge) * 90 / 70);
  constexpr int R = 14, SHADOW = 5;
  const int px0 = 14, px1 = width_ - 15;
  const auto inside = [&](int xx, int yy, int grow) {
    const int l = px0 - grow, r = px1 + grow, t = panelTop_ - grow, b = panelBottom_ + grow;
    if (xx < l || xx > r || yy < t || yy > b) return false;
    const int cx = xx < l + R ? l + R : (xx > r - R ? r - R : xx);
    const int cy = yy < t + R ? t + R : (yy > b - R ? b - R : yy);
    return (xx - cx) * (xx - cx) + (yy - cy) * (yy - cy) <= R * R;
  };
  if (panelBottom_ > panelTop_) {
    if (inside(x - SHADOW / 2, y - SHADOW, 0) && !inside(x, y, 0)) lum = darken(lum, 110);
    if (inside(x, y, 0)) lum = inside(x, y, -2) ? 250 : 0;
  }
  return static_cast<uint8_t>(clampi(lum, 0, 255));
}

void SleepRoomRenderer::renderRow(const int y, uint8_t* out) const {
  if (out == nullptr || width_ <= 0) return;
  if (y < 0 || y >= sceneH_) {
    std::memset(out, 255, static_cast<size_t>(width_));
    return;
  }
  // Visible span of the room on this row: the ball's window, or the whole row.
  int wx0 = 0, wx1 = 0;
  if (fullScreen_) {
    wx1 = width_;
  } else {
    const int dy2 = 2 * y + 1 - 2 * BALL_CY;
    if (dy2 * dy2 < 4 * WIN_R * WIN_R) {
      const int half2 = static_cast<int>(isqrt32(static_cast<uint32_t>(4 * WIN_R * WIN_R - dy2 * dy2)));  // half px
      wx0 = std::max(0, (2 * ballCx_ - half2) / 2);
      wx1 = std::min(width_, (2 * ballCx_ + half2 + 1) / 2);
    }
  }
  // Floor or ceiling depth for this row (back wall when nearer).
  const int hy = 2 * vy_ - (2 * y + 1);
  int rowT = ZB_MM, rowSurface = 0;
  if (hy < 0 && H_MM * 2 * f_ / (-hy) < rowT) {
    rowT = H_MM * 2 * f_ / (-hy);
    rowSurface = 1;
  } else if (hy > 0 && (CH_MM - H_MM) * 2 * f_ / hy < rowT) {
    rowT = (CH_MM - H_MM) * 2 * f_ / hy;
    rowSurface = 2;
  }
  for (int x = 0; x < width_; ++x) out[x] = (x >= wx0 && x < wx1) ? shadeRoom(x, y, rowT, rowSurface) : 255;
  if (wx1 > wx0 && prims_ != nullptr) {
    size_t index = 0;
    drawPrimsRow(y, out, wx0, wx1, 32767, spriteKey_, index);
    drawSpriteRow(y, out, wx0, wx1);
    drawPrimsRow(y, out, wx0, wx1, spriteKey_, -32768, index);
  }
  if (fullScreen_) {
    for (int x = 0; x < width_; ++x) out[x] = shadeFrame(x, y, out[x]);
  } else {
    for (int x = 0; x < width_; ++x) out[x] = shadeShell(x, y, out[x]);
  }
}

}  // namespace pokemon

#endif  // CROSSINK_ENABLE_POKEMON
