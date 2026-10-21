# ScrollPen uninstall: installed by setup to %LocalAppData%\ScrollPen\Uninstall, with a
# Start-menu shortcut. Removes the app, the Chrome link helper, the unpacked extension and
# the sideload certificate setup trusted (one UAC prompt). Settings in %AppData%\ScrollPen
# are kept unless -RemoveSettings is given.
param([switch] $RemoveSettings)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -AssemblyName System.Windows.Forms

function Show([string]$text, [string]$icon = 'Information') {
  [System.Windows.Forms.MessageBox]::Show($text, 'ScrollPen Uninstall', 'OK', $icon) | Out-Null
}

try {
  # Written by setup. Only a 40-digit hex thumbprint ever reaches the elevated command.
  $thumbFile = Join-Path $here 'certificate.thumbprint'
  $thumb = if (Test-Path $thumbFile) { (Get-Content -LiteralPath $thumbFile -Raw).Trim().ToUpperInvariant() } else { '' }
  if ($thumb -and $thumb -notmatch '^[0-9A-F]{40}$') { throw "Unexpected certificate thumbprint in $thumbFile." }

  Get-Process ScrollPen, ScrollPenHost -ErrorAction SilentlyContinue | Stop-Process -Force
  Get-AppxPackage -Name ScrollPen | Remove-AppxPackage

  foreach ($b in 'Google\Chrome', 'Chromium') {
    Remove-Item "HKCU:\Software\$b\NativeMessagingHosts\com.scrollpen.sync" -Force -ErrorAction SilentlyContinue
  }
  Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'Uninstall ScrollPen.lnk') -Force -ErrorAction SilentlyContinue
  if ($RemoveSettings) { Remove-Item (Join-Path $env:APPDATA 'ScrollPen') -Recurse -Force -ErrorAction SilentlyContinue }

  # Another ScrollPen install for a different Windows user may still need the certificate:
  # it only goes once no user on this PC has ScrollPen installed (asked from the elevated
  # step, which can see every user's packages).
  if ($thumb -and (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb")) {
    $cmd = "if (-not (Get-AppxPackage -AllUsers -Name ScrollPen)) { Remove-Item 'Cert:\LocalMachine\TrustedPeople\$thumb' }"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
      -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded
    if ($p.ExitCode -ne 0) { throw 'The ScrollPen certificate could not be removed (administrator permission is required).' }
  }

  # Last: this script lives inside this folder.
  Remove-Item (Join-Path $env:LOCALAPPDATA 'ScrollPen') -Recurse -Force -ErrorAction SilentlyContinue
  Show(@"
ScrollPen is uninstalled.

Remove "ScrollPen Browser Sync" in chrome://extensions too (its folder is gone).
"@)
} catch {
  Show "Uninstall failed:`n$($_.Exception.Message)" 'Error'
  exit 1
}
