#include "pch.h"
#include "PipeServer.h"

#include "LocalPeer.h"

namespace sp {

namespace {

constexpr DWORD kWriteTimeoutMs = 250;

struct SecurityAttributes {
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  SecurityAttributes() {
    ConvertStringSecurityDescriptorToSecurityDescriptorW(peer::SyncPipeSddl().c_str(), SDDL_REVISION_1,
                                                         &sa.lpSecurityDescriptor, nullptr);
  }
  ~SecurityAttributes() { LocalFree(sa.lpSecurityDescriptor); }
  bool Valid() const { return sa.lpSecurityDescriptor != nullptr; }
};

HANDLE CreateInstance(const std::wstring &name, size_t bufferBytes, bool first) {
  SecurityAttributes sec;
  if (!sec.Valid() || peer::CurrentUserSid().empty()) return INVALID_HANDLE_VALUE;
  const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
  const DWORD mode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
  return CreateNamedPipeW(name.c_str(), open, mode, PIPE_UNLIMITED_INSTANCES, static_cast<DWORD>(bufferBytes),
                          static_cast<DWORD>(bufferBytes), 0, &sec.sa);
}

// Waits for an overlapped operation, giving up (and cancelling it) on stop or timeout.
bool Complete(HANDLE h, OVERLAPPED &ov, HANDLE stop, DWORD timeoutMs, DWORD &bytes) {
  const HANDLE waits[] = {ov.hEvent, stop};
  const DWORD r = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeoutMs);
  if (r != WAIT_OBJECT_0) {
    CancelIoEx(h, &ov);
    GetOverlappedResult(h, &ov, &bytes, TRUE); // the OVERLAPPED must outlive the I/O
    return false;
  }
  return GetOverlappedResult(h, &ov, &bytes, FALSE) != 0;
}

} // namespace

struct PipeServer::Client {
  HANDLE pipe{INVALID_HANDLE_VALUE};
  HANDLE writeEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  std::mutex writeMutex;
  std::atomic<bool> dead{false};
  std::thread reader;
  ~Client() {
    if (reader.joinable()) reader.join();
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    CloseHandle(writeEvent);
  }
};

PipeServer::PipeServer() = default;
PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(Options options, std::function<void(const std::string &)> onMessage) {
  if (running_) return true;
  options_ = options;
  onMessage_ = std::move(onMessage);
  name_ = peer::SyncPipeName();
  // Exclusive first instance: if the name is already taken, someone else owns it.
  const HANDLE first = CreateInstance(name_, options_.maxMessageBytes, true);
  if (first == INVALID_HANDLE_VALUE) return false;
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  running_ = true;
  thread_ = std::thread([this, first] { AcceptLoop(first); });
  return true;
}

void PipeServer::Stop() {
  if (!thread_.joinable()) return;
  SetEvent(stop_);
  thread_.join();
  {
    std::lock_guard lock(clientsMutex_);
    clients_.clear(); // readers wake on stop_ and are joined here
  }
  CloseHandle(stop_);
  stop_ = nullptr;
  running_ = false;
}

void PipeServer::ReapDead() {
  std::lock_guard lock(clientsMutex_);
  std::erase_if(clients_, [](const std::unique_ptr<Client> &c) { return c->dead.load(); });
}

void PipeServer::AcceptLoop(HANDLE pipe) {
  const HANDLE connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  while (pipe != INVALID_HANDLE_VALUE) {
    OVERLAPPED ov{};
    ov.hEvent = connected;
    ResetEvent(connected);
    bool ok = ConnectNamedPipe(pipe, &ov) != 0;
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
      DWORD unused = 0;
      ok = Complete(pipe, ov, stop_, INFINITE, unused);
    } else if (err == ERROR_PIPE_CONNECTED) {
      ok = true;
    }
    if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {
      CloseHandle(pipe);
      break;
    }

    ReapDead();
    ULONG pid = 0;
    size_t count = 0;
    {
      std::lock_guard lock(clientsMutex_);
      count = clients_.size();
    }
    const bool accepted = ok && count < options_.maxClients && GetNamedPipeClientProcessId(pipe, &pid) &&
        peer::IsOwnProcess(pid, L"ScrollPenHost.exe", true);
    if (accepted) {
      auto client = std::make_unique<Client>();
      client->pipe = pipe;
      Client *raw = client.get();
      std::lock_guard lock(clientsMutex_);
      clients_.push_back(std::move(client));
      raw->reader = std::thread([this, raw] { ReadLoop(raw); });
    } else {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
    pipe = CreateInstance(name_, options_.maxMessageBytes, false);
  }
  CloseHandle(connected);
}

void PipeServer::ReadLoop(Client *client) {
  const HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<char> buf(options_.maxMessageBytes);
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = readEvent;
    ResetEvent(readEvent);
    DWORD n = 0;
    bool ok = ReadFile(client->pipe, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov) != 0;
    if (!ok && GetLastError() != ERROR_IO_PENDING) break;
    // A message longer than the buffer fails with ERROR_MORE_DATA: too large, drop the client.
    if (!Complete(client->pipe, ov, stop_, INFINITE, n)) break;
    if (n > 0 && onMessage_) onMessage_(std::string(buf.data(), n));
  }
  CloseHandle(readEvent);
  client->dead = true;
}

void PipeServer::Broadcast(const std::string &text) {
  if (text.size() > options_.maxMessageBytes) return;
  std::lock_guard lock(clientsMutex_);
  for (auto &c : clients_) {
    if (c->dead) continue;
    std::lock_guard write(c->writeMutex);
    OVERLAPPED ov{};
    ov.hEvent = c->writeEvent;
    ResetEvent(c->writeEvent);
    DWORD n = 0;
    const bool started = WriteFile(c->pipe, text.data(), static_cast<DWORD>(text.size()), nullptr, &ov) != 0 ||
        GetLastError() == ERROR_IO_PENDING;
    // A helper that stops reading must not stall the engine: give up on it.
    if (!started || !Complete(c->pipe, ov, nullptr, kWriteTimeoutMs, n)) {
      c->dead = true;
      CancelIoEx(c->pipe, nullptr); // wakes its reader, which then exits
    }
  }
}

} // namespace sp
