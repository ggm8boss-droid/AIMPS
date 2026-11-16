[CmdletBinding()]
param(
    [string]$CMakeExe = '',
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$OutputDirectory = 'artifacts/alien-api',
    [string]$SecondPcApi = '\\DESKTOP-2JNROA0\win-unpacked\resources\api',
    [switch]$DeliveryOnly
)

# Build and deploy the complete API: native AIMP.NetZ, Launcher, Protocol, Injector32 and UI resources.
# No unit tests and no Electron rebuild.
# Full builds always discard old generated native/.NET files before compilation.
# -DeliveryOnly may reuse only a previously verified build of this exact revision.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$api = Join-Path $output 'api'
$native = Join-Path $output 'native-vs2026'
$localApi = Join-Path $root 'dist/win-unpacked/resources/api'
$required = @('AIMP.Launcher.dll','AIMP.Protocol.dll','AIMP.NetZ.dll','Injector32/AIMP.Injector32.exe','UIBridge/NEWFRONTENDMENU.GFX','UIBridge/PICKUPOVERLAY.GFX')
$buildInfoPath = Join-Path $output 'build-info.json'
New-Item -ItemType Directory -Path $output -Force | Out-Null

function Run([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed ($LASTEXITCODE)." }
}

function Assert-BinaryVersion([string]$Path, [string]$ExpectedVersion, [string]$TextEncoding) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Build output is missing: $Path"
    }
    # x86 AIMP.NetZ cannot be loaded into x64 PowerShell. Inspect its compiled
    # version string without loading or executing the DLL.
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($TextEncoding -eq 'Unicode') {
        $actual = [Text.Encoding]::Unicode.GetString($bytes)
    } else {
        $actual = [Text.Encoding]::ASCII.GetString($bytes)
    }
    if (!$actual.Contains($ExpectedVersion)) {
        throw "Stale or incompatible binary: $Path does not contain version $ExpectedVersion. Deployment cancelled."
    }
}

function Assert-CompiledApi {
    Assert-BinaryVersion -Path (Join-Path $api 'AIMP.NetZ.dll') -ExpectedVersion $dllVersion -TextEncoding 'ASCII'
    Assert-BinaryVersion -Path (Join-Path $api 'AIMP.Launcher.dll') -ExpectedVersion $dllVersion -TextEncoding 'Unicode'
}

function Copy-Api([string]$Destination) {
    # /E preserves files that exist only at the destination. Never use /MIR.
    & robocopy $api $Destination /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "API delivery failed: $Destination (robocopy $LASTEXITCODE)." }
    foreach ($name in $required) {
        $expected = (Get-FileHash -LiteralPath (Join-Path $api $name) -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath (Join-Path $Destination $name) -Algorithm SHA256).Hash
        if ($expected -ne $actual) { throw "SHA-256 mismatch: $Destination/$name" }
        [pscustomobject]@{ File=$name; Destination=$Destination; SHA256=$actual }
    }
}

