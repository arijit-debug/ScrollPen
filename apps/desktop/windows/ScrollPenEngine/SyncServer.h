#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sp {

// Minimal RFC 6455 WebSocket server for the browser-sync extension.
//
// Security: binds 127.0.0.1 only; requires a chrome-extension:// Origin (optionally a
// pinned extension id); requires a loopback Host header (blocks DNS rebinding); caps
// frame size and client count; text frames only; never sends page data anywhere.
class SyncServer {
 public:
  struct Options {
    unsigned short port{47615};
    std::vector<std::string> allowedExtensionIds; // empty = any chrome-extension:// origin
    size_t maxMessageBytes{64 * 1024};
    size_t maxClients{8};
  };

  bool Start(Options options, std::function<void(const std::string &)> onMessage);
  void Stop();
  ~SyncServer() { Stop(); }

  bool Running() const { return running_; }
  unsigned short Port() const { return options_.port; }

  // Exposed for unit tests.
  static std::string AcceptKey(const std::string &clientKey);
  static bool OriginAllowed(const std::string &origin, const std::vector<std::string> &allowedIds);
  static bool HostAllowed(const std::string &host, unsigned short port);

 private:
  void Run();

  Options options_;
  std::function<void(const std::string &)> onMessage_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  uintptr_t listen_{~uintptr_t{0}};
};

} // namespace sp
