#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sp {

// The app's end of the browser link. Chrome starts ScrollPenHost.exe for the extension
// (native messaging); the helper connects here and relays the extension's messages.
//
// Security: the pipe is named after the user's SID and only that user may open it; remote
// clients are rejected; the first instance is created exclusively, so a pipe squatted by
// someone else is detected instead of shared; every client must be ScrollPenHost.exe in
// our session running as our user; one message per pipe message, size-capped.
class PipeServer {
 public:
  struct Options {
    size_t maxMessageBytes{64 * 1024};
    size_t maxClients{4}; // one helper per Chrome profile
  };

  bool Start(Options options, std::function<void(const std::string &)> onMessage);
  void Stop();
  // Sends a message to every connected helper. Any thread.
  void Broadcast(const std::string &text);
  PipeServer();
  ~PipeServer();

  bool Running() const { return running_; }

 private:
  struct Client;
  void AcceptLoop(HANDLE first);
  void ReadLoop(Client *client);
  void ReapDead();

  Options options_;
  std::function<void(const std::string &)> onMessage_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  HANDLE stop_{};
  std::wstring name_;
  std::mutex clientsMutex_;
  std::vector<std::unique_ptr<Client>> clients_;
};

} // namespace sp
