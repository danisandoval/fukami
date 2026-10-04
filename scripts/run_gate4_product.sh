#!/bin/zsh -f
# Launch the default product: the Gate-3 guest-time composition with the
# kept Gate-4 performance changes (owner decision 2026-09-26; see
# docs/evidence/PRODUCT_DEFAULT_GATE3_GATE4_2026-09-26.md).
#
# Usage: scripts/run_gate4_product.sh [--headless] [--unpaced] [--inline] [--render-mode field|full] [--scale 1..8] [--no-aa1] [--fxaa] [--cas 0..100] [--aniso 0|2|4|8|16] [--no-mipmap] [--no-analog] [--no-rumble] [--car-lod FACTOR] [--draw-distance N] [--no-fast-unpack] [--no-native-code] [--ratio 4:3|16:9|16:10|21:9] [--widescreen] [--hud fixed|race|stretch|no47] [--aspect auto|4:3|16:9|16:10|21:9|stretch] [--window WxH] [--fullscreen] [--integer-scaling] [--no-present-pacing] [--workload DIR] [--replay DIR [--from START]] [--runtime NAME] [/path/to/user-owned.elf]
#
# --render-mode full|field: full (the default, owner decision 2026-09-26) is
# Gate-6 full-frame rendering at 448+ real rows; field is the Gate-4 field
# output. Full needs a full-capable runtime (the default runtime is one).
# --scale 1..8 sets the full-mode internal scale (default 4, owner decision
# 2026-09-26).
# --no-aa1 turns off PCSX2's AA1 edge emulation (on by default in full mode,
# owner decision 2026-09-26).
# --fxaa turns on PCSX2's FXAA pass; --cas N (1..100) turns on CAS sharpening at
# N% (0 = off). Both are presentation-only, windowed sessions only, off by
# default (owner decision 2026-09-29).
# --aniso N (2, 4, 8, 16) turns on anisotropic texture filtering (default off);
# --no-mipmap turns off PCSX2's GS mipmap emulation (default on, full mode).
# --no-analog makes the controller a digital-only pad (no sticks or trigger
# pressure); --no-rumble turns off vibration. Both default on (owner decision
# 2026-09-29).
# --car-lod FACTOR is an RR5 enhancement (RRV_RR5_CAR_LOD): the game's car LOD
# distance thresholds are scaled by FACTOR, capped at its cull distance. It
# changes guest data. Default 4 (owner decision 2026-09-27); --car-lod 1 is
# the stock game. `best` (all cars at top
# detail) overflows the game's per-field display list and hangs a race.
# --draw-distance N is an RR5 enhancement (RRV_RR5_DRAW_DISTANCE): the track
# scenery is drawn from the game's baked per-section visibility lists; N adds
# the lists of the next N sections ahead, so far scenery appears sooner
# instead of popping in. 0 is the stock game (default); 2 is the sweet spot,
# higher costs speed for little more. It changes what the guest draws.
# --no-fast-unpack turns off an RR5 enhancement (RRV_RR5_FAST_UNPACK, on by
# default, owner decision 2026-09-30): the game's LZSS unpacker (func_221D68)
# runs natively and costs almost no guest time, so loading does not freeze
# the picture (the boot Namco line animation froze for 20 + 21 fields). Same
# unpacked bytes; it changes guest time only.
# --no-native-code turns off native hot functions (RRV_RR5_NATIVE_HOT, on by
# default, owner direction 2026-09-30): the game's busiest functions run as
# native versions of their generated code with the guest clock kept exact
# (tools/ee-native). Host CPU only; the guest sees identical state.
# --aspect auto|4:3|16:9|stretch picks how the picture fits the window (Gate 7;
# default auto = 4:3 with bars). 16:9 and stretch widen the same picture; they
# do not show more of the scene. --window WxH sets the starting window size
# (default: 4:3, or 16:9 with --aspect 16:9, at 80% of the screen height);
# --fullscreen starts fullscreen (F11 toggles); --integer-scaling snaps the
# picture to whole multiples. Presentation only: the guest is unchanged.
# --no-present-pacing turns off even frame pacing (RRV_PCSX2_GS_PRESENT_PACING;
# on by default, T-PRESENT-PACING): frames are then shown as soon as they are
# ready, about 6-9 ms sooner but with judder on a 120 Hz display.
# --ratio 4:3|16:9|16:10|21:9 is real widescreen (RR5 enhancement,
# RRV_RR5_WIDESCREEN): the game's camera renders a wider view (No.47's
# mechanism, every camera) and the picture is shown at that ratio. The
# attract cinematic is the exception: it is already a wide letterboxed picture
# (X/Y factors 0.8/0.368), so it keeps its width, its Y factors are scaled by
# ratio/(4/3) and its two letterbox quads are drawn with zero height: it fills
# the screen with the view it was built for (src/product/patches.cpp). 4:3 is the
# stock game. It changes guest data; 2D elements (HUD, text) are shown
# widened unless the HUD fix is on: with a wide --ratio the bridge gives the
# 2D (HUD, menus, text) back its 4:3 proportions, each element pinned to its
# screen edge (RRV_PCSX2_GS_HUD_SCALE; host-side only). --hud picks when:
# fixed everywhere; race only during races (menus stay stretched;
# RRV_PCSX2_GS_HUD_MODE=race); stretch (alias no47, and the old
# --stretched-hud) never, which is what No.47's hack shows: it changes only
# the 3D camera. Defaults (owner decision 2026-09-27): --ratio 16:9 --hud
# stretch; --ratio 4:3 is the stock game.
# --widescreen is --ratio 16:9.
#
# The runtime package (runtime/NAME, made by scripts/package_gate4_product.py)
# is verified file by file before every launch. The workload (made by
# scripts/gate3_product_workload.py from your own verified Gate-3 workload)
# binds your disc, the guest clock and a persistent memory card. Live pad input
# is admitted at each guest VBlank start and logged per session, so a session
# can be replayed. Saves live in local/product/mc and persist across sessions.
# Real-time pacing (RRV_GATE3_REALTIME=1) holds the guest to 60000/1001
# effective VBlank starts per host second; --unpaced runs as fast as the host can.
#
# VIF1, VU1 and GS run on one owner thread by default
# (RRV_VU1GS_EXECUTION=owner-async with RRV_GS_EXECUTION=worker-sync; owner
# decision 2026-09-26, docs/evidence/GATE4_VU1_GS_WORKER_S1_S4_2026-09-26.md).
# --inline runs them on the EE thread, as before.
set -eu
root="${0:A:h:h}"
runtime_name="game001-gate9-v155-9cdff06"
usage='Usage: ./run.sh [--headless] [--unpaced] [--inline] [--render-mode field|full] [--scale 1..8] [--no-aa1] [--fxaa] [--cas 0..100] [--aniso 0|2|4|8|16] [--no-mipmap] [--no-analog] [--no-rumble] [--car-lod FACTOR] [--draw-distance N] [--no-fast-unpack] [--no-native-code]
                [--ratio 4:3|16:9|16:10|21:9] [--widescreen] [--hud fixed|race|stretch|no47] [--aspect auto|4:3|16:9|16:10|21:9|stretch] [--window WxH] [--fullscreen] [--integer-scaling] [--no-present-pacing]
                [--workload DIR] [--replay DIR [--from START]] [--runtime NAME] [--config FILE] [/path/to/user-owned.elf]
       Defaults come from rrv.ini (edit it); flags override it for one launch.
       ./run.sh --legacy-product [legacy options]   (the previous default package)
       ./run.sh --gate3-control [elf]               (the frozen c79d3c0 control)'

