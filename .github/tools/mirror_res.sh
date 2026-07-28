#!/usr/bin/env bash
# Mirror a project's release binaries from its upstream GitHub release to the
# xlings-res/<project> resource repo on GitHub AND GitCode, so `XLINGS_RES`
# downloads (esp. the CN path, which is GitCode-only for package binaries)
# resolve for ALL platforms.
#
# Generic over <project> — currently `xlings` and `mcpp`. Tag scheme on
# xlings-res/<project> is the BARE version (e.g. 0.4.63 / 0.0.82); the upstream
# source tag is v<version>. Asset filenames are identical on both ends (the
# xlings-res convention `<project>-<ver>-<platform>.<ext>` matches upstream).
#
# Usage: tools/mirror_res.sh <project> <version>      # e.g. mcpp 0.0.82
# Auth:  XLINGS_RES_TOKEN (github write to xlings-res), GITCODE_TOKEN (+ gtc on PATH)
# Env:   SRC_REPO / GH_DST / GTC_DST / ASSETS (space-separated) override the
#        per-project defaults below.
set -euo pipefail

PROJ="${1:?usage: mirror_res.sh <project> <version>}"
VER="${2:?usage: mirror_res.sh <project> <version>}"

# ── Per-project defaults (source repo + platform asset list) ──────
case "$PROJ" in
  xlings)
    : "${SRC_REPO:=openxlings/xlings}"
    DEFAULT_ASSETS="xlings-${VER}-linux-x86_64.tar.gz xlings-${VER}-linux-aarch64.tar.gz xlings-${VER}-macosx-arm64.tar.gz xlings-${VER}-windows-x86_64.zip"
    ;;
  mcpp)
    : "${SRC_REPO:=mcpp-community/mcpp}"
    # mcpp ships a .sha256 sidecar next to each archive; mirror both so the
    # xlings-res/mcpp release stays byte-for-byte equivalent to upstream.
    p="mcpp-${VER}"
    DEFAULT_ASSETS="${p}-linux-x86_64.tar.gz ${p}-linux-x86_64.tar.gz.sha256 ${p}-linux-aarch64.tar.gz ${p}-linux-aarch64.tar.gz.sha256 ${p}-macosx-arm64.tar.gz ${p}-macosx-arm64.tar.gz.sha256 ${p}-windows-x86_64.zip ${p}-windows-x86_64.zip.sha256"
    ;;
  *)
    echo "[mirror] unknown project '$PROJ' (expected xlings|mcpp)" >&2
    exit 2
    ;;
esac
: "${GH_DST:=xlings-res/$PROJ}"
: "${GTC_DST:=xlings-res/$PROJ}"
read -r -a ASSETS <<< "${ASSETS:-$DEFAULT_ASSETS}"

info() { echo "[mirror] $*"; }

# ── Budget: one deadline per HOST LEG, not per asset ────────────────────────
# The old per-asset cap (180s, abandon-on-expiry) failed four releases in a row
# — 0.0.94 / 0.0.97 / 0.0.105 / 2026.7.28.2 — and was tuned three times without
# anyone measuring what it was capping. Measured (probe PR #301):
#
#   GitHub US runner -> file.gitcode.com  0.012 MB/s   <- the failing path
#   same runner      <- file.gitcode.com  3.87  MB/s
#   same runner      -> github.com       16     MB/s
#   mainland-CN host -> file.gitcode.com  1.84  MB/s
#
# file.gitcode.com is a single Huawei Cloud origin in Beijing; it is the
# inbound-to-CN direction that is shaped, not the host and not the client
# (curl and urllib measure identically). At 0.012 MB/s a 34.8MB asset needs
# ~45 MINUTES, so no per-asset value in the 180s neighbourhood was ever going
# to work. The rate also varies ~4.6x run to run (0.011-0.051 MB/s), which
# rules out ANY fixed per-asset cap being simultaneously safe and useful.
#
# So: bound the LEG, and let each upload have whatever is left of it. A leg
# that overruns fails the completeness gate exactly as before.
#
# The v0.0.90 rule still binds: never kill a slow-but-PROGRESSING upload and
# then retry it — a restart resumes from byte zero (the presigned OBS PUT has
# no multipart/resume; see #301). Here nothing is killed and retried within a
# round: the deadline ends the leg.
: "${MIRROR_LEG_DEADLINE_GH:=600}"     # github is fast; 10min is already absurd
: "${MIRROR_LEG_DEADLINE_GTC:=2400}"   # gitcode: shaped inbound, needs headroom

