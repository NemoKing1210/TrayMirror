// Icon bitmap cache (spec 6.2): HICON -> ID2D1Bitmap keyed by the icon
// handle, the model's replace serial and the pixel size. The serial is
// per icon (it restarts when a new icon is added), so it is not an
// identity on its own.
#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstddef>
#include <functional>
#include <map>
#include <set>

namespace traymirror {

struct IconCacheKey {
  HICON icon = nullptr;
  UINT serial = 0;
  int px = 0;
  bool operator<(const IconCacheKey& other) const noexcept;
};

class IconCache {
 public:
  // Converts an HICON into a premultiplied BGRA bitmap of `px` pixels.
  // The cache takes ownership of the returned bitmap.
  using BitmapFactory = std::function<ID2D1Bitmap*(HICON, int)>;

  explicit IconCache(BitmapFactory factory = {});
  ~IconCache() = default;
  IconCache(const IconCache&) = delete;
  IconCache& operator=(const IconCache&) = delete;

  // Null icon (or no factory, or a failed conversion) yields nullptr;
  // the overlay then draws the placeholder.
  ID2D1Bitmap* Get(HICON icon, UINT serial, int px);
  // Releases every entry whose key is not in `live`; called once per
  // SetIcons so a replaced or removed icon does not leak its bitmap.
  void Prune(const std::set<IconCacheKey>& live);
  [[nodiscard]] std::size_t size() const noexcept { return bitmaps_.size(); }

 private:
  BitmapFactory factory_;
  std::map<IconCacheKey, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
};

}  // namespace traymirror
