#include "pch.h"
#include "SyncServer.h"

#include "LocalPeer.h"

#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace sp {

namespace {

std::string Base64(const unsigned char *data, size_t n) {
  static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t b = (data[i] << 16) | ((i + 1 < n ? data[i + 1] : 0) << 8) | (i + 2 < n ? data[i + 2] : 0);
    out += tbl[(b >> 18) & 63];
    out += tbl[(b >> 12) & 63];
    out += i + 1 < n ? tbl[(b >> 6) & 63] : '=';
    out += i + 2 < n ? tbl[b & 63] : '=';
  }
  return out;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string Trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

constexpr ULONGLONG kHandshakeTimeoutMs = 5000;

struct Client {
  SOCKET s{INVALID_SOCKET};
  bool upgraded{false};
  std::string in;
  ULONGLONG acceptedAt{};
};

// The TCP peer must be chrome.exe in our own logon session: the Origin header is only
// trustworthy from a browser, and loopback ports are shared by every user on the PC.
bool PeerIsOwnChrome(SOCKET s) {
  sockaddr_in peer{};
  int len = sizeof(peer);
  if (getpeername(s, reinterpret_cast<sockaddr *>(&peer), &len) != 0 || peer.sin_family != AF_INET) return false;
  ULONG size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
  std::vector<uint8_t> buf(size);
  auto *table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID *>(buf.data());
  if (!size || GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR)
    return false;
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto &row = table->table[i];
    if (static_cast<USHORT>(row.dwLocalPort) == peer.sin_port && row.dwLocalAddr == peer.sin_addr.s_addr)
      return peer::IsOwnProcess(row.dwOwningPid, L"chrome.exe", false);
  }
  return false;
}

bool SendAll(SOCKET s, const char *data, size_t n) {
  while (n > 0) {
    const int sent = send(s, data, static_cast<int>(n), 0);
    if (sent == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv{0, 100000};
        if (select(0, nullptr, &w, nullptr, &tv) <= 0) return false;
        continue;
      }
      return false;
    }
    data += sent;
    n -= static_cast<size_t>(sent);
  }
  return true;
}

void SendFrame(SOCKET s, uint8_t opcode, const std::string &payload) {
  std::string f;
  f += static_cast<char>(0x80 | opcode);
  if (payload.size() < 126) {
    f += static_cast<char>(payload.size());
  } else {
    f += static_cast<char>(126);
    f += static_cast<char>((payload.size() >> 8) & 0xFF);
    f += static_cast<char>(payload.size() & 0xFF);
  }
  f += payload;
  SendAll(s, f.data(), f.size());
}

} // namespace

std::string SyncServer::AcceptKey(const std::string &clientKey) {
  const std::string input = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char hash[20]{};
  BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(input.data())),
             static_cast<ULONG>(input.size()), hash, sizeof(hash));
  return Base64(hash, sizeof(hash));
}

bool SyncServer::OriginAllowed(const std::string &origin, const std::vector<std::string> &allowedIds) {
  static const std::string prefix = "chrome-extension://";
  if (origin.rfind(prefix, 0) != 0) return false;
  std::string id = origin.substr(prefix.size());
  if (!id.empty() && id.back() == '/') id.pop_back();
  if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; }))
    return false;
  return std::find(allowedIds.begin(), allowedIds.end(), id) != allowedIds.end();
}

bool SyncServer::HostAllowed(const std::string &host, unsigned short port) {
  const std::string p = ":" + std::to_string(port);
  const std::string h = Lower(host);
  return h == "127.0.0.1" + p || h == "localhost" + p || h == "[::1]" + p;
}

bool SyncServer::Start(Options options, std::function<void(const std::string &)> onMessage) {
  if (running_) return true;
  options_ = std::move(options);
  onMessage_ = std::move(onMessage);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) {
    WSACleanup();
    return false;
  }
  // Refuse to share the port with another process (prevents port hijacking).
  BOOL exclusive = TRUE;
  setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(ls, 4) == SOCKET_ERROR) {
    closesocket(ls);
    WSACleanup();
    return false;
  }
  u_long nb = 1;
  ioctlsocket(ls, FIONBIO, &nb);
  listen_ = ls;
  stop_ = false;
  running_ = true;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void SyncServer::Stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  thread_.join();
  closesocket(static_cast<SOCKET>(listen_));
  listen_ = ~uintptr_t{0};
  running_ = false;
  WSACleanup();
}