# Assets are uploaded CONCURRENTLY within a leg. Measured on the same probe:
# the shaping is per-CONNECTION, so concurrency scales almost linearly —
# 1/4/8 concurrent 1MB uploads took 76s/80s/93s wall (0.013/0.050/0.086 MB/s
# aggregate, 6.6x at N=8). Concurrency does raise the error rate (one 502 in
# the N=4 round), which the existing probe-then-reupload rounds absorb.
: "${MIRROR_MAX_PARALLEL:=8}"

DL="$(mktemp -d)"; trap 'rm -rf "$DL"' EXIT

human_size() { # path → e.g. 31.9MB
  local b; b=$(stat -c %s "$1" 2>/dev/null || stat -f %z "$1" 2>/dev/null || echo 0)
  awk -v b="$b" 'BEGIN{ if (b>=1048576) printf "%.1fMB", b/1048576; else if (b>=1024) printf "%.1fKB", b/1024; else printf "%dB", b }'
}

host_label() { [[ "$1" == gh ]] && echo github || echo gitcode; }

# Upload one asset with whatever is left of the leg deadline, timing it.
# 0 = the command returned inside the budget (NOT proof it landed — gtc's exit
# code lies both ways, so the probe / gate remains the only source of truth);
# 1 = the leg deadline fired.
#
# Runs in a SUBSHELL under the parallel launcher, so it must not rely on any
# state surviving the call; everything it produces goes to stdout/stderr, which
# the launcher collects per asset and replays in order.
upload_asset() { # kind(gh|gtc) asset deadline_epoch → 0 returned / 1 out of budget
  local kind="$1" a="$2" deadline="$3" start elapsed rc=0 sz host budget
  sz=$(human_size "$DL/$a")
  host=$(host_label "$kind")
  budget=$(( deadline - SECONDS ))
  if (( budget <= 0 )); then
    info "WARN: $host $a ($sz) not attempted — leg deadline already spent"
    return 1
  fi
  start=$SECONDS
  if [[ "$kind" == gh ]]; then
    GH_TOKEN="${XLINGS_RES_TOKEN:-}" timeout "$budget" \
      gh release upload "$VER" "$DL/$a" -R "$GH_DST" --clobber >/dev/null 2>&1 || rc=$?
  else
    timeout "$budget" gtc release upload "$GTC_DST" "$DL/$a" --tag "$VER" \
      >/dev/null 2>&1 || rc=$?
  fi
  elapsed=$((SECONDS - start))
  if [[ $rc == 124 || $rc == 137 ]]; then
    info "WARN: $host $a ($sz) hit the leg deadline after ${elapsed}s — abandoning (the verify gate below decides the release)"
    return 1
  fi
  info "$host $a ($sz) uploaded in ${elapsed}s"
  return 0
}

info "downloading $SRC_REPO v$VER assets ($PROJ)"
for a in "${ASSETS[@]}"; do
  gh release download "v$VER" -R "$SRC_REPO" -D "$DL" -p "$a" 2>/dev/null || { echo "[mirror] FAIL: missing $a in $SRC_REPO v$VER" >&2; exit 1; }
done

# ── Probes ──────────────────────────────────────────────────────────
# Wait-loop probe: a RANGED GET (first byte). It's a real object read — HEAD
# lies on both hosts (gitcode returns a redirect stub; the 0.0.75/76 github
# phantom assets HEAD'd fine) — but it doesn't download multi-MB assets on
# every poll (the old full-GET probes against gitcode from a US runner were
# minute-scale each and blew the job's 20min budget, v0.0.89 incident).
# The final completeness gate below still does FULL GETs.
probe() { # host_path asset → 0 iff the object serves bytes
  local code
  code=$(curl -fsSL -o /dev/null -w '%{http_code}' -r 0-0 -L "$1" 2>/dev/null)
  [[ "$code" == 200 || "$code" == 206 ]]
}

