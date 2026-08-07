#include "oos/runtime/native_ui.h"

#include "oos/runtime/application_scene.h"
#include "oos/runtime/canvas_2d.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace oos::runtime {
namespace {

constexpr size_t kMaximumUiNodes = 4096;
constexpr size_t kMaximumUiStrings = 1024 * 1024;

enum Edge : size_t { Top = 0, Right = 1, Bottom = 2, Left = 3 };

uint32_t rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) {
  return static_cast<uint32_t>(red) | (static_cast<uint32_t>(green) << 8) |
         (static_cast<uint32_t>(blue) << 16) |
         (static_cast<uint32_t>(alpha) << 24);
}

bool decimal(std::string_view value, float &result) {
  if (value.empty() || value.size() > 16)
    return false;
  std::string terminated(value);
  char *end = nullptr;
  const float parsed = std::strtof(terminated.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(parsed))
    return false;
  result = parsed;
  return true;
}

bool spacing(std::string_view value, float &result) {
  if (value == "px") {
    result = 1;
    return true;
  }
  float scale = 0;
  if (!decimal(value, scale) || scale < 0)
    return false;
  result = scale * 4;
  return true;
}

bool arbitraryPixels(std::string_view value, float &result) {
  if (value.size() < 5 || value.front() != '[' || value.back() != ']' ||
      value.substr(value.size() - 3, 2) != "px")
    return false;
  return decimal(value.substr(1, value.size() - 4), result) && result >= 0;
}

void setEdges(float edges[4], std::string_view prefix, float value) {
  if (prefix == "p" || prefix == "m") {
    std::fill(edges, edges + 4, value);
  } else if (prefix == "px" || prefix == "mx") {
    edges[Left] = value;
    edges[Right] = value;
  } else if (prefix == "py" || prefix == "my") {
    edges[Top] = value;
    edges[Bottom] = value;
  } else if (prefix == "pt" || prefix == "mt") {
    edges[Top] = value;
  } else if (prefix == "pr" || prefix == "mr") {
    edges[Right] = value;
  } else if (prefix == "pb" || prefix == "mb") {
    edges[Bottom] = value;
  } else if (prefix == "pl" || prefix == "ml") {
    edges[Left] = value;
  }
}

