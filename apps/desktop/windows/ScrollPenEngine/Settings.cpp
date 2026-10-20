#include "pch.h"
#include "Settings.h"

namespace sp {

namespace {

std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

const std::map<std::string, unsigned> &KeyNames() {
  static const std::map<std::string, unsigned> names = {
      {"CAPSLOCK", VK_CAPITAL}, {"SPACE", VK_SPACE},     {"TAB", VK_TAB},          {"ESC", VK_ESCAPE},
      {"ESCAPE", VK_ESCAPE},    {"ENTER", VK_RETURN},    {"`", VK_OEM_3},          {"BACKTICK", VK_OEM_3},
      {"DELETE", VK_DELETE},    {"INSERT", VK_INSERT},   {"HOME", VK_HOME},        {"END", VK_END},
      {"PAGEUP", VK_PRIOR},     {"PAGEDOWN", VK_NEXT},   {"UP", VK_UP},            {"DOWN", VK_DOWN},
      {"LEFT", VK_LEFT},        {"RIGHT", VK_RIGHT},     {"SCROLLLOCK", VK_SCROLL}, {"PAUSE", VK_PAUSE},
      {"RCTRL", VK_RCONTROL},   {"RALT", VK_RMENU},      {"RSHIFT", VK_RSHIFT},    {"APPS", VK_APPS},
      {"MENU", VK_APPS},
  };
  return names;
}

} // namespace

unsigned ParseKeyName(const std::string &name) {
  const std::string k = Upper(name);
  if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return static_cast<unsigned>(k[0]);
  if (k.size() >= 2 && k[0] == 'F') {
    const int n = std::atoi(k.c_str() + 1);
    if (n >= 1 && n <= 24 && std::to_string(n) == k.substr(1)) return VK_F1 + static_cast<unsigned>(n - 1);
  }
  auto it = KeyNames().find(k);
  return it == KeyNames().end() ? 0 : it->second;
}

std::optional<Hotkey> ParseHotkey(const std::string &text) {
  Hotkey hk;
  size_t start = 0;
  std::vector<std::string> parts;
  while (start <= text.size()) {
    const auto plus = text.find('+', start);
    parts.push_back(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  if (parts.size() < 2) return std::nullopt; // a global hotkey needs at least one modifier
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    const std::string m = Upper(parts[i]);
    if (m == "CTRL" || m == "CONTROL") hk.modifiers |= MOD_CONTROL;
    else if (m == "SHIFT") hk.modifiers |= MOD_SHIFT;
    else if (m == "ALT") hk.modifiers |= MOD_ALT;
    else if (m == "WIN") hk.modifiers |= MOD_WIN;
    else return std::nullopt;
  }
  hk.vk = ParseKeyName(parts.back());
  if (!hk.vk || !hk.modifiers) return std::nullopt;
  return hk;
}

std::string FormatHotkey(const Hotkey &hk) {
  std::string s;
  if (hk.modifiers & MOD_CONTROL) s += "Ctrl+";
  if (hk.modifiers & MOD_SHIFT) s += "Shift+";
  if (hk.modifiers & MOD_ALT) s += "Alt+";
  if (hk.modifiers & MOD_WIN) s += "Win+";
  for (const auto &[name, vk] : KeyNames())
    if (vk == hk.vk) return s + name;
  if (hk.vk >= VK_F1 && hk.vk <= VK_F24) return s + "F" + std::to_string(hk.vk - VK_F1 + 1);
  return s + static_cast<char>(hk.vk);
}

std::string ColorToHex(uint32_t argb) {
  char buf[10];
  snprintf(buf, sizeof(buf), "#%06X", argb & 0xFFFFFF);
  return buf;
}

std::optional<uint32_t> ParseColor(const std::string &hex) {
  if (hex.size() != 7 || hex[0] != '#') return std::nullopt;
  uint32_t v = 0;
  for (size_t i = 1; i < 7; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i])));
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
    else return std::nullopt;
  }
  return 0xFF000000u | v;
}

