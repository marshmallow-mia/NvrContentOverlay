#!/bin/bash
# Runs NvrContentOverlay.dll through test/host.exe under Wine (Linux), for where run.ps1 can't run: the pack paths and
# the plugin's log lines in both plugin layouts (bin\win10\plugins before nEVR 6.0, <nevr root>\plugins from 6.0 on).
# run.ps1 has the full set of refusals.
#
#   test/run-wine.sh build/NvrContentOverlay.dll build/host.exe
#
# Uses its own WINEPREFIX under the work folder, with DISPLAY and WAYLAND_DISPLAY unset.
set -uo pipefail
dll=$(readlink -f "${1:?NvrContentOverlay.dll}"); hostexe=$(readlink -f "${2:?host.exe}")
work=$(mktemp -d "${TMPDIR:-$PWD}/nco-wine-XXXXXX")
export WINEPREFIX="$work/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
unset DISPLAY WAYLAND_DISPLAY
pkg=48037dc70b0ecab2
pass=0; fail=0
check() { if eval "$1"; then echo "  ok   $2"; pass=$((pass + 1)); else echo "  FAIL $2"; fail=$((fail + 1)); fi; }
win() { winepath -w "$1" 2>/dev/null; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

# new_game <name> [root] [packinbin]: a fake install with the stock manifest + package _0 and a pack 't'
# (manifest + package _1); "root" puts the plugin (and the pack) under <name>/nevr/echovr.
new_game() {
    local name=$1 layout=${2:-} packinbin=${3:-}
    G_ROOT=$work/$name; G_BIN=$G_ROOT/bin/win10; G_DATA=$G_ROOT/_data/5932408047/rad15/win10
    if [ "$layout" = root ]; then G_HOME=$G_ROOT/nevr/echovr; else G_HOME=$G_BIN; fi
    if [ -n "$packinbin" ]; then G_PACK=$G_BIN/content/t; else G_PACK=$G_HOME/content/t; fi
    mkdir -p "$G_HOME/plugins" "$G_DATA/manifests" "$G_DATA/packages" "$G_PACK/manifests" "$G_PACK/packages" "$G_BIN"
    cp "$hostexe" "$G_BIN/host.exe"; cp "$dll" "$G_HOME/plugins/NvrContentOverlay.dll"
    printf 'STOCK-MANIFEST\n' > "$G_DATA/manifests/$pkg"; printf 'STOCK-PKG0\n' > "$G_DATA/packages/${pkg}_0"
    printf 'PACK-MANIFEST\n' > "$G_PACK/manifests/$pkg"; printf 'PACK-PKG1\n' > "$G_PACK/packages/${pkg}_1"
    cat > "$G_PACK/content.json" <<EOF
{"format": 1, "id": "t", "name": "Test", "version": "1.2.3", "data_dir": "_data/5932408047/rad15/win10",
 "manifests": [{"package": "$pkg", "stock_sha256": "$(sha "$G_DATA/manifests/$pkg")", "stock_packages": 1,
                "file": "manifests/$pkg", "packages": ["packages/${pkg}_1"]}],
 "scripts": [], "files": {"manifests/$pkg": "$(sha "$G_PACK/manifests/$pkg")",
                          "packages/${pkg}_1": "$(sha "$G_PACK/packages/${pkg}_1")"}}
EOF
}

# run <host args...>: OUT = what host.exe printed; the plugin's log lines are in it (host.cpp's StubLog), since the
# plugin writes no log file.
run() {
    OUT=$(NCO_TEST_DLL=$(win "$G_HOME/plugins/NvrContentOverlay.dll") WINEPATH= wine "$G_BIN/host.exe" "$@" 2>&1 | tr -d '\r')
}

wineboot -i >/dev/null 2>&1; wineserver -w

echo "serving a pack (plugin in bin\\win10\\plugins, before nEVR 6.0)"
new_game serve
run '{"active":"t"}' --read "$(win "$G_DATA/manifests/$pkg")" --read "$(win "$G_DATA/packages/${pkg}_1")"
check '[[ $OUT == *"init 0"* ]]' 'init succeeds'
check '[[ $OUT == *"$pkg: PACK-MANIFEST"* ]]' 'the manifest comes from bin\win10\content\t'
check '[[ $OUT == *"${pkg}_1: PACK-PKG1"* ]]' 'the added package comes from the pack'
check '[[ $OUT == *"STUBLOG INFO [NEVR.PLUGIN] plugin=NvrContentOverlay pack '"'"'t'"'"' 1.2.3: 2 files checked"* ]]' 'logs the checked pack through the host'

echo "serving a pack from nEVR's root (6.0 on)"
new_game root root
run '{"active":"t"}' --read "$(win "$G_DATA/manifests/$pkg")" --read "$(win "$G_DATA/packages/${pkg}_1")"
check '[[ $OUT == *"init 0"* ]]' 'init succeeds'
check '[[ $OUT == *"$pkg: PACK-MANIFEST"* ]]' 'the manifest comes from <root>\content\t'
check '[[ $OUT == *"${pkg}_1: PACK-PKG1"* ]]' 'the added package comes from the pack under the root'
check '[[ $OUT == *"STUBLOG INFO [NEVR.PLUGIN] plugin=NvrContentOverlay pack '"'"'t'"'"' 1.2.3: 2 files checked"* ]]' 'logs the checked pack through the host, under the root'
check '[ ! -e "$G_BIN/plugin_logs" ] && [ -z "$(find "$G_BIN" -newer "$G_BIN/host.exe" ! -path "$G_BIN")" ]' 'writes nothing in the game folder'

echo "a pack left in the game folder, plugin in nEVR's root"
new_game rootbin root packinbin
run '{"active":"t"}' --read "$(win "$G_DATA/manifests/$pkg")"
check '[[ $OUT == *"init 1"* ]]' 'init fails'
check '[[ $OUT == *"STOCK-MANIFEST"* ]]' 'the game stays stock'
check '[[ $OUT == *"STUBLOG WARNING [NEVR.PLUGIN] plugin=NvrContentOverlay pack '"'"'t'"'"': no content.json"* ]]' 'logs a warning that the root has no such pack'

wineserver -k 2>/dev/null
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] && rm -rf -- "$work"
[ "$fail" -eq 0 ]
