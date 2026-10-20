#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

namespace sp {

struct Hotkey {
  unsigned modifiers{}; // MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN
  unsigned vk{};
};

// "Ctrl+Shift+D" -> {MOD_CONTROL|MOD_SHIFT, 'D'}. Empty or invalid -> nullopt.
std::optional<Hotkey> ParseHotkey(const std::string &text);
std::string FormatHotkey(const Hotkey &hk);
// "CapsLock", "`", "F8"... -> virtual-key code, 0 if unknown.
unsigned ParseKeyName(const std::string &name);

struct Settings {
  // Tools
  std::string tool{"pen"};
  uint32_t color{0xFFE53935};
  float penWidth{4};
  float highlighterWidth{22};
  float shapeWidth{4};
  float fontSize{28};
  bool fill{false};
  double laserFadeSeconds{1.2};
  std::vector<uint32_t> palette{0xFFE53935, 0xFFFB8C00, 0xFFFDD835, 0xFF43A047, 0xFF1E88E5,
                                0xFF8E24AA, 0xFF212121, 0xFFFFFFFF, 0xFF00ACC1};

  // Behaviour
  bool documentMode{true};        // ink follows scrolling content when possible
  std::string clickThroughKey{"CapsLock"}; // hold to click/type into apps while drawing
  bool hideDockFromCapture{true}; // audience sees the ink, not the toolbar
  bool startActive{false};

  // Global hotkeys (work everywhere, even when not drawing).
  std::map<std::string, std::string> hotkeys{
      {"toggleDraw", "Ctrl+Shift+D"},
      {"clear", "Ctrl+Shift+X"},
      {"toggleInk", "Ctrl+Shift+H"},
      {"screenshot", "Ctrl+Shift+S"},
      {"toggleMode", "Ctrl+Shift+M"},
  };

  // Browser sync
  unsigned short syncPort{47615};
  std::vector<std::string> allowedExtensionIds; // empty = any extension (dev builds)

  nlohmann::json ToJson() const;
  // Applies known fields from j, validating ranges; unknown/invalid fields are ignored.
  void Merge(const nlohmann::json &j);
};

std::string ColorToHex(uint32_t argb);
std::optional<uint32_t> ParseColor(const std::string &hex);

} // namespace sp
