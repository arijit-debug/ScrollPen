#pragma once

#include <functional>
#include <memory>
#include <string>

namespace sp {

// ScrollPen's native core. Runs on its own thread with its own message loop so ink
// latency never depends on React Native. Talk to it with JSON commands; it answers
// with JSON events (see docs/engine-protocol.md).
class Engine {
 public:
  using EventSink = std::function<void(const std::string &json)>;

  static Engine &Instance();

  // dock: the React Native window, kept above the overlays and optionally hidden from capture.
  void Start(HWND dock, EventSink sink);
  void Stop();

  // Thread-safe; the command runs on the engine thread.
  void Post(const std::string &commandJson);

 private:
  Engine() = default;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sp
