#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::runtime {

class ApplicationScene;

enum class UiNodeKind : uint8_t {
  Container = 0,
  Text = 1,
  Canvas = 2,
};

struct UiNodeRecord {
  uint32_t id = 0;
  uint32_t parent = UINT32_MAX;
  uint8_t kind = 0;
  uint8_t reserved[3] = {};
  uint32_t class_offset = 0;
  uint32_t class_length = 0;
  uint32_t text_offset = 0;
  uint32_t text_length = 0;
  uint32_t canvas = 0;
};

static_assert(sizeof(UiNodeRecord) == 32, "native UI node wire layout changed");

enum class UiFlexDirection : uint8_t { Row, Column };
enum class UiAlign : uint8_t { Start, Center, End, Stretch };
enum class UiJustify : uint8_t { Start, Center, End, SpaceBetween };

struct UiStyle {
  float width = -1;
  float height = -1;
  float min_width = 0;
  float min_height = 0;
  float max_width = -1;
  float max_height = -1;
  float margin[4] = {};
  float padding[4] = {};
  float gap = 0;
  float grow = 0;
  float shrink = 1;
  float radius = 0;
  float font_size = 14;
  uint32_t background_rgba = 0;
  uint32_t text_rgba = 0xff000000u;
  UiFlexDirection direction = UiFlexDirection::Column;
  UiAlign align = UiAlign::Stretch;
  UiJustify justify = UiJustify::Start;
  bool visible = true;
  bool has_background = false;
  bool has_text_color = false;
};

struct UiLayoutRect {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
};

bool parseTailwindClasses(const char *classes, size_t size, UiStyle &style,
                          std::string &error);

class NativeUiEngine {
public:
  explicit NativeUiEngine(ApplicationScene &scene);
  ~NativeUiEngine();

  NativeUiEngine(const NativeUiEngine &) = delete;
  NativeUiEngine &operator=(const NativeUiEngine &) = delete;

  bool submit(const UiNodeRecord *nodes, size_t node_count,
              const uint8_t *strings, size_t string_bytes);
  void clear();

  const std::vector<UiLayoutRect> &layout() const;
  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
