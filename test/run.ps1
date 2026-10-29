# Runs NvrContentOverlay.dll through test\host.exe on Windows, one fake install per
# scenario, and checks what it printed and logged.
#
#   powershell -ExecutionPolicy Bypass -File test\run.ps1 -Dll build\NvrContentOverlay.dll -HostExe build\host.exe
#
# host.exe isn't echovr.exe, so the script loader hook can't go in here: a pack with
# scripts is refused (tested), and the hook itself is tested in the game. Build host.exe with
#   x86_64-w64-mingw32-g++ -std=c++17 -O1 -static -o build/host.exe test/host.cpp
param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$HostExe)
$ErrorActionPreference = 'Stop'
$Dll = (Resolve-Path $Dll).Path
$HostExe = (Resolve-Path $HostExe).Path
$work = Join-Path $env:TEMP ("nco-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$pkg = '48037dc70b0ecab2'
$script:pass = 0
$script:fail = 0

function Check($ok, $what) {
    if ($ok) { "  ok   $what"; $script:pass++ } else { "  FAIL $what"; $script:fail++ }
}

function Sha($path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() }

# A fake install: stock manifest + package _0, and a pack 't' with its manifest and package _1.
# -NevrRoot: the plugin in <nevr root>\plugins (nEVR 6.0 on), so its pack and log are under that root;
# -PackInBin with it: the pack is left in bin\win10\content, where the plugin no longer looks.
function New-Game($name, [switch]$Scripts, [switch]$NevrRoot, [switch]$PackInBin) {
    $root = Join-Path $work $name
    $bin = "$root\bin\win10"
    $data = "$root\_data\5932408047\rad15\win10"
    $home_ = if ($NevrRoot) { "$root\nevr\echovr" } else { $bin }
    $pack = if ($PackInBin) { "$bin\content\t" } else { "$home_\content\t" }
    foreach ($d in "$home_\plugins", "$data\manifests", "$data\packages", "$pack\manifests", "$pack\packages", "$pack\scripts") {
        New-Item -ItemType Directory -Force $d | Out-Null
    }
    Copy-Item $HostExe "$bin\host.exe"
    Copy-Item $Dll "$home_\plugins\NvrContentOverlay.dll"
    [IO.File]::WriteAllText("$data\manifests\$pkg", "STOCK-MANIFEST`n")
    [IO.File]::WriteAllText("$data\packages\${pkg}_0", "STOCK-PKG0`n")
    [IO.File]::WriteAllText("$pack\manifests\$pkg", "PACK-MANIFEST`n")
    [IO.File]::WriteAllText("$pack\packages\${pkg}_1", "PACK-PKG1`n")
    $files = [ordered]@{ "manifests/$pkg" = (Sha "$pack\manifests\$pkg"); "packages/${pkg}_1" = (Sha "$pack\packages\${pkg}_1") }
    $scriptList = @()
    if ($Scripts) {
        [IO.File]::WriteAllText("$pack\scripts\0123456789abcdef.dll", "MZ")
        $files["scripts/0123456789abcdef.dll"] = (Sha "$pack\scripts\0123456789abcdef.dll")
        $scriptList = @("scripts/0123456789abcdef.dll")
    }
    $json = [ordered]@{
        format = 1; id = 't'; name = 'Test'; version = '1.2.3'; data_dir = '_data/5932408047/rad15/win10'
        manifests = @([ordered]@{ package = $pkg; stock_sha256 = (Sha "$data\manifests\$pkg"); stock_packages = 1
                                 file = "manifests/$pkg"; packages = @("packages/${pkg}_1") })
        scripts = $scriptList; files = $files
    }
    [IO.File]::WriteAllText("$pack\content.json", ($json | ConvertTo-Json -Depth 5))
    return @{ root = $root; bin = $bin; data = $data; pack = $pack; home = $home_; dll = "$home_\plugins\NvrContentOverlay.dll" }
}

function Run($g, [string[]]$hostArgs) {
    # Windows PowerShell drops the double quotes inside native arguments: escape them.
    $native = @($hostArgs | ForEach-Object { $_ -replace '"', '\"' })
    $env:NCO_TEST_DLL = $g.dll
    $out = & "$($g.bin)\host.exe" @native 2>&1 | Out-String
    Remove-Item Env:NCO_TEST_DLL
    $log = "$($g.home)\plugin_logs\NvrContentOverlay\NvrContentOverlay.log"
    $logText = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    return @{ out = $out; log = $logText }
}

New-Item -ItemType Directory -Force $work | Out-Null
try {
    "no pack"
    $g = New-Game 'none'
    $r = Run $g @('{}', '--read', "$($g.data)\manifests\$pkg")
    Check ($r.out -match 'info content_overlay 1\.1\.0 api 5') 'exports its info and ABI v5'
    Check ($r.out -match 'init 0') 'init succeeds'
    Check ($r.out -match 'manifests\\48037dc70b0ecab2: STOCK-MANIFEST') 'the game reads the stock manifest'
    Check ($r.log -match 'nothing to do') 'logs that it has nothing to do'

    "serving a pack"
    $g = New-Game 'serve'
    $m = "$($g.data)\manifests\$pkg"
    $r = Run $g @('{"active":"t"}', '--read', $m, '--read', "\\?\$m", '--read', ($m -replace '\\', '/'),
                  '--read', "$($g.data)\packages\${pkg}_1", '--read', "$($g.data)\packages\${pkg}_0",
                  '--write', $m, '--shutdown', '--read', $m)
    $lines = $r.out -split "`r?`n"
    Check ($r.out -match 'init 0') 'init succeeds'
    Check (($lines | Where-Object { $_ -match "^read .*manifests.48037dc70b0ecab2: PACK-MANIFEST" }).Count -eq 3) 'the manifest comes from the pack (plain, \\?\ and / paths)'
    Check ($r.out -match "${pkg}_1: PACK-PKG1") 'the added package comes from the pack'
    Check ($r.out -match "${pkg}_0: STOCK-PKG0") 'a stock package stays stock'
    Check ($r.out -match "write .*: STOCK-MANIFEST") 'an open for writing is not redirected'
    Check (($lines | Select-Object -Last 3) -match 'read .*: STOCK-MANIFEST') 'after shutdown the stock manifest is back'
    Check ($r.log -match "pack 't' 1\.2\.3: 2 files checked") 'logs the checked pack'

    "serving a pack from nEVR's root (6.0 on)"
    $g = New-Game 'root' -NevrRoot
    $r = Run $g @('{"active":"t"}', '--read', "$($g.data)\manifests\$pkg", '--read', "$($g.data)\packages\${pkg}_1")
    Check ($r.out -match 'init 0') 'init succeeds'
    Check ($r.out -match 'manifests.48037dc70b0ecab2: PACK-MANIFEST') 'the manifest comes from the pack under the root'
    Check ($r.out -match "${pkg}_1: PACK-PKG1") 'the added package comes from the pack under the root'
    Check ($r.log -match "pack 't' 1\.2\.3: 2 files checked") 'logs under the root'
    Check (-not (Test-Path "$($g.bin)\plugin_logs")) 'writes nothing in the game folder'

    "a pack left in the game folder, plugin in nEVR's root"
    $g = New-Game 'rootbin' -NevrRoot -PackInBin
    $r = Run $g @('{"active":"t"}', '--read', "$($g.data)\manifests\$pkg")
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.out -match 'STOCK-MANIFEST') 'the game stays stock'
    Check ($r.log -match 'no content.json') 'logs that the root has no such pack'

    "stock manifest not the one the pack was built from"
    $g = New-Game 'stock-changed'
    [IO.File]::WriteAllText("$($g.data)\manifests\$pkg", "SOMEONE-REPACKED`n")
    $r = Run $g @('{"active":"t"}', '--read', "$($g.data)\manifests\$pkg")
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.out -match 'SOMEONE-REPACKED') 'the game keeps its own manifest'
    Check ($r.log -match 'not stock') 'logs that the manifest is not stock'

    "a pack file changed"
    $g = New-Game 'pack-changed'
    [IO.File]::WriteAllText("$($g.pack)\packages\${pkg}_1", "TAMPERED`n")
    $r = Run $g @('{"active":"t"}')
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.log -match 'changed') 'logs the changed file'

    "a path climbing out of the pack"
    $g = New-Game 'bad-path'
    $j = Get-Content "$($g.pack)\content.json" -Raw
    [IO.File]::WriteAllText("$($g.pack)\content.json", ($j -replace '"packages/48037dc70b0ecab2_1"\s*:', '"../packages/48037dc70b0ecab2_1":'))
    $r = Run $g @('{"active":"t"}')
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.log -match 'bad file entry') 'logs the bad entry'

    "a package below the stock count"
    $g = New-Game 'low-package'
    Copy-Item "$($g.pack)\packages\${pkg}_1" "$($g.pack)\packages\${pkg}_0"
    $j = Get-Content "$($g.pack)\content.json" -Raw | ConvertFrom-Json
    $j.manifests[0].packages = @("packages/${pkg}_0")
    $j.files | Add-Member -NotePropertyName "packages/${pkg}_0" -NotePropertyValue (Sha "$($g.pack)\packages\${pkg}_0")
    [IO.File]::WriteAllText("$($g.pack)\content.json", ($j | ConvertTo-Json -Depth 5))
    $r = Run $g @('{"active":"t"}')
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.log -match "isn't one of") 'logs that it would shadow a stock package'

    "an id that is not a plain name"
    $g = New-Game 'bad-id'
    $r = Run $g @('{"active":"..\\t"}')
    Check ($r.out -match 'init 1') 'init fails'

    "loaded after the game opened its data"
    $g = New-Game 'too-late'
    $r = Run $g @('{"active":"t"}', '--hold', "$($g.data)\packages\${pkg}_0", '--read', "$($g.data)\manifests\$pkg")
    Check ($r.out -match 'hold ok') 'the stand-in game holds its package'
    Check ($r.out -match 'init 1') 'init fails'
    Check ($r.out -match 'STOCK-MANIFEST') 'the game stays stock'
    Check ($r.log -match 'loaded too late') 'logs that it came too late'

    "scripts on another exe"
    $g = New-Game 'scripts' -Scripts
    $r = Run $g @('{"active":"t"}')
    Check ($r.out -match 'init 1') 'init fails (the script loader is only known in echovr.exe 631547)'
    Check ($r.log -match "isn't the build") 'logs why'
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
""
"$($script:pass) passed, $($script:fail) failed"
if ($script:fail -gt 0) { exit 1 }
