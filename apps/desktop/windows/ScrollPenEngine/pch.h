#pragma once

#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <uiautomation.h>
#include <psapi.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "third_party/nlohmann/json.hpp"
