// ScrollPenHost.exe: Chrome native-messaging host for the ScrollPen Browser Sync extension.
//
// Chrome starts it (registered under HKCU for the current Windows user, allowed only for the
// ScrollPen extension) and talks to it over stdin/stdout: each message is a 32-bit length in
// native byte order followed by that many bytes of JSON. The helper relays messages to and
// from the ScrollPen app over the per-user pipe, after checking that the pipe's server is
// ScrollPen.exe running as this user in this session. It tells the extension whether the app
// is reachable with {"type":"host","v":1,"connected":...}.

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../ScrollPenEngine/LocalPeer.h"

namespace {

constexpr uint32_t kMaxMessage = 64 * 1024;

HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
std::mutex g_outMutex;
std::mutex g_pipeMutex;
HANDLE g_pipe = INVALID_HANDLE_VALUE; // guarded by g_pipeMutex

bool ReadExact(HANDLE h, void *data, DWORD n) {
  auto *p = static_cast<char *>(data);
  while (n > 0) {
    DWORD got = 0;
    if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool WriteExact(HANDLE h, const void *data, DWORD n) {
  auto *p = static_cast<const char *>(data);
  while (n > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, n, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

void ToChrome(const std::string &message) {
  if (message.size() > kMaxMessage) return;
  std::lock_guard lock(g_outMutex);
  const uint32_t len = static_cast<uint32_t>(message.size());
  if (!WriteExact(g_out, &len, sizeof(len)) || !WriteExact(g_out, message.data(), len)) ExitProcess(0);
}

void Status(bool connected) {
  ToChrome(connected ? R"({"type":"host","v":1,"connected":true})" : R"({"type":"host","v":1,"connected":false})");
}

// Overlapped I/O on the pipe, so reading and writing never block each other.
bool PipeIo(HANDLE pipe, bool write, void *data, DWORD size, DWORD &bytes, DWORD timeoutMs) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool ok = write ? WriteFile(pipe, data, size, nullptr, &ov) != 0 : ReadFile(pipe, data, size, nullptr, &ov) != 0;
  if (ok || GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
    ok = GetOverlappedResult(pipe, &ov, &bytes, TRUE) != 0;
  }
  CloseHandle(ov.hEvent);
  return ok;
}

// Chrome -> app. Messages arriving while the app isn't reachable are dropped; the extension
// keeps its own outbox and resends its state when told the app is back.
void StdinLoop() {
  std::string message;
  for (;;) {
    uint32_t len = 0;
    if (!ReadExact(g_in, &len, sizeof(len))) ExitProcess(0); // Chrome closed the port
    if (len > kMaxMessage) ExitProcess(1);
    message.resize(len);
    if (len && !ReadExact(g_in, message.data(), len)) ExitProcess(0);
    std::lock_guard lock(g_pipeMutex);
    if (g_pipe == INVALID_HANDLE_VALUE) continue;
    DWORD put = 0;
    PipeIo(g_pipe, true, message.data(), len, put, 1000);
  }
}

HANDLE ConnectToApp(const std::wstring &name) {
  // SECURITY_IDENTIFICATION: the app can tell who we are but can't act as us.
  const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return pipe;
  ULONG pid = 0;
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!GetNamedPipeServerProcessId(pipe, &pid) || !sp::peer::IsOwnProcess(pid, L"ScrollPen.exe", true) ||
      !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    CloseHandle(pipe);
    return INVALID_HANDLE_VALUE;
  }
  return pipe;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  // Chrome passes the calling extension's origin first; Chrome itself only lets the
  // extensions listed in our manifest start us, this is a second check.
  const std::wstring expected = L"chrome-extension://" + std::wstring(sp::peer::kExtensionId,
                                                                      sp::peer::kExtensionId + 32) + L"/";
  if (argc < 2 || expected != argv[1]) return 1;

  g_in = GetStdHandle(STD_INPUT_HANDLE);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  std::thread(StdinLoop).detach();

  const std::wstring name = sp::peer::SyncPipeName();
  std::vector<char> buf(kMaxMessage);
  bool reported = false;
  for (;;) {
    const HANDLE pipe = ConnectToApp(name);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (!reported) Status(false); // the app isn't running (yet)
      reported = true;
      Sleep(1000);
      continue;
    }
    {
      std::lock_guard lock(g_pipeMutex);
      g_pipe = pipe;
    }
    Status(true);
    for (;;) {
      DWORD got = 0;
      if (!PipeIo(pipe, false, buf.data(), kMaxMessage, got, INFINITE)) break; // app quit, or oversized
      if (got) ToChrome(std::string(buf.data(), got));
    }
    {
      std::lock_guard lock(g_pipeMutex);
      CloseHandle(g_pipe);
      g_pipe = INVALID_HANDLE_VALUE;
    }
    Status(false);
    reported = true;
    Sleep(500);
  }
}
