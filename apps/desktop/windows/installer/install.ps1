# ScrollPen setup: runs from the IExpress-extracted temp folder (see build-setup.ps1).
# Trusts the sideload certificate (one UAC prompt), installs the MSIX and its
# dependencies for the current user, registers the Chrome link helper for this user and
# unpacks the Chrome extension.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $env:TEMP 'ScrollPen-Setup.log'
Start-Transcript -Path $log -Force | Out-Null
Add-Type -AssemblyName System.Windows.Forms

function Show([string]$text, [string]$icon = 'Information') {
  [System.Windows.Forms.MessageBox]::Show($text, 'ScrollPen Setup', 'OK', $icon) | Out-Null
}

try {
  # Read the certificate once; the elevated step gets its bytes, never a path in a folder
  # other programs could write to, and never text spliced into a command.
  $bytes = [IO.File]::ReadAllBytes((Join-Path $here 'ScrollPen.cer'))
  $thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (, $bytes)).Thumbprint
  # Never trust a certificate that didn't sign the package it came with. (The signature
  # can't verify as trusted yet, but its signer and an intact hash can be checked.)
  $sig = Get-AuthenticodeSignature (Join-Path $here 'ScrollPen.msix')
  if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $thumb -or
      $sig.Status -in 'HashMismatch', 'NotSigned', 'Incompatible') {
    throw 'ScrollPen.msix is not signed by the certificate in this setup. Download ScrollPen again.'
  }
  if (-not (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb")) {
    # MSIX sideloading only trusts certificates in the machine store, which needs admin.
    $cmd = "`$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (, [Convert]::FromBase64String('" +
      [Convert]::ToBase64String($bytes) + "'));" +
      "`$s = New-Object System.Security.Cryptography.X509Certificates.X509Store 'TrustedPeople', 'LocalMachine';" +
      "`$s.Open('ReadWrite'); `$s.Add(`$c); `$s.Close()"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
      -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded
    if ($p.ExitCode -ne 0 -or -not (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb")) {
      throw 'The ScrollPen certificate could not be trusted (administrator permission is required).'
    }
  }

  Get-Process ScrollPen, ScrollPenHost -ErrorAction SilentlyContinue | Stop-Process -Force
  # Replace any earlier install (including a developer registration). Settings in
  # %AppData%\ScrollPen are kept.
  Get-AppxPackage -Name ScrollPen | Remove-AppxPackage

  foreach ($dep in Get-ChildItem $here -Filter 'Microsoft.*' | Where-Object Extension -in '.appx', '.msix') {
    try { Add-AppxPackage -Path $dep.FullName }
    catch { Write-Output "Dependency $($dep.Name) skipped: $($_.Exception.Message)" }  # usually already installed
  }
  Add-AppxPackage -Path (Join-Path $here 'ScrollPen.msix')

  # Chrome starts this helper for the extension; it is the extension's only way to the app.
  & (Join-Path $here 'register-native-host.ps1') -HostExe (Join-Path $here 'ScrollPenHost.exe')

  $ext = Join-Path $env:LOCALAPPDATA 'ScrollPen\ChromeExtension'
  if (Test-Path $ext) { Remove-Item $ext -Recurse -Force }
  Expand-Archive -Path (Join-Path $here 'ChromeExtension.zip') -DestinationPath $ext -Force

  # The uninstaller also removes the certificate trusted above.
  $un = Join-Path $env:LOCALAPPDATA 'ScrollPen\Uninstall'
  New-Item -ItemType Directory -Force $un | Out-Null
  Copy-Item -LiteralPath (Join-Path $here 'uninstall.ps1') -Destination $un -Force
  Set-Content -LiteralPath (Join-Path $un 'certificate.thumbprint') -Value $thumb -Encoding ascii
  $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut(
    (Join-Path ([Environment]::GetFolderPath('Programs')) 'Uninstall ScrollPen.lnk'))
  $lnk.TargetPath = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $un 'uninstall.ps1')`""
  $lnk.Description = 'Remove ScrollPen, its Chrome link and its certificate'
  $lnk.Save()

  Start-Process "shell:AppsFolder\$((Get-AppxPackage -Name ScrollPen).PackageFamilyName)!App"
  Start-Process explorer.exe $ext
  Show(@"
ScrollPen is installed and running (find it in the Start menu).

To make ink follow pages in Chrome, add the ScrollPen extension once:
1. Open chrome://extensions in Chrome
2. Turn on "Developer mode" (top right)
3. Click "Load unpacked" and choose this folder (opened for you):
   $ext
   (If an older ScrollPen Browser Sync is listed, remove it first.)
4. In the extension's Details, turn on "Allow access to file URLs"
   (for local PDFs and images).

ScrollPen never checks for updates by itself. Run a newer setup to get fixes
(including for the PDF viewer), then restart Chrome so it loads the new extension.
To remove ScrollPen, use "Uninstall ScrollPen" in the Start menu.
"@)
} catch {
  Write-Output $_
  Show "Setup failed:`n$($_.Exception.Message)`n`nLog: $log" 'Error'
  exit 1
} finally {
  Stop-Transcript | Out-Null
}
