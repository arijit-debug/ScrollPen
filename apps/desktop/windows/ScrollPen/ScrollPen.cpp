// ScrollPen.cpp : Defines the entry point for the application.
//

#include "pch.h"
#include "ScrollPen.h"

#include <dwmapi.h>

#include "AutolinkedNativeModules.g.h"

#include "NativeModules.h"
#include "ScrollPenModule.h"

// A PackageProvider containing any turbo modules you define within this app project
struct CompReactPackageProvider
    : winrt::implements<CompReactPackageProvider, winrt::Microsoft::ReactNative::IReactPackageProvider> {
 public: // IReactPackageProvider
  void CreatePackage(winrt::Microsoft::ReactNative::IReactPackageBuilder const &packageBuilder) noexcept {
    AddAttributedModules(packageBuilder, true);
  }
};

// WinAppSDK keeps WS_DLGFRAME on the borderless dock (and re-adds it if removed), which
// paints a light 3-D frame around it. Give the whole window to the client area instead.
static WNDPROC g_dockBaseProc = nullptr;

static LRESULT CALLBACK DockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCALCSIZE && wp) return 0;
  return CallWindowProcW(g_dockBaseProc, hwnd, msg, wp, lp);
}

// RN's tooltip popup opens over the neighbouring dock button and can linger after the
// pointer leaves, swallowing clicks meant for that button. Make it click-through.
static void CALLBACK OnWindowShown(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
  if (idObject != OBJID_WINDOW || !hwnd) return;
  wchar_t cls[32]{};
  if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"RN_TOOLTIP") != 0) return;
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TRANSPARENT) return;
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
}

// The entry point of the Win32 application
_Use_decl_annotations_ int CALLBACK WinMain(HINSTANCE /* instance */, HINSTANCE, PSTR /* commandLine */, int /* showCmd */) {
  // One ScrollPen per user session: a second copy would fight over hotkeys and the sync port.
  HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\ScrollPen.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(L"ScrollPenController", nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(existing, &pid);
      AllowSetForegroundWindow(pid);
    }
    return 0;
  }

  // Initialize WinRT
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  // Enable per monitor DPI scaling
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Find the path hosting the app exe file
  WCHAR appDirectory[MAX_PATH];
  GetModuleFileNameW(NULL, appDirectory, MAX_PATH);
  PathCchRemoveFileSpec(appDirectory, MAX_PATH);

  // Create a ReactNativeWin32App with the ReactNativeAppBuilder
  auto reactNativeWin32App{winrt::Microsoft::ReactNative::ReactNativeAppBuilder().Build()};

  // Configure the initial InstanceSettings for the app's ReactNativeHost
  auto settings{reactNativeWin32App.ReactNativeHost().InstanceSettings()};
  // Register any autolinked native modules
  RegisterAutolinkedNativeModulePackages(settings.PackageProviders());
  // Register any native modules defined within this app project
  settings.PackageProviders().Append(winrt::make<CompReactPackageProvider>());

#if BUNDLE
  // Load the JS bundle from a file (not Metro):
  // Set the path (on disk) where the .bundle file is located
  settings.BundleRootPath(std::wstring(L"file://").append(appDirectory).append(L"\\Bundle\\").c_str());
  // Set the name of the bundle file (without the .bundle extension)
  settings.JavaScriptBundleFile(L"index.windows");
  // Disable hot reload
  settings.UseFastRefresh(false);
#else
  // Load the JS bundle from Metro
  settings.JavaScriptBundleFile(L"index");
  // Enable hot reload
  settings.UseFastRefresh(true);
#endif
#if _DEBUG
  // For Debug builds
  // Enable Direct Debugging of JS
  settings.UseDirectDebugger(true);
  // Enable the Developer Menu
  settings.UseDeveloperSupport(true);
#else
  // For Release builds:
  // Disable Direct Debugging of JS
  settings.UseDirectDebugger(false);
  // Disable the Developer Menu
  settings.UseDeveloperSupport(false);
#endif

  // The app window is ScrollPen's floating dock: small, frameless, always on top.
  auto appWindow{reactNativeWin32App.AppWindow()};
  appWindow.Title(L"ScrollPen");
  appWindow.IsShownInSwitchers(true);
  if (auto presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
    // No system frame; the dock draws its own 1 px border.
    presenter.SetBorderAndTitleBar(false, false);
    presenter.IsResizable(false);
    presenter.IsMaximizable(false);
    presenter.IsAlwaysOnTop(true);
    presenter.PreferredMinimumWidth(1);
    presenter.PreferredMinimumHeight(1);
  }

  HWND dock = winrt::Microsoft::UI::GetWindowFromWindowId(appWindow.Id());
  ScrollPen::g_dockHwnd = dock;
  // Windows won't size an overlapped window below SM_CXMIN (~136 px at 100%), far wider
  // than the dock; popups have no such minimum.
  SetWindowLongPtrW(dock, GWL_STYLE, GetWindowLongPtrW(dock, GWL_STYLE) | WS_POPUP);
  g_dockBaseProc =
      reinterpret_cast<WNDPROC>(SetWindowLongPtrW(dock, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DockProc)));
  SetWindowPos(dock, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND; // Windows 11; ignored on 10
  DwmSetWindowAttribute(dock, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  // Windows 11 draws a light 1 px border around rounded windows; the dock has its own.
  const COLORREF noBorder = DWMWA_COLOR_NONE;
  DwmSetWindowAttribute(dock, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
  // Delivered on this thread's message loop, which also owns the tooltip windows.
  SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &OnWindowShown, GetCurrentProcessId(), 0,
                  WINEVENT_OUTOFCONTEXT);
  const UINT dpi = GetDpiForWindow(dock);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int dockW = MulDiv(64, dpi, 96), dockH = MulDiv(700, dpi, 96); // DOCK_WIDTH/HEIGHT in Dock.tsx
  appWindow.MoveAndResize({work.left + MulDiv(16, dpi, 96), work.top + (work.bottom - work.top - dockH) / 2, dockW, dockH});

  // Get the ReactViewOptions so we can set the initial RN component to load
  auto viewOptions{reactNativeWin32App.ReactViewOptions()};
  viewOptions.ComponentName(L"ScrollPen");

  // Start the app
  reactNativeWin32App.Start();

  sp::Engine::Instance().Stop();
  if (singleInstance) CloseHandle(singleInstance);
  return 0;
}