bool paletteColor(std::string_view name, uint32_t &color) {
  struct NamedColor {
    std::string_view name;
    uint32_t value;
  };
  constexpr NamedColor colors[] = {
      {"transparent", 0},
      {"black", 0xff000000u},
      {"white", 0xffffffffu},
      {"gray-50", 0xfffafafau},
      {"gray-100", 0xfff4f4f5u},
      {"gray-200", 0xffe4e4e7u},
      {"gray-300", 0xffd4d4d8u},
      {"gray-400", 0xffa1a1aau},
      {"gray-500", 0xff71717au},
      {"gray-600", 0xff52525bu},
      {"gray-700", 0xff3f3f46u},
      {"gray-800", 0xff27272au},
      {"gray-900", 0xff18181bu},
      {"red-500", 0xff4444efu},
      {"red-600", 0xff2626dcu},
      {"green-500", 0xff5ec522u},
      {"green-600", 0xff4aa316u},
      {"blue-500", 0xfff6823bu},
      {"blue-600", 0xffeb6325u},
      {"yellow-400", 0xff15ccfau},
      {"yellow-500", 0xff08b3eau},
  };
  for (const NamedColor &entry : colors) {
    if (entry.name == name) {
      color = entry.value;
      return true;
    }
  }
  if (name.size() == 9 && name.substr(0, 2) == "[#" && name.back() == ']') {
    uint32_t rgb = 0;
    for (size_t index = 2; index < 8; ++index) {
      const char digit = name[index];
      const uint32_t value = digit >= '0' && digit <= '9'
                                 ? digit - '0'
                             : digit >= 'a' && digit <= 'f'
                                 ? digit - 'a' + 10
                             : digit >= 'A' && digit <= 'F'
                                 ? digit - 'A' + 10
                                 : 16;
      if (value > 15)
        return false;
      rgb = (rgb << 4) | value;
    }
    color = rgba(static_cast<uint8_t>(rgb >> 16),
                 static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
    return true;
  }
  return false;
}

bool sizeUtility(std::string_view token, std::string_view prefix,
                 float &value) {
  if (token.rfind(prefix, 0) != 0)
    return false;
  const std::string_view amount = token.substr(prefix.size());
  if (amount == "full") {
    value = -2;
    return true;
  }
  if (amount == "auto") {
    value = -1;
    return true;
  }
  return arbitraryPixels(amount, value) || spacing(amount, value);
}

bool parseOne(std::string_view token, UiStyle &style) {
  if (token.empty())
    return true;
  if (token == "flex" || token == "flex-col") {
    style.direction = UiFlexDirection::Column;
    return true;
  }
  if (token == "flex-row") {
    style.direction = UiFlexDirection::Row;
    return true;
  }
  if (token == "hidden") {
    style.visible = false;
    return true;
  }
  if (token == "grow") {
    style.grow = 1;
    return true;
  }
  if (token == "grow-0") {
    style.grow = 0;
    return true;
  }
  if (token == "shrink") {
    style.shrink = 1;
    return true;
  }
  if (token == "shrink-0") {
    style.shrink = 0;
    return true;
  }
  if (token == "items-start" || token == "items-center" ||
      token == "items-end" || token == "items-stretch") {
    style.align = token == "items-center" ? UiAlign::Center
                  : token == "items-end"  ? UiAlign::End
                  : token == "items-stretch" ? UiAlign::Stretch
                                               : UiAlign::Start;
    return true;
  }
  if (token == "justify-start" || token == "justify-center" ||
      token == "justify-end" || token == "justify-between") {
    style.justify = token == "justify-center" ? UiJustify::Center
                    : token == "justify-end"  ? UiJustify::End
                    : token == "justify-between" ? UiJustify::SpaceBetween
                                                   : UiJustify::Start;
    return true;
  }
  if (sizeUtility(token, "w-", style.width) ||
      sizeUtility(token, "h-", style.height) ||
      sizeUtility(token, "min-w-", style.min_width) ||
      sizeUtility(token, "min-h-", style.min_height) ||
      sizeUtility(token, "max-w-", style.max_width) ||
      sizeUtility(token, "max-h-", style.max_height))
    return true;
  if (token.rfind("size-", 0) == 0) {
    float value = 0;
    if (!sizeUtility(token, "size-", value))
      return false;
    style.width = value;
    style.height = value;
    return true;
  }
  constexpr std::string_view spacing_prefixes[] = {
      "p-", "px-", "py-", "pt-", "pr-", "pb-", "pl-",
      "m-", "mx-", "my-", "mt-", "mr-", "mb-", "ml-"};
  for (std::string_view prefix : spacing_prefixes) {
    if (token.rfind(prefix, 0) != 0)
      continue;
    float value = 0;
    if (!spacing(token.substr(prefix.size()), value) &&
        !arbitraryPixels(token.substr(prefix.size()), value))
      return false;
    setEdges(prefix.front() == 'p' ? style.padding : style.margin,
             prefix.substr(0, prefix.size() - 1), value);
    return true;
  }
  if (token.rfind("gap-", 0) == 0)
    return spacing(token.substr(4), style.gap) ||
           arbitraryPixels(token.substr(4), style.gap);
  if (token == "rounded" || token == "rounded-md") {
    style.radius = 6;
    return true;
  }
  if (token == "rounded-none") {
    style.radius = 0;
    return true;
  }
  if (token == "rounded-sm" || token == "rounded-lg" ||
      token == "rounded-xl" || token == "rounded-2xl" ||
      token == "rounded-full") {
    style.radius = token == "rounded-sm" ? 2
                   : token == "rounded-lg" ? 8
                   : token == "rounded-xl" ? 12
                   : token == "rounded-2xl" ? 16
                                              : 10000;
    return true;
  }
  if (token == "text-xs" || token == "text-sm" || token == "text-base" ||
      token == "text-lg" || token == "text-xl" || token == "text-2xl" ||
      token == "text-3xl") {
    style.font_size = token == "text-xs" ? 12
                      : token == "text-sm" ? 14
                      : token == "text-base" ? 16
                      : token == "text-lg" ? 18
                      : token == "text-xl" ? 20
                      : token == "text-2xl" ? 24
                                             : 30;
    return true;
  }
  if (token == "font-normal" || token == "relative")
    return true;
  if (token.rfind("bg-", 0) == 0) {
    style.has_background = paletteColor(token.substr(3), style.background_rgba);
    return style.has_background;
  }
  if (token.rfind("text-", 0) == 0) {
    style.has_text_color = paletteColor(token.substr(5), style.text_rgba);
    return style.has_text_color;
  }
  return false;
}

float resolvedLength(float value, float available, float fallback) {
  if (value == -2)
    return available;
  return value >= 0 ? value : fallback;
}

} // namespace

