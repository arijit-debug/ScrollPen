#pragma once

// Identity checks for ScrollPen's local links: the native-messaging pipe between the app and
// ScrollPenHost.exe, and the dev WebSocket. Header-only, so the helper can use it too.

#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace sp::peer {

// Chrome's ID for the ScrollPen Browser Sync extension (fixed by the "key" in its manifest).
inline constexpr char kExtensionId[] = "egihgjpnabmnoljofdkeakgplgjeidai";

// String SID ("S-1-5-21-...") of the user a process runs as; empty if it can't be read.
inline std::wstring ProcessUserSid(HANDLE process) {
  HANDLE token{};
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
  std::wstring out;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buf(size);
  if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
    LPWSTR s = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &s)) {
      out = s;
      LocalFree(s);
    }
  }
  CloseHandle(token);
  return out;
}

inline const std::wstring &CurrentUserSid() {
  static const std::wstring sid = ProcessUserSid(GetCurrentProcess());
  return sid;
}

// One pipe per Windows user, so two people signed in to the same PC never share a link.
inline std::wstring SyncPipeName() { return L"\\\\.\\pipe\\ScrollPen.Sync." + CurrentUserSid(); }

// Only the current user may open the pipe (protected DACL, no inherited entries).
inline std::wstring SyncPipeSddl() { return L"D:P(A;;GA;;;" + CurrentUserSid() + L")"; }

// True when process pid runs in our logon session, its image file is exeName (any folder,
// case-insensitive) and, with sameUser, it runs as our user. Chrome's sandboxed network
// process doesn't let us read its token, so the dev WebSocket checks the session only.
inline bool IsOwnProcess(DWORD pid, const wchar_t *exeName, bool sameUser) {
  DWORD theirs = 0, ours = 0;
  if (!ProcessIdToSessionId(pid, &theirs) || !ProcessIdToSessionId(GetCurrentProcessId(), &ours) || theirs != ours)
    return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  wchar_t path[1024];
  DWORD n = ARRAYSIZE(path);
  const bool named = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
  const std::wstring sid = sameUser ? ProcessUserSid(h) : std::wstring{};
  CloseHandle(h);
  if (!named || (sameUser && (sid.empty() || sid != CurrentUserSid()))) return false;
  const wchar_t *base = wcsrchr(path, L'\\');
  return _wcsicmp(base ? base + 1 : path, exeName) == 0;
}

} // namespace sp::peer