nlohmann::json Settings::ToJson() const {
  nlohmann::json pal = nlohmann::json::array();
  for (auto c : palette) pal.push_back(ColorToHex(c));
  return {
      {"tool", tool},
      {"color", ColorToHex(color)},
      {"penWidth", penWidth},
      {"highlighterWidth", highlighterWidth},
      {"shapeWidth", shapeWidth},
      {"fontSize", fontSize},
      {"fill", fill},
      {"laserFadeSeconds", laserFadeSeconds},
      {"palette", pal},
      {"documentMode", documentMode},
      {"clickThroughKey", clickThroughKey},
      {"hideDockFromCapture", hideDockFromCapture},
      {"startActive", startActive},
      {"hotkeys", hotkeys},
      {"syncPort", syncPort},
      {"allowedExtensionIds", allowedExtensionIds},
  };
}

void Settings::Merge(const nlohmann::json &j) {
  if (!j.is_object()) return;
  auto num = [&](const char *key, float lo, float hi, float &out) {
    if (j.contains(key) && j[key].is_number()) out = std::clamp(j[key].get<float>(), lo, hi);
  };
  auto flag = [&](const char *key, bool &out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
  };
  static const std::vector<std::string> tools = {"pen", "highlighter", "laser", "eraser", "line",
                                                 "arrow", "rect", "ellipse", "text"};
  if (j.contains("tool") && j["tool"].is_string()) {
    const auto t = j["tool"].get<std::string>();
    if (std::find(tools.begin(), tools.end(), t) != tools.end()) tool = t;
  }
  if (j.contains("color") && j["color"].is_string())
    if (auto c = ParseColor(j["color"].get<std::string>())) color = *c;
  num("penWidth", 1, 64, penWidth);
  num("highlighterWidth", 4, 96, highlighterWidth);
  num("shapeWidth", 1, 64, shapeWidth);
  num("fontSize", 8, 200, fontSize);
  flag("fill", fill);
  if (j.contains("laserFadeSeconds") && j["laserFadeSeconds"].is_number())
    laserFadeSeconds = std::clamp(j["laserFadeSeconds"].get<double>(), 0.2, 10.0);
  if (j.contains("palette") && j["palette"].is_array()) {
    std::vector<uint32_t> p;
    for (const auto &c : j["palette"])
      if (c.is_string())
        if (auto v = ParseColor(c.get<std::string>())) p.push_back(*v);
    if (!p.empty() && p.size() <= 16) palette = p;
  }
  flag("documentMode", documentMode);
  if (j.contains("clickThroughKey") && j["clickThroughKey"].is_string()) {
    const auto k = j["clickThroughKey"].get<std::string>();
    if (k.empty() || ParseKeyName(k)) clickThroughKey = k;
  }
  flag("hideDockFromCapture", hideDockFromCapture);
  flag("startActive", startActive);
  if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
    for (auto &[name, value] : j["hotkeys"].items()) {
      if (!hotkeys.count(name) || !value.is_string()) continue;
      const auto v = value.get<std::string>();
      if (v.empty() || ParseHotkey(v)) hotkeys[name] = v;
    }
  }
  if (j.contains("syncPort") && j["syncPort"].is_number_integer()) {
    const int p = j["syncPort"].get<int>();
    if (p >= 1024 && p <= 65535) syncPort = static_cast<unsigned short>(p);
  }
  if (j.contains("allowedExtensionIds") && j["allowedExtensionIds"].is_array()) {
    std::vector<std::string> ids;
    for (const auto &id : j["allowedExtensionIds"]) {
      if (!id.is_string()) continue;
      const auto s = id.get<std::string>();
      if (s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'p'; })) ids.push_back(s);
    }
    allowedExtensionIds = ids;
  }
}

} // namespace sp