bool parseTailwindClasses(const char *classes, size_t size, UiStyle &style,
                          std::string &error) {
  error.clear();
  if ((!classes && size != 0) || size > 4096) {
    error = "Tailwind class list is invalid or too long";
    return false;
  }
  style = {};
  const std::string_view input(classes ? classes : "", size);
  size_t offset = 0;
  while (offset < input.size()) {
    while (offset < input.size() && std::isspace(
                                      static_cast<unsigned char>(input[offset])))
      ++offset;
    const size_t start = offset;
    while (offset < input.size() && !std::isspace(
                                      static_cast<unsigned char>(input[offset])))
      ++offset;
    if (start == offset)
      continue;
    const std::string_view token = input.substr(start, offset - start);
    if (!parseOne(token, style)) {
      error = "unsupported Tailwind utility: " + std::string(token);
      return false;
    }
  }
  return true;
}

class NativeUiEngine::Impl {
public:
  struct Node {
    UiNodeRecord record;
    UiStyle style;
    std::vector<size_t> children;
  };

  explicit Impl(ApplicationScene &scene) : scene(scene) {}

  bool range(uint32_t offset, uint32_t length, size_t total) const {
    return offset <= total && length <= total - offset;
  }

  float intrinsicMain(const Node &node, bool row, const uint8_t *strings) {
    if (node.record.kind == static_cast<uint8_t>(UiNodeKind::Text)) {
      const char *text = reinterpret_cast<const char *>(strings) +
                         node.record.text_offset;
      return row ? scene.measureText(text, node.record.text_length,
                                     node.style.font_size)
                 : node.style.font_size * 1.25f;
    }
    const float explicit_size = row ? node.style.width : node.style.height;
    return explicit_size >= 0 ? explicit_size : 0;
  }

  void layoutNode(size_t index, float x, float y, float forced_width,
                  float forced_height, const uint8_t *strings,
                  uint32_t inherited_text, bool exact_size = false) {
    Node &node = nodes[index];
    UiStyle &style = node.style;
    if (!style.visible) {
      layout[index] = {};
      return;
    }
    float width = exact_size
                      ? forced_width
                      : resolvedLength(style.width, forced_width, forced_width);
    float height = exact_size
                       ? forced_height
                       : resolvedLength(style.height, forced_height,
                                        forced_height);
    width = std::max(style.min_width, width);
    height = std::max(style.min_height, height);
    if (style.max_width >= 0)
      width = std::min(width, style.max_width);
    if (style.max_height >= 0)
      height = std::min(height, style.max_height);
    width = std::max(0.0f, width);
    height = std::max(0.0f, height);
    layout[index] = {x, y, width, height};
    if (!style.has_text_color)
      style.text_rgba = inherited_text;
    if (node.children.empty())
      return;

    const bool row = style.direction == UiFlexDirection::Row;
    const float inner_x = x + style.padding[Left];
    const float inner_y = y + style.padding[Top];
    const float inner_width =
        std::max(0.0f, width - style.padding[Left] - style.padding[Right]);
    const float inner_height =
        std::max(0.0f, height - style.padding[Top] - style.padding[Bottom]);
    const float main_available = row ? inner_width : inner_height;
    const float cross_available = row ? inner_height : inner_width;
    float occupied = style.gap *
                     static_cast<float>(node.children.size() > 0
                                            ? node.children.size() - 1
                                            : 0);
    float total_grow = 0;
    float total_shrink_weight = 0;
    std::vector<float> basis(node.children.size(), 0);
    for (size_t child_index = 0; child_index < node.children.size();
         ++child_index) {
      Node &child = nodes[node.children[child_index]];
      const float explicit_main = row ? child.style.width : child.style.height;
      basis[child_index] =
          explicit_main == -2
              ? main_available
              : explicit_main >= 0
                    ? explicit_main
                    : intrinsicMain(child, row, strings);
      occupied += basis[child_index] +
                  (row ? child.style.margin[Left] + child.style.margin[Right]
                       : child.style.margin[Top] + child.style.margin[Bottom]);
      total_grow += child.style.grow;
      total_shrink_weight += child.style.shrink * basis[child_index];
    }
    const float remaining = std::max(0.0f, main_available - occupied);
    const float deficit = std::max(0.0f, occupied - main_available);
    float dynamic_gap = style.gap;
    float cursor = 0;
    if (total_grow == 0) {
      if (style.justify == UiJustify::Center)
        cursor = remaining * 0.5f;
      else if (style.justify == UiJustify::End)
        cursor = remaining;
      else if (style.justify == UiJustify::SpaceBetween &&
               node.children.size() > 1)
        dynamic_gap += remaining / (node.children.size() - 1);
    }
    for (size_t child_index = 0; child_index < node.children.size();
         ++child_index) {
      Node &child = nodes[node.children[child_index]];
      float main = basis[child_index];
      if (total_grow > 0)
        main += remaining * child.style.grow / total_grow;
      else if (deficit > 0 && total_shrink_weight > 0)
        main = std::max(
            0.0f,
            main - deficit * child.style.shrink * basis[child_index] /
                       total_shrink_weight);
      const float explicit_cross = row ? child.style.height : child.style.width;
      const float cross_margin_start =
          row ? child.style.margin[Top] : child.style.margin[Left];
      const float cross_margin_end =
          row ? child.style.margin[Bottom] : child.style.margin[Right];
      float cross = explicit_cross == -2
                        ? cross_available - cross_margin_start - cross_margin_end
                    : explicit_cross >= 0
                        ? explicit_cross
                    : style.align == UiAlign::Stretch
                        ? cross_available - cross_margin_start - cross_margin_end
                        : intrinsicMain(child, !row, strings);
      cross = std::max(0.0f, cross);
      float cross_position = cross_margin_start;
      const float cross_remaining = std::max(
          0.0f, cross_available - cross - cross_margin_start - cross_margin_end);
      if (style.align == UiAlign::Center)
        cross_position += cross_remaining * 0.5f;
      else if (style.align == UiAlign::End)
        cross_position += cross_remaining;
      const float main_margin_start =
          row ? child.style.margin[Left] : child.style.margin[Top];
      const float main_margin_end =
          row ? child.style.margin[Right] : child.style.margin[Bottom];
      cursor += main_margin_start;
      const float child_x = row ? inner_x + cursor : inner_x + cross_position;
      const float child_y = row ? inner_y + cross_position : inner_y + cursor;
      layoutNode(node.children[child_index], child_x, child_y,
                 row ? main : cross, row ? cross : main, strings,
                 style.text_rgba, true);
      cursor += main + main_margin_end + dynamic_gap;
    }
  }

