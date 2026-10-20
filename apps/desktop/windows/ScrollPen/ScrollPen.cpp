// ScrollPen.cpp : Defines the entry point for the application.
//

#include "pch.h"
#include "ScrollPen.h"

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
    presenter.SetBorderAndTitleBar(true, false);
    presenter.IsResizable(false);
    presenter.IsMaximizable(false);
    presenter.IsAlwaysOnTop(true);
  }

  HWND dock = winrt::Microsoft::UI::GetWindowFromWindowId(appWindow.Id());
  ScrollPen::g_dockHwnd = dock;
  const UINT dpi = GetDpiForWindow(dock);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int dockW = MulDiv(64, dpi, 96), dockH = MulDiv(640, dpi, 96);
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