void SyncServer::Run() {
  const SOCKET ls = static_cast<SOCKET>(listen_);
  std::vector<Client> clients;

  auto drop = [&](size_t i) {
    {
      std::lock_guard lock(sendMutex_);
      upgraded_.erase(std::remove(upgraded_.begin(), upgraded_.end(), static_cast<uintptr_t>(clients[i].s)),
                      upgraded_.end());
    }
    closesocket(clients[i].s);
    clients.erase(clients.begin() + static_cast<ptrdiff_t>(i));
  };
  auto reply = [&](SOCKET s, uint8_t opcode, const std::string &payload) {
    std::lock_guard lock(sendMutex_);
    SendFrame(s, opcode, payload);
  };

  while (!stop_) {
    // A client that never finishes its handshake must not hold a slot forever.
    const ULONGLONG now = GetTickCount64();
    for (size_t i = clients.size(); i-- > 0;)
      if (!clients[i].upgraded && now - clients[i].acceptedAt > kHandshakeTimeoutMs) drop(i);

    fd_set r;
    FD_ZERO(&r);
    FD_SET(ls, &r);
    for (auto &c : clients) FD_SET(c.s, &r);
    timeval tv{0, 200000};
    if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;

    if (FD_ISSET(ls, &r)) {
      SOCKET s = accept(ls, nullptr, nullptr);
      if (s != INVALID_SOCKET) {
        if (clients.size() >= options_.maxClients) {
          closesocket(s);
        } else {
          u_long nb = 1;
          ioctlsocket(s, FIONBIO, &nb);
          clients.push_back({s, false, {}, GetTickCount64()});
        }
      }
    }

    for (size_t i = clients.size(); i-- > 0;) {
      auto &c = clients[i];
      if (!FD_ISSET(c.s, &r)) continue;
      char buf[8192];
      const int n = recv(c.s, buf, sizeof(buf), 0);
      if (n <= 0) {
        drop(i);
        continue;
      }
      c.in.append(buf, static_cast<size_t>(n));
      if (c.in.size() > options_.maxMessageBytes + 16) {
        drop(i);
        continue;
      }

      if (!c.upgraded) {
        const auto end = c.in.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        std::string origin, host, key, upgrade;
        size_t pos = c.in.find("\r\n") + 2;
        while (pos < end) {
          const auto eol = c.in.find("\r\n", pos);
          const std::string line = c.in.substr(pos, eol - pos);
          const auto colon = line.find(':');
          if (colon != std::string::npos) {
            const std::string name = Lower(Trim(line.substr(0, colon)));
            const std::string value = Trim(line.substr(colon + 1));
            if (name == "origin") origin = value;
            else if (name == "host") host = value;
            else if (name == "sec-websocket-key") key = value;
            else if (name == "upgrade") upgrade = Lower(value);
          }
          pos = eol + 2;
        }
        const bool ok = c.in.rfind("GET ", 0) == 0 && upgrade == "websocket" && !key.empty() &&
            HostAllowed(host, options_.port) && OriginAllowed(origin, options_.allowedExtensionIds) && PeerIsOwnChrome(c.s);
        if (!ok) {
          static const char forbidden[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
          SendAll(c.s, forbidden, sizeof(forbidden) - 1);
          drop(i);
          continue;
        }
        const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " + AcceptKey(key) + "\r\n\r\n";
        {
          std::lock_guard lock(sendMutex_);
          SendAll(c.s, resp.data(), resp.size());
          upgraded_.push_back(static_cast<uintptr_t>(c.s));
        }
        c.upgraded = true;
        c.in.erase(0, end + 4);
      }

      // Parse as many complete frames as we have.
      bool closed = false;
      while (c.in.size() >= 2) {
        const auto *d = reinterpret_cast<const unsigned char *>(c.in.data());
        const bool fin = (d[0] & 0x80) != 0;
        const uint8_t opcode = d[0] & 0x0F;
        const bool masked = (d[1] & 0x80) != 0;
        uint64_t len = d[1] & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
          if (c.in.size() < 4) break;
          len = (uint64_t{d[2]} << 8) | d[3];
          hdr = 4;
        } else if (len == 127) {
          closed = true; // we never accept messages this large
          break;
        }
        if (!masked || !fin || len > options_.maxMessageBytes) {
          closed = true; // clients must mask; we don't support fragmentation
          break;
        }
        if (c.in.size() < hdr + 4 + len) break;
        const unsigned char *mask = d + hdr;
        std::string payload(static_cast<size_t>(len), '\0');
        for (size_t k = 0; k < len; ++k) payload[k] = static_cast<char>(d[hdr + 4 + k] ^ mask[k % 4]);
        c.in.erase(0, hdr + 4 + static_cast<size_t>(len));

        if (opcode == 0x1) {
          if (onMessage_) onMessage_(payload);
        } else if (opcode == 0x9) {
          reply(c.s, 0xA, payload);
        } else if (opcode == 0x8) {
          reply(c.s, 0x8, {});
          closed = true;
          break;
        } else if (opcode != 0xA) {
          closed = true; // binary or unknown
          break;
        }
      }
      if (closed) drop(i);
    }
  }
  {
    std::lock_guard lock(sendMutex_);
    upgraded_.clear();
  }
  for (auto &c : clients) closesocket(c.s);
}

void SyncServer::Broadcast(const std::string &text) {
  std::lock_guard lock(sendMutex_);
  for (const uintptr_t s : upgraded_) SendFrame(static_cast<SOCKET>(s), 0x1, text);
}

} // namespace sp