  bool submit(const UiNodeRecord *records, size_t count, const uint8_t *strings,
              size_t string_bytes) {
    error.clear();
    if (!records || count == 0 || count > kMaximumUiNodes ||
        (!strings && string_bytes != 0) || string_bytes > kMaximumUiStrings) {
      error = "native UI batch is empty or exceeds its limits";
      return false;
    }
    static const uint8_t empty_strings[] = {0};
    if (!strings)
      strings = empty_strings;
    nodes.clear();
    layout.clear();
    nodes.reserve(count);
    layout.resize(count);
    std::unordered_map<uint32_t, size_t> ids;
    std::vector<size_t> roots;
    for (size_t index = 0; index < count; ++index) {
      const UiNodeRecord &record = records[index];
      if (record.id == 0 || record.kind > static_cast<uint8_t>(UiNodeKind::Canvas) ||
          !range(record.class_offset, record.class_length, string_bytes) ||
          !range(record.text_offset, record.text_length, string_bytes) ||
          !ids.emplace(record.id, index).second) {
        error = "native UI node id, kind, or string range is invalid";
        return false;
      }
      Node node;
      node.record = record;
      if (!parseTailwindClasses(
              reinterpret_cast<const char *>(strings) + record.class_offset,
              record.class_length, node.style, error))
        return false;
      if (record.kind == static_cast<uint8_t>(UiNodeKind::Canvas) &&
          record.canvas == 0) {
        error = "native UI canvas node requires a non-root canvas handle";
        return false;
      }
      if (record.kind != static_cast<uint8_t>(UiNodeKind::Text) &&
          record.text_length != 0) {
        error = "only native UI text nodes may contain text";
        return false;
      }
      nodes.push_back(std::move(node));
      if (record.parent == UINT32_MAX) {
        roots.push_back(index);
      } else {
        const auto parent = ids.find(record.parent);
        if (parent == ids.end()) {
          error = "native UI parents must precede their children";
          return false;
        }
        nodes[parent->second].children.push_back(index);
      }
    }
    if (roots.empty()) {
      error = "native UI batch has no root node";
      return false;
    }
    for (size_t root : roots)
      layoutNode(root, 0, 0, static_cast<float>(scene.width()),
                 static_cast<float>(scene.height()), strings, 0xff000000u);

    if (ui_canvas == 0) {
      ui_canvas = scene.createCanvas(
          {0, 0, scene.width(), scene.height(), 0, true},
          CanvasContextKind::Canvas2d);
      if (ui_canvas == 0) {
        error = scene.lastError();
        return false;
      }
    }
    std::vector<Canvas2dCommand> commands;
    commands.reserve(count * 2 + 1);
    Canvas2dCommand clear;
    clear.opcode = static_cast<uint8_t>(Canvas2dOpcode::Clear);
    clear.width = static_cast<float>(scene.width());
    clear.height = static_cast<float>(scene.height());
    commands.push_back(clear);
    std::unordered_map<uint32_t, CanvasGeometry> next_attached_canvases;
    for (size_t index = 0; index < nodes.size(); ++index) {
      const Node &node = nodes[index];
      const UiLayoutRect &rect = layout[index];
      if (!node.style.visible || rect.width <= 0 || rect.height <= 0)
        continue;
      if (node.style.has_background) {
        Canvas2dCommand background;
        background.opcode = static_cast<uint8_t>(Canvas2dOpcode::FillRect);
        background.x = rect.x;
        background.y = rect.y;
        background.width = rect.width;
        background.height = rect.height;
        background.radius = node.style.radius;
        background.rgba = node.style.background_rgba;
        commands.push_back(background);
      }
      if (node.record.kind == static_cast<uint8_t>(UiNodeKind::Text)) {
        Canvas2dCommand text;
        text.opcode = static_cast<uint8_t>(Canvas2dOpcode::FillText);
        text.x = rect.x + node.style.padding[Left];
        text.y = rect.y + node.style.padding[Top] + node.style.font_size;
        text.width = std::max(0.0f, rect.width - node.style.padding[Left] -
                                       node.style.padding[Right]);
        text.font_size = node.style.font_size;
        text.rgba = node.style.text_rgba;
        text.text_offset = node.record.text_offset;
        text.text_length = node.record.text_length;
        commands.push_back(text);
      } else if (node.record.kind ==
                 static_cast<uint8_t>(UiNodeKind::Canvas)) {
        CanvasGeometry geometry;
        geometry.x = std::max(0, static_cast<int32_t>(std::floor(rect.x)));
        geometry.y = std::max(0, static_cast<int32_t>(std::floor(rect.y)));
        geometry.width = static_cast<uint32_t>(std::max(1.0f, std::floor(rect.width)));
        geometry.height = static_cast<uint32_t>(std::max(1.0f, std::floor(rect.height)));
        geometry.z_order = static_cast<int32_t>(index + 1);
        geometry.visible = true;
        if (!next_attached_canvases.emplace(node.record.canvas, geometry).second) {
          error = "a canvas may appear only once in a native UI tree";
          return false;
        }
        if (!scene.placeCanvas(node.record.canvas, geometry)) {
          error = scene.lastError();
          return false;
        }
      }
    }
    for (const auto &attached : attached_canvases) {
      if (next_attached_canvases.find(attached.first) !=
          next_attached_canvases.end())
        continue;
      CanvasGeometry hidden = attached.second;
      hidden.visible = false;
      if (!scene.placeCanvas(attached.first, hidden)) {
        error = scene.lastError();
        return false;
      }
    }
    if (!scene.submitCanvas2d(ui_canvas, commands.data(), commands.size(),
                              strings, string_bytes)) {
      error = scene.lastError();
      return false;
    }
    attached_canvases = std::move(next_attached_canvases);
    return true;
  }

