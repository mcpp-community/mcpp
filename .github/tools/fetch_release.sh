#!/usr/bin/env bash
# fetch_release.sh <url> <dest> — download a release archive, and mean it.
#
# ONE implementation for every bootstrap point in the repo, same reason
# install_pinned_mcpp.sh is: the two legs of bootstrap-mcpp had a bare
# `curl -fsSL` each, and a fix applied to one of them is a fix half the CI does
# not get.
#
# WHAT KEPT BREAKING. The Windows legs fail regularly with
#
#     curl: (52) Empty reply from server
#     Error: Process completed with exit code 52
#
# — the GitHub release CDN accepting the connection and then closing it with no
# response. It is transient and it is not rare: it accounted for essentially
# every unexplained red on this branch, always inside 12 seconds, always with no
# test name in the log.
#
# `curl --retry` ALONE DOES NOT COVER IT. `--retry` handles timeouts and a
# specific list of 5xx responses; an empty reply is a *transport* error and is
# not on that list. `--retry-all-errors` (curl 7.71+) is the flag that does, and
# it is the one that was missing. The outer loop below is not redundant with it:
# it also re-runs when the bytes arrive but do not form a readable archive, which
# curl considers a complete success.
#
# WHY THE ARCHIVE IS OPENED HERE. A truncated download is not detected by curl —
# `-f` only checks the HTTP status. Without this check the failure surfaces later
# as `tar: unexpected EOF` or `unzip: cannot find zipfile directory`, several
# steps away from the download that actually failed, and reads like a corrupt
# release rather than a flaky fetch.
set -euo pipefail

url="${1:?usage: fetch_release.sh <url> <dest>}"
dest="${2:?usage: fetch_release.sh <url> <dest>}"

attempts="${FETCH_ATTEMPTS:-5}"

verify() {
    case "$dest" in
        *.tar.gz|*.tgz) tar -tzf "$dest"  >/dev/null 2>&1 ;;
        *.zip)          unzip -tqq "$dest" >/dev/null 2>&1 ;;
        # Nothing to open: fall back to "it is not empty", which still catches
        # the zero-byte result an interrupted transfer leaves behind.
        *)              [ -s "$dest" ] ;;
    esac
}

for i in $(seq 1 "$attempts"); do
    rm -f "$dest"
    # --retry-all-errors is what covers exit 52; the rest bound how long a single
    # attempt may hang. --max-time is generous because these archives are tens of
    # megabytes on a shared runner.
    if curl -fsSL \
            --retry 3 --retry-delay 2 --retry-all-errors \
            --connect-timeout 20 --max-time 600 \
            -o "$dest" "$url"; then
        if verify; then
            [ "$i" -eq 1 ] || echo "fetch_release: succeeded on attempt $i" >&2
            exit 0
        fi
        echo "fetch_release: attempt $i downloaded $(wc -c < "$dest" 2>/dev/null || echo 0)" \
             "bytes but the archive does not open" >&2
    else
        echo "fetch_release: attempt $i failed to download" >&2
    fi
    # Back off before retrying: an immediate retry against a CDN that just
    # dropped the connection tends to be dropped again.
    [ "$i" -lt "$attempts" ] && sleep $(( i * 5 ))
done

echo "fetch_release: giving up after $attempts attempts" >&2
echo "  url : $url" >&2
echo "  dest: $dest" >&2
exit 1
