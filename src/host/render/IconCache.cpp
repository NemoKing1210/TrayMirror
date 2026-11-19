#include "render/IconCache.h"

#include <tuple>
#include <utility>

namespace traymirror {

bool IconCacheKey::operator<(const IconCacheKey& other) const noexcept {
  return std::tie(icon, serial, px) <
         std::tie(other.icon, other.serial, other.px);
}

IconCache::IconCache(BitmapFactory factory) : factory_(std::move(factory)) {}

ID2D1Bitmap* IconCache::Get(HICON icon, UINT serial, int px) {
  if (icon == nullptr || !factory_) {
    return nullptr;
  }
  const IconCacheKey key{.icon = icon, .serial = serial, .px = px};
  const auto existing = bitmaps_.find(key);
  if (existing != bitmaps_.end()) {
    return existing->second.Get();
  }
  Microsoft::WRL::ComPtr<ID2D1Bitmap> created;
  created.Attach(factory_(icon, px));
  if (created == nullptr) {
    return nullptr;
  }
  ID2D1Bitmap* raw = created.Get();
  bitmaps_.emplace(key, std::move(created));
  return raw;
}

void IconCache::Prune(const std::set<IconCacheKey>& live) {
  for (auto it = bitmaps_.begin(); it != bitmaps_.end();) {
    if (live.contains(it->first)) {
      ++it;
    } else {
      it = bitmaps_.erase(it);
    }
  }
}

}  // namespace traymirror