Push-Location $root
try {
    $launcherVersionSource = Get-Content -LiteralPath (Join-Path $root 'AIMP.Launcher/src/AIMP.Launcher/ProductInfo.cs') -Raw
    $nativeVersionSource = Get-Content -LiteralPath (Join-Path $root 'AIMP.Native/include/aimp/ProductVersion.h') -Raw
    $revisionMatch = [regex]::Match($launcherVersionSource, 'BuildRevision\s*=\s*"(\d+)"')
    $dllVersionMatch = [regex]::Match($nativeVersionSource, 'kNativeVersion\[\]\s*=\s*"([^"]+)"')
    $expectedMatch = [regex]::Match($launcherVersionSource, 'ExpectedDllVersion\s*=\s*"([^"]+)"\s*\+\s*BuildRevision\s*\+\s*"([^"]+)"')
    if (!$revisionMatch.Success -or !$dllVersionMatch.Success -or !$expectedMatch.Success) {
        throw 'Unable to read Launcher/Native version constants.'
    }
    $revision = $revisionMatch.Groups[1].Value
    if ([int]$revision -lt 511) {
        throw "Source revision $revision is outdated. Pull feature/aisync-direct-locomotion (511 or newer) before rebuilding."
    }
    $dllVersion = $dllVersionMatch.Groups[1].Value
    $expectedDllVersion = $expectedMatch.Groups[1].Value + $revision + $expectedMatch.Groups[2].Value
    if ($dllVersion -cne $expectedDllVersion) {
        throw "Version mismatch: native $dllVersion but Launcher expects $expectedDllVersion. Bump both together."
    }
    Write-Host "Building/delivering AIMP API 0.8.4.$revision (native $dllVersion)"
    if (!$DeliveryOnly) {
      # A successful incremental build could still ship stale binaries when the
      # output path/cache was reused. Recreate isolated outputs every full build.
      # Never wipe the installed API destination or any source directory.
      foreach ($stalePath in @($native, $api, (Join-Path $output 'dotnet'))) {
          if (Test-Path -LiteralPath $stalePath) {
              Write-Host "Removing old build output: $stalePath"
              Remove-Item -LiteralPath $stalePath -Recurse -Force -ErrorAction Stop
          }
      }
      foreach ($manifest in @($buildInfoPath, (Join-Path $output 'delivery.json'))) {
          if (Test-Path -LiteralPath $manifest) {
              Remove-Item -LiteralPath $manifest -Force -ErrorAction Stop
          }
      }
      New-Item -ItemType Directory -Path $api -Force | Out-Null
      if (!$CMakeExe) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installation) {
                $candidate = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
                if (Test-Path -LiteralPath $candidate) { $CMakeExe = $candidate }
            }
        }
        if (!$CMakeExe) { $CMakeExe = (Get-Command cmake -ErrorAction Stop).Source }
      }
      Run $CMakeExe @('-S','AIMP.Native','-B',$native,'-G',$Generator,'-A','Win32','-DBUILD_TESTING=OFF')
      Run $CMakeExe @('--build',$native,'--config','Release','--target','AIMP.NetZ','--parallel','4')
      # Keep restore/build intermediates separate from IDE and other RID builds.
      Run 'dotnet' @('publish','AIMP.Launcher/src/AIMP.Launcher/AIMP.Launcher.csproj','-c','Release','-r','win-x64','--self-contained','true','--artifacts-path',(Join-Path $output 'dotnet'),'-o',$api)
      Run 'dotnet' @('publish','AIMP.Launcher/src/AIMP.Injector32/AIMP.Injector32.csproj','-c','Release','-r','win-x86','--self-contained','true','--artifacts-path',(Join-Path $output 'dotnet'),'-o',(Join-Path $api 'Injector32'))
      Copy-Item -LiteralPath (Join-Path $native 'bin/Release/AIMP.NetZ.dll') -Destination (Join-Path $api 'AIMP.NetZ.dll') -Force
    }
    foreach ($name in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $api $name) -PathType Leaf)) { throw "Incomplete build artifacts: $name" }
    }
    # Never bless a stale output based solely on matching source constants.
    # This check runs for both fresh builds and -DeliveryOnly.
    Assert-CompiledApi
    if ($DeliveryOnly) {
        if (!(Test-Path -LiteralPath $buildInfoPath -PathType Leaf)) {
            throw 'Missing build-info.json: build the API at least once before -DeliveryOnly.'
        }
        $built = Get-Content -LiteralPath $buildInfoPath -Raw | ConvertFrom-Json
        if ($built.dllVersion -cne $dllVersion -or $built.revision -cne $revision) {
            throw 'Cached API version differs from source. Rebuild the full API.'
        }
        foreach ($name in $required) {
            $entry = @($built.hashes | Where-Object { $_.name -ceq $name })
            if ($entry.Count -ne 1 -or $entry[0].sha256 -cne (Get-FileHash -LiteralPath (Join-Path $api $name) -Algorithm SHA256).Hash) {
                throw "Cached artifact changed since last build: $name"
            }
        }
    } else {
        $artifactHashes = @(
            foreach ($name in $required) {
                [pscustomobject]@{ name=$name; sha256=(Get-FileHash -LiteralPath (Join-Path $api $name) -Algorithm SHA256).Hash }
            }
        )
        [ordered]@{ revision=$revision; dllVersion=$dllVersion; builtAtUtc=[DateTime]::UtcNow.ToString('o'); hashes=$artifactHashes } |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $buildInfoPath -Encoding UTF8
    }

    $protocolVersion = [int][regex]::Match((Get-Content -LiteralPath (Join-Path $root 'AIMP.Launcher/src/AIMP.Protocol/ProtocolConstants.cs') -Raw), 'Version = (\d+)').Groups[1].Value
    $delivery = [ordered]@{ builtAtUtc=[DateTime]::UtcNow.ToString('o'); apiVersion="0.8.4.$revision"; nativeVersion=$dllVersion; protocol=$protocolVersion; unitTestsRun=$false; localDelivered=$false; secondPcDelivered=$false; hashes=@(); failures=@() }
    foreach ($target in @($localApi,$SecondPcApi)) {
        try {
            $delivery.hashes += @(Copy-Api $target)
            if ($target -eq $localApi) { $delivery.localDelivered=$true } else { $delivery.secondPcDelivered=$true }
        }
        catch { $delivery.failures += $_.Exception.Message; Write-Warning $_.Exception.Message }
    }
    $delivery | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'delivery.json') -Encoding UTF8
    if (!$delivery.localDelivered -or !$delivery.secondPcDelivered) {
        throw "Delivery incomplete. See $(Join-Path $output 'delivery.json')."
    }
    Write-Host "AIMP API revision $revision (native $dllVersion) deployed to both PCs; compiled-version and SHA-256 checks passed."
    # robocopy returns 1 when it copied files successfully. Do not leak that
    # success code as a build failure to callers inspecting LASTEXITCODE.
    $global:LASTEXITCODE = 0
}
finally { Pop-Location }
