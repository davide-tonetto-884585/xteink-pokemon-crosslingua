#if defined(CROSSINK_ENABLE_POKEMON)

#include "PokemonPortrait.h"

#include <HalStorage.h>

#include <cstring>
#include <new>

#include "PokemonArtPath.h"

namespace pokemon {

namespace {
constexpr int MAX_PORTRAIT = 320;
}

bool loadPokemonPortrait(const uint16_t speciesId, int& width, int& height, std::unique_ptr<uint8_t[]>& gray4) {
  char path[64]{};
  if (pokemonSleepArtPath(speciesId, path, sizeof(path)) == nullptr) return false;
  FsFile file;
  if (!Storage.openFileForRead("PKART", path, file)) return false;
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

}  // namespace pokemon

#endif
