# Builds everything a release needs and packs it into release\ScrollPen-Setup.exe with
# IExpress (built into Windows): the extension, ScrollPenHost.exe, the signed Release MSIX
# and its x64 dependencies, the public signing certificate and the setup script.
#
# Signing uses the certificate "ScrollPen sideload signing" (CN=ScrollPen Developer, matching the package
# Publisher) in Cert:\CurrentUser\My. Its private key is non-exportable and never written
# to disk; create it once with:
#   New-SelfSignedCertificate -Type Custom -Subject 'CN=ScrollPen Developer' -KeyUsage DigitalSignature `
#     -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 `
#     -FriendlyName 'ScrollPen sideload signing' -CertStoreLocation Cert:\CurrentUser\My `
#     -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') -NotAfter (Get-Date).AddYears(3)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..\..").Path
$windows = (Resolve-Path "$PSScriptRoot\..").Path
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'

$cert = Get-ChildItem Cert:\CurrentUser\My |
  Where-Object { $_.Subject -eq 'CN=ScrollPen Developer' -and $_.FriendlyName -eq 'ScrollPen sideload signing' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
  Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) { throw 'Signing certificate not found (see the comment at the top of this script).' }

function Invoke-Checked([string]$what, [scriptblock]$run) {
  & $run
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)." }
}

Invoke-Checked 'Extension build' { npm run ext:build --prefix $root }
Invoke-Checked 'ScrollPenHost build' {
  & $msbuild "$windows\ScrollPenHost\ScrollPenHost.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
}
Get-Process ScrollPen -ErrorAction SilentlyContinue | Stop-Process -Force
Invoke-Checked 'App build' {
  & $msbuild "$windows\ScrollPen.sln" /restore /p:Configuration=Release /p:Platform=x64 /m /v:minimal `
    /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:GenerateAppxPackageOnBuild=true `
    /p:AppxPackageSigningEnabled=true /p:PackageCertificateThumbprint=$($cert.Thumbprint) /p:RestorePackagesConfig=true
}

$pkgDir = Get-ChildItem "$windows\ScrollPen.Package\AppPackages" -Directory -Filter 'ScrollPen.Package_*_x64_Test' |
  Sort-Object LastWriteTime | Select-Object -Last 1
$msix = Get-ChildItem $pkgDir.FullName -Filter '*.msix' | Select-Object -First 1
$signer = (Get-AuthenticodeSignature $msix.FullName).SignerCertificate
if (-not $signer -or $signer.Thumbprint -ne $cert.Thumbprint) { throw "$($msix.Name) is not signed with the ScrollPen certificate." }
$hostExe = "$windows\x64\Release\ScrollPenHost.exe"
$extDist = "$root\extensions\browser-sync\dist"
foreach ($p in $hostExe, "$extDist\manifest.json") { if (-not (Test-Path $p)) { throw "Missing $p" } }
if (Get-ChildItem $extDist -Recurse -Filter '*.map') { throw 'Release extension build must not contain source maps.' }

$out = Join-Path $root 'release'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $msix.FullName "$stage\ScrollPen.msix"
Export-Certificate -Cert $cert -FilePath "$stage\ScrollPen.cer" | Out-Null # public part only
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\register-native-host.ps1", $hostExe -Destination $stage
Get-ChildItem "$($pkgDir.FullName)\Dependencies\x64" -File | Copy-Item -Destination $stage
Compress-Archive -Path "$extDist\*" -DestinationPath "$stage\ChromeExtension.zip"

$files = Get-ChildItem $stage -File
$target = Join-Path $out 'ScrollPen-Setup.exe'
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$target
FriendlyName=ScrollPen Setup
AppLaunched=powershell.exe -NoProfile -ExecutionPolicy Bypass -File install.ps1
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles
[Strings]
$(($files | ForEach-Object -Begin { $i = 0 } -Process { "FILE$i=`"$($_.Name)`""; $i++ }) -join "`r`n")
[SourceFiles]
SourceFiles0=$stage\
[SourceFiles0]
$(($files | ForEach-Object -Begin { $i = 0 } -Process { "%FILE$i%="; $i++ }) -join "`r`n")
"@
$sedPath = Join-Path $out 'ScrollPen-Setup.sed'
Set-Content -Path $sedPath -Value $sed -Encoding ascii
if (Test-Path $target) { Remove-Item $target -Force }
Start-Process "$env:WINDIR\System32\iexpress.exe" -ArgumentList '/N', '/Q', $sedPath -Wait
if (-not (Test-Path $target)) { throw 'IExpress did not produce the setup file.' }
Get-Item $target