  void clear() {
    for (const auto &attached : attached_canvases) {
      CanvasGeometry hidden = attached.second;
      hidden.visible = false;
      scene.placeCanvas(attached.first, hidden);
    }
    attached_canvases.clear();
    if (ui_canvas != 0) {
      scene.destroyCanvas(ui_canvas);
      ui_canvas = 0;
    }
    nodes.clear();
    layout.clear();
    error.clear();
  }

  ApplicationScene &scene;
  std::vector<Node> nodes;
  std::vector<UiLayoutRect> layout;
  std::unordered_map<uint32_t, CanvasGeometry> attached_canvases;
  uint32_t ui_canvas = 0;
  std::string error;
};

NativeUiEngine::NativeUiEngine(ApplicationScene &scene)
    : impl_(std::make_unique<Impl>(scene)) {}
NativeUiEngine::~NativeUiEngine() { clear(); }
bool NativeUiEngine::submit(const UiNodeRecord *nodes, size_t node_count,
                            const uint8_t *strings, size_t string_bytes) {
  return impl_->submit(nodes, node_count, strings, string_bytes);
}
void NativeUiEngine::clear() { impl_->clear(); }
const std::vector<UiLayoutRect> &NativeUiEngine::layout() const {
  return impl_->layout;
}
const std::string &NativeUiEngine::lastError() const { return impl_->error; }

} // namespace oos::runtime