# Shared-deadline batch verify: ONE ~2min propagation clock sweeps ALL
# pending assets (per-asset serial 120s waits stacked up to 16min in the
# v0.0.90 incident). Echoes the still-missing set; empty output = all good.
verify_batch() { # base_url asset... → prints assets still not serving
  local base="$1"; shift
  local deadline=$((SECONDS + 150))
  local pending=("$@")
  while ((${#pending[@]})) && ((SECONDS < deadline)); do
    local still=()
    for a in "${pending[@]}"; do
      probe "${base}/${a}" || still+=("$a")
    done
    pending=("${still[@]}")
    ((${#pending[@]})) && sleep 5
  done
  ((${#pending[@]})) && echo "${pending[*]}"
  return 0
}

# One host's mirror: probe → SKIP whatever already serves → upload only the
# missing, then batch-verify with propagation patience, up to 3 rounds.
#
# HARD RULE: this script issues NO delete operations, on either host.
# (The gh `--clobber` below is an overwrite-upload that can only fire after
# the download probe already said the asset is NOT serving — it can repair a
# phantom record but can never touch a serving asset, which is skipped
# before any upload is attempted.)
#
# Hard-won rules (0.0.86 / 0.0.89 / 0.0.90 postmortems):
#  - NEVER delete on a verify timeout — the eager 404→delete loop repeatedly
#    deleted GOOD uploads whose propagation was merely slow.
#  - NEVER kill a slow-but-progressing upload AND RETRY IT. v0.0.90 wrapped
#    uploads in `timeout 300`, so every cross-border PUT >5min was SIGKILLed
#    at 60%% and restarted from byte zero — the 20min job ceiling fell to
#    this. Nothing is killed-and-retried inside a round now: an upload gets
#    the remaining LEG budget, and exhausting it ends the leg.
#  - gtc's exit code lies both ways (obs_callback flakiness); the download
#    probe is the only source of truth. Measured mechanism (#301): the
#    presigned PUT carries an `x-obs-callback` header pointing at
#    api.gitcode.com; OBS stores the object and THEN calls back, so a failed
#    callback reports `code:400 ... EOF` for an upload that did land.
#
# Assets within a leg upload CONCURRENTLY (the shaping is per-connection —
# see the deadline block at the top). Each upload runs in its own subshell
# writing to its own log, which is replayed in asset order after the wait, so
# concurrent progress lines don't interleave into unreadable soup.
mirror_host() { # kind(gh|gtc) base_url deadline_seconds
  local kind="$1" base="$2" budget="$3" try a
  local pending failed
  local -A lost=()
  local host_start=$SECONDS
  local deadline=$((SECONDS + budget))
  local host; host=$(host_label "$kind")
  local wdir; wdir=$(mktemp -d)
  info "$host leg budget ${budget}s, up to ${MIRROR_MAX_PARALLEL} concurrent uploads"
  for try in 1 2 3; do
    # Step 1: probe first — anything already serving is mirrored and must
    # never be re-uploaded.
    local todo=()
    for a in "${ASSETS[@]}"; do
      [[ -n "${lost[$a]:-}" ]] && continue
      if probe "${base}/${a}"; then
        [[ $try == 1 ]] && info "$host $a already mirrored, skipping"
        continue
      fi
      todo+=("$a")
    done
    [[ ${#todo[@]} == 0 ]] && break

    # Step 2: upload the missing ones concurrently, bounded by a live count of
    # running children (not a hand-kept counter — that mis-books as soon as one
    # finishes early). `if/else` around upload_asset so the subshell's own
    # errexit can't swallow the rc file.
    local i=0
    for a in "${todo[@]}"; do
      while (( $(jobs -rp | wc -l) >= MIRROR_MAX_PARALLEL )); do sleep 1; done
      ( if upload_asset "$kind" "$a" "$deadline" >"$wdir/$i.log" 2>&1
        then echo 0; else echo 1; fi >"$wdir/$i.rc" ) &
      i=$((i + 1))
    done
    wait

    pending=()
    for i in "${!todo[@]}"; do
      a="${todo[$i]}"
      cat "$wdir/$i.log" 2>/dev/null || true
      if [[ "$(cat "$wdir/$i.rc" 2>/dev/null || echo 1)" == 0 ]]; then
        pending+=("$a")
      else
        lost[$a]=1
      fi
      rm -f "$wdir/$i.log" "$wdir/$i.rc"
    done

    [[ ${#pending[@]} == 0 ]] && break
    failed=$(verify_batch "$base" "${pending[@]}")
    [[ -z "$failed" ]] && break
    info "$host not serving after patience (try $try): $failed — re-uploading (no delete)"
  done
  rm -rf "$wdir"
  ((${#lost[@]})) && info "WARN: $host abandoned ${#lost[@]} asset(s) at the ${budget}s leg deadline: ${!lost[*]}"
  info "$host mirror leg finished in $((SECONDS - host_start))s"
  return 0  # the completeness gate below is the real pass/fail
}

# ── Both hosts IN PARALLEL: they are fully independent, and the gitcode
# leg is cross-border-slow — serializing them doubled wall time for nothing.
GH_ENABLED=0
if [[ -n "${XLINGS_RES_TOKEN:-}" ]] || gh auth status >/dev/null 2>&1; then
  GH_ENABLED=1
  info "GitHub $GH_DST tag $VER"
  GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "$VER" -R "$GH_DST" >/dev/null 2>&1 \
    || GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release create "$VER" -R "$GH_DST" --title "$VER" --notes "$PROJ $VER (mirror of $SRC_REPO)"
else
  info "no github auth; skipping github mirror"
fi
GTC_ENABLED=0
if [[ -n "${GITCODE_TOKEN:-}" ]] && command -v gtc >/dev/null 2>&1; then
  GTC_ENABLED=1
  info "GitCode $GTC_DST tag $VER"
  gtc release create "$GTC_DST" --tag "$VER" --name "$VER" 2>/dev/null || true
else
  info "no GITCODE_TOKEN/gtc; skipping gitcode mirror"
fi

GH_PID=""; GTC_PID=""
if [[ "$GH_ENABLED" == 1 ]]; then
  mirror_host gh  "https://github.com/${GH_DST}/releases/download/${VER}" "$MIRROR_LEG_DEADLINE_GH" &
  GH_PID=$!
fi
if [[ "$GTC_ENABLED" == 1 ]]; then
  mirror_host gtc "https://gitcode.com/${GTC_DST}/releases/download/${VER}" "$MIRROR_LEG_DEADLINE_GTC" &
  GTC_PID=$!
fi
[[ -n "$GH_PID"  ]] && wait "$GH_PID"
[[ -n "$GTC_PID" ]] && wait "$GTC_PID"

# ── Completeness gate: verify every asset on every ENABLED host ──
# A4: this is the mirror's definition of done. The caller must treat a
# non-zero exit as a hard failure (release.yml does since the 0.0.85
# incident — the old `|| echo non-blocking` swallowed exactly this).
info "verify:"
rc=0
hosts=()
[[ "${GH_ENABLED:-0}"  == 1 ]] && hosts+=("github.com/$GH_DST")
[[ "${GTC_ENABLED:-0}" == 1 ]] && hosts+=("gitcode.com/$GTC_DST")
for host in "${hosts[@]}"; do
  for a in "${ASSETS[@]}"; do
    code=$(curl -fsSL -o /dev/null -w '%{http_code}' -L "https://${host}/releases/download/${VER}/${a}" 2>/dev/null || echo ERR)
    echo "  $code  https://${host}/releases/download/${VER}/${a}"
    [[ "$code" == 200 ]] || { rc=1; echo "[mirror] FAIL: missing/unverified: https://${host}/releases/download/${VER}/${a}" >&2; }
  done
done
if [[ $rc != 0 ]]; then
  echo "[mirror] hint: if the asset above was WARNed at a leg deadline, raise MIRROR_LEG_DEADLINE_GH/GTC for this run, or push it by hand:" >&2
  echo "[mirror]       gh release download v$VER -R $SRC_REPO -p '<asset>' && gtc release upload $GTC_DST '<asset>' --tag $VER" >&2
fi
[[ $rc == 0 ]] && info "all assets mirrored + verified on ${#hosts[@]} host(s) in ${SECONDS}s"
exit $rc