# ---- settings: rrv.ini and the flags above are translated by the product binary ------------------------
# The ini schema, the flag mapping, validation and the environment the game receives live in one place,
# src/app/fukami_settings.cpp, and are reached through the runtime's own
#   Fukami --print-launch-config|--print-launch-env INI [options] --flags "$@"
# (RRV_LAUNCH_ENV_TOOL=PATH uses another build of the same entry point; the asset-free tests use it).
# Nothing here parses the ini or the flags. [env] lines beyond the documented allowlist need
# `[developer] enabled = true` in the ini, or RRV_DEVELOPER=1 in the environment of this launcher.
ini="$root/rrv.ini"
for (( i = 1; i <= $#; i++ )); do
 case "${argv[i]}" in
  --config)
   (( i < $# )) || { print -u2 -- "$usage"; exit 2; }
   ini="${argv[i+1]:A}" ;;
  --help|-h) print -- "$usage"; exit 0 ;;
 esac
done
if [[ ! -f "$ini" ]] && (( ${argv[(I)--config]} )); then
 print -u2 -- "Config file not found: $ini"; exit 2
fi
dev=()
case "${RRV_DEVELOPER:-}" in 1|true|yes|on) dev=(--developer) ;; esac
menu=(); [[ -f "$ini" ]] && menu=(--menu-ini)

# The game executable of a runtime package: bin/Fukami, or bin/rrv-gate3-candidate in packages made
# before the rename (v154 and older).
runtime_exe() { # RUNTIME
 local exe="$root/runtime/$1/bin/Fukami"
 [[ -x "$exe" ]] || exe="$root/runtime/$1/bin/rrv-gate3-candidate"
 print -r -- "$exe"
}
launch_tool() { # RUNTIME ARGS...: the runtime's own binary, or the test tool
 local rt="$1"; shift
 local tool="${RRV_LAUNCH_ENV_TOOL:-$(runtime_exe "$rt")}"
 # Runtimes built before v143 cannot answer --print-launch-*; a developer checkout can use the same module
 # built from this tree (cmake --build build-release --target rrv-launch-env).
 local fallback="${RRV_LAUNCH_ENV_FALLBACK:-$root/build-release/rrv-launch-env}"
 local err rc=0 attempt
 # portable: BSD and GNU mktemp differ on -t, and a recorded TMPDIR may not exist on this host
 err="$(mktemp "${TMPDIR:-/tmp}/rrv-launch.XXXXXX" 2>/dev/null || mktemp /tmp/rrv-launch.XXXXXX)"
 for attempt in "$tool" "$fallback"; do
  if [[ ! -x "$attempt" ]]; then rc=3; continue; fi
  rc=0
  env DYLD_LIBRARY_PATH="$root/runtime/$rt/lib" "$attempt" "$@" 2>"$err" || rc=$?
  (( rc == 0 )) && break
  grep -q 'user-owned-boot-elf' "$err" || break   # a real error from a runtime that knows the entry point
  rc=3
 done
 if (( rc == 3 )); then
  print -u2 -- "Runtime $rt has no launcher entry point (--print-launch-env: runtimes before v143) and $fallback is not built."
  print -u2 -- "Use a newer runtime (--runtime NAME) or run: cmake --build build-release --target rrv-launch-env"
 elif (( rc != 0 )); then
  cat "$err" >&2
 fi
 rm -f "$err"
 return $rc
}
read_config() { # fills cfg from the product binary
 local text
 text="$(launch_tool "$runtime_name" --print-launch-config "$ini" --root "$root" "${dev[@]}" --flags "$@")" || return $?
 cfg=()
 local l
 for l in "${(@f)text}"; do cfg[${l%%=*}]="${l#*=}"; done
}
typeset -A cfg
read_config "$@" || exit $?
if [[ -n "${cfg[runtime]}" && "${cfg[runtime]}" != "$runtime_name" ]]; then
 runtime_name="${cfg[runtime]}"   # [paths] runtime or --runtime names another package: ask that one
 read_config "$@" || exit $?
fi
workload="${cfg[workload]:-$root/local/gate3/product-live-v1}"
replay="${cfg[replay]}"
elf="${cfg[elf]}"
ff_from="${cfg[from]}"
headless="${cfg[headless]}"
unpaced="${cfg[unpaced]}"
inline="${cfg[inline]}"

# RRV_LAUNCH_DRY_RUN=1 prints the runtime environment instead of launching,
# with no side effects (tests/test_launch_env_golden.py compares it with the
# environment the bash launcher produced before the translation moved into the
# product binary).
dry_run=${RRV_LAUNCH_DRY_RUN:-}
[[ -n "$dry_run" ]] || python3 "$root/scripts/package_gate4_product.py" --verify --name "$runtime_name" >&2
package="$root/runtime/$runtime_name"

# A replay (scripts/make_replay.py) runs a recorded session again from its own
# copy of the card; it never reads or writes local/product/mc.
[[ -n "$replay" ]] && workload="$replay"
if [[ -n "$replay" && ! -d "$replay/mc-initial" ]]; then
 print -u2 -- "Not a replay (no mc-initial): $replay. Make one with scripts/make_replay.py <session> <dir>."
 exit 1
fi
bound="$workload/bound-workload.txt"
if [[ ! -f "$bound" ]]; then
 print -u2 -- "Product workload not found: $workload"
 print -u2 -- "Create it once from your own verified Gate-3 workload, for example:"
 print -u2 -- "  python3 scripts/gate3_product_workload.py local/gate3/race-record-v2 local/gate3/product-live-v1"
 exit 1
fi
if ! grep -qx 'storage persistent_user_card' "$bound"; then
 print -u2 -- "Workload is not a product workload (no persistent storage binding): $workload"
 exit 1
fi
disc_elf="$workload/disc/SLUS_200.02"
if [[ -n "$elf" ]]; then
 # The workload binds exactly one user ELF; a supplied ELF must be that one.
 want="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["private_inputs"]["elf"]["sha256"])' "$workload/manifest-v3.json")"
 have="$(shasum -a 256 "$elf" | cut -d' ' -f1)"
 if [[ "$want" != "$have" ]]; then
  print -u2 -- "This ELF is not the one the product workload binds (sha256 $have, expected $want)."
  exit 1
 fi
fi

if [[ -z "$dry_run" ]] && (( $(ps -Ao comm= | grep -c -e "/bin/Fukami$" -e "/bin/rrv-gate3-candidate$") >= 2 )); then
 print -u2 -- "Two game instances are already running; refusing to start a third."
 exit 1
fi

session="$root/local/product/sessions/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mc="$root/local/product/mc"
if [[ -n "$replay" ]]; then
 [[ -n "$dry_run" ]] || { mkdir -p "$session"; cp -R "$replay/mc-initial" "$session/mc"; }
 mc="$session/mc"
elif [[ -z "$dry_run" ]]; then
 mkdir -p "$session" "$mc"
 # The card as it was at launch, so this session can become a replay later.
 cp -R "$mc" "$session/mc-start"
fi
print -u2 -- "Product: runtime $runtime_name, $([[ -n "$replay" ]] && print replay || print workload) ${workload:t}, $([[ "$unpaced" == true ]] && print unpaced || print paced)${ff_from:+ (fast-forward to start $ff_from)}, render ${cfg[render_mode]} ${cfg[scale]}x, car LOD x${cfg[car_lod]}, draw distance +${cfg[draw_distance]}, $([[ "$inline" == true ]] && print inline || print owner-async), saves $mc, session log $session"
# The environment the game process receives, from the same module (replay: recorded input, no pad log).
envlines=("${(@f)$(launch_tool "$runtime_name" --print-launch-env "$ini" --root "$root" --workload-bound "$bound" \
 --memory-card "$mc" --session "$session" --libraries "$package/lib" ${replay:+--replay} "${menu[@]}" "${dev[@]}" --flags "$@")}") || exit $?
if [[ -n "$dry_run" ]]; then
 print -l -- "${envlines[@]}"
 exit 0
fi
env -i "${envlines[@]}" \
 "$(runtime_exe "$runtime_name")" "$disc_elf" \
 > "$session/stdout.log" 2> "$session/stderr.log" && rc=0 || rc=$?
# The runtime's diagnostic output stays in the session logs; the terminal gets
# the outcome, and the last lines of the log if the session did not end cleanly.
outcome="$(grep -h -e '^\[gate3\] terminal outcome=' "$session/stderr.log" | tail -1)"
print -u2 -- "Session ended (exit $rc) ${outcome:+- ${outcome#\[gate3\] }}"
if (( rc != 0 )); then
 print -u2 -- "Last log lines ($session/stderr.log):"
 tail -n 8 "$session/stderr.log" >&2
fi
exit $rc
