# Registers ScrollPenHost.exe as the Chrome native-messaging host for the ScrollPen Browser
# Sync extension, for the current Windows user only (HKCU). Used by the installer, and by
# developers after building ScrollPenHost (add -Chromium for Playwright's Chromium).
#   pwsh register-native-host.ps1 -HostExe <path\to\ScrollPenHost.exe> [-Chromium]
param(
  [Parameter(Mandatory)] [string] $HostExe,
  [switch] $Chromium
)
$ErrorActionPreference = 'Stop'
$name = 'com.scrollpen.sync'
$extensionId = 'egihgjpnabmnoljofdkeakgplgjeidai' # keep in sync with LocalPeer.h kExtensionId

# Chrome runs the copy in the user's profile, so rebuilding or moving the source never
# breaks (or silently changes) what Chrome starts.
$dir = Join-Path $env:LOCALAPPDATA 'ScrollPen\NativeHost'
New-Item -ItemType Directory -Force $dir | Out-Null
Get-Process ScrollPenHost -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item -LiteralPath $HostExe -Destination (Join-Path $dir 'ScrollPenHost.exe') -Force

$manifest = Join-Path $dir "$name.json"
[ordered]@{
  name            = $name
  description     = 'ScrollPen Browser Sync link to the ScrollPen app on this PC'
  path            = 'ScrollPenHost.exe'
  type            = 'stdio'
  allowed_origins = @("chrome-extension://$extensionId/")
} | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding ascii

$browsers = @('Google\Chrome') + $(if ($Chromium) { @('Chromium') } else { @() })
foreach ($b in $browsers) {
  $key = "HKCU:\Software\$b\NativeMessagingHosts\$name"
  New-Item -Path $key -Force | Out-Null
  Set-Item -Path $key -Value $manifest
}
Write-Output "Registered $name for $($browsers -join ', '): $manifest"
