#!/usr/bin/env python3
"""probe_gtc_upload.py — instrumented single-asset upload to a GitCode release.

TEMPORARY diagnostic tool for the recurring `publish-ecosystem` failure where
the GitCode leg of tools/mirror_res.sh blows its 180s per-asset cap on the two
largest tarballs (0.0.94 / 0.0.97 / 0.0.105 / 2026.7.28.2). It is a measurement
harness, not a mirror: it reproduces exactly what tools/gtc `release upload`
does, but times every stage separately and reports one JSON line per upload so
the stages can be compared across transports and concurrency levels.

The upload path under test (identical to tools/gtc `_upload_one`):

  1. GET  {api}/repos/{repo}/releases/{tag}/upload_url?file_name=X
         -> {"url": <presigned OBS PUT URL>, "headers": {...}}
  2. PUT  <presigned url>  with the whole file as the body
  3. (server-side) OBS calls back into the GitCode API to register the asset

Stage 3 is invisible to the client except when it fails, which is where the
`obs_callback ... code:400 ... EOF` false negative comes from: the object is
in OBS but the registration callback errored, so the PUT reports failure for
an upload that actually landed. This tool always re-probes the public download
URL afterwards, so `put_ok` and `serves` are reported independently and that
false negative shows up as put_ok=false + serves=true.

Usage:
  probe_gtc_upload.py --repo xlings-res/mcpp --tag probe-x --file f.bin \
                      [--method urllib|curl] [--label NAME]

Auth: GITCODE_TOKEN env var (same as tools/gtc).
Output: one JSON object per line on stdout; human notes on stderr.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API_BASE = os.environ.get("GITCODE_API_BASE", "https://api.gitcode.com/api/v5")
DL_HOST = "https://gitcode.com"


def get_upload_url(repo, tag, fname, token):
    """Stage 1 — ask GitCode for a presigned OBS PUT URL."""
    url = (f"{API_BASE}/repos/{repo}/releases/{tag}/upload_url"
           f"?file_name={urllib.parse.quote(fname)}")
    req = urllib.request.Request(
        url, headers={"PRIVATE-TOKEN": token, "Accept": "application/json"})
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=120) as r:
        body = r.read()
    dt = time.monotonic() - t0
    info = json.loads(body)
    return info["url"], info.get("headers") or {}, dt


def put_urllib(put_url, headers, path):
    """Stage 2, transport A — exactly what tools/gtc does today: read the
    whole file into memory and hand it to urllib as a single PUT body."""
    with open(path, "rb") as f:
        data = f.read()
    req = urllib.request.Request(put_url, data=data, headers=headers, method="PUT")
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=1800) as r:
            status = r.status
            resp = r.read().decode(errors="replace")[:300]
    except urllib.error.HTTPError as e:
        status = e.code
        resp = e.read().decode(errors="replace")[:300]
    except Exception as e:                       # noqa: BLE001 - report, don't raise
        status = -1
        resp = f"{type(e).__name__}: {e}"[:300]
    return status, resp, time.monotonic() - t0, {}


def put_curl(put_url, headers, path):
    """Stage 2, transport B — same presigned URL, but curl streams the file
    from disk instead of buffering it in the process. curl also reports its own
    connect/TLS/first-byte breakdown, which is what separates "the network is
    slow" from "the client is slow"."""
    fmt = ("%{http_code} %{speed_upload} %{time_namelookup} %{time_connect} "
           "%{time_appconnect} %{time_pretransfer} %{time_starttransfer} %{time_total}")
    cmd = ["curl", "-sS", "-X", "PUT", "-T", path, "--max-time", "1800", "-o", "/tmp/put_body",
           "-w", fmt]
    for k, v in headers.items():
        cmd += ["-H", f"{k}: {v}"]
    cmd.append(put_url)
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.monotonic() - t0
    detail, status, resp = {}, -1, (p.stderr or "")[:300]
    if p.returncode == 0 and p.stdout.strip():
        parts = p.stdout.split()
        try:
            status = int(parts[0])
            detail = {
                "curl_speed_upload_Bps": float(parts[1]),
                "curl_t_namelookup": float(parts[2]),
                "curl_t_connect": float(parts[3]),
                "curl_t_appconnect": float(parts[4]),
                "curl_t_pretransfer": float(parts[5]),
                "curl_t_starttransfer": float(parts[6]),
                "curl_t_total": float(parts[7]),
            }
        except (IndexError, ValueError):
            pass
        try:
            resp = Path("/tmp/put_body").read_text(errors="replace")[:300]
        except OSError:
            resp = ""
    else:
        detail["curl_exit"] = p.returncode
    return status, resp, dt, detail


def serves(repo, tag, fname):
    """Independent truth: does the public download URL actually return bytes?
    Ranged GET — HEAD lies on GitCode (returns a redirect stub)."""
    url = f"{DL_HOST}/{repo}/releases/download/{tag}/{urllib.parse.quote(fname)}"
    p = subprocess.run(
        ["curl", "-sSL", "-o", "/dev/null", "-w", "%{http_code}", "-r", "0-0", url],
        capture_output=True, text=True)
    return p.stdout.strip(), url


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--method", choices=("urllib", "curl"), default="urllib")
    ap.add_argument("--label", default="")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    token = os.environ.get("GITCODE_TOKEN", "")
    if not token:
        sys.exit("GITCODE_TOKEN is empty")

    path = Path(args.file)
    size = path.stat().st_size
    fname = path.name

    put_url, headers, t_url = get_upload_url(args.repo, args.tag, fname, token)
    obs_host = urllib.parse.urlparse(put_url).netloc

    put = put_urllib if args.method == "urllib" else put_curl
    status, resp, t_put, detail = put(put_url, headers, str(path))

    rec = {
        "label": args.label or fname,
        "method": args.method,
        "file": fname,
        "size_bytes": size,
        "size_mb": round(size / 1048576, 2),
        "obs_host": obs_host,
        "t_upload_url_s": round(t_url, 3),
        "t_put_s": round(t_put, 2),
        "put_status": status,
        "put_ok": status == 200,
        "throughput_MBps": round(size / 1048576 / t_put, 3) if t_put > 0 else None,
        "put_response_head": resp.replace("\n", " ")[:200],
    }
    rec.update(detail)

    if not args.no_verify:
        # Deliberately independent of put_ok: this is the check that exposes
        # the obs_callback false negative (put_ok=false while serves=200/206).
        code, url = serves(args.repo, args.tag, fname)
        rec["verify_code"] = code
        rec["serves"] = code in ("200", "206")
        rec["download_url"] = url

    print(json.dumps(rec), flush=True)
    sys.stderr.write(
        f"[probe] {rec['label']}: {rec['size_mb']}MB via {args.method} "
        f"-> put={rec['put_status']} in {rec['t_put_s']}s "
        f"({rec['throughput_MBps']} MB/s) serves={rec.get('serves')}\n")


if __name__ == "__main__":
    main()
