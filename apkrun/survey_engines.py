#!/usr/bin/env python3
"""Classify every Recompa port's APK by engine, reading only the zip's central directory over HTTP ranges
(a few KB per APK, no full download). Output: survey.json + a summary on stdout.
Usage: python3 -I survey_engines.py index.json survey.json"""
import json, struct, sys, urllib.request
from concurrent.futures import ThreadPoolExecutor

UA = {"User-Agent": "onn-apk-survey/1"}

def rng(url, start, end):
    r = urllib.request.Request(url, headers={**UA, "Range": f"bytes={start}-{end}"})
    with urllib.request.urlopen(r, timeout=30) as f:
        return f.read(), f.headers.get("Content-Range", "")

def names(url, size):
    tail = min(size, 70000)
    data, _ = rng(url, size - tail, size - 1)
    i = data.rfind(b"PK\x05\x06")
    if i < 0:
        raise ValueError("no EOCD")
    cd_size, cd_off = struct.unpack_from("<II", data, i + 12)
    if cd_off == 0xFFFFFFFF:
        raise ValueError("zip64")
    cd, _ = rng(url, cd_off, cd_off + cd_size - 1)
    out, p = [], 0
    while p + 46 <= len(cd) and cd[p:p + 4] == b"PK\x01\x02":
        n, m, k = struct.unpack_from("<HHH", cd, p + 28)
        out.append(cd[p + 46:p + 46 + n].decode("utf8", "replace"))
        p += 46 + n + m + k
    return out

def classify(files):
    libs = {f.split("/")[-1] for f in files if f.startswith("lib/arm64-v8a/")}
    abis = {f.split("/")[1] for f in files if f.startswith("lib/") and f.count("/") >= 2}
    if not abis:
        eng = "no-native"            # Java/Kotlin, web view, or downloads its code later
    elif "libyoyo.so" in libs:
        eng = "gamemaker"
    elif libs & {"libunity.so", "libil2cpp.so"}:
        eng = "unity"
    elif any(l.startswith("libgodot") for l in libs):
        eng = "godot"
    elif "libgdx.so" in libs:
        eng = "libgdx"
    elif "libSDL3.so" in libs:
        eng = "sdl3"
    elif "libSDL2.so" in libs or "libSDL2-2.0.so" in libs:
        eng = "sdl2"
    elif "libmain.so" in libs:
        eng = "native-other"         # NativeActivity / custom glue without SDL
    elif libs:
        eng = "native-other"
    else:
        eng = "no-arm64"
    return eng, sorted(abis), sorted(libs)

def one(port):
    f = (port.get("forks") or [{}])[0]; a = ((f.get("release") or {}).get("asset")) or {}
    rec = {"name": port["name"], "type": port.get("type"), "url": a.get("url", ""), "size": a.get("size", 0)}
    if not a.get("url"):
        rec["engine"] = "no-release"; return rec
    if not a["url"].lower().endswith(".apk"):
        rec["engine"] = "not-apk"; return rec
    try:
        files = names(a["url"], a["size"])
        rec["engine"], rec["abis"], rec["libs"] = classify(files)
        rec["has_v7a"] = "armeabi-v7a" in rec["abis"]
    except Exception as e:
        rec["engine"] = "error"; rec["error"] = str(e)[:120]
    return rec

def main():
    idx = json.load(open(sys.argv[1], encoding="utf8"))
    ports = [p for p in idx["ports"] if p.get("type") in ("decomp", "recomp", "source-port")]
    with ThreadPoolExecutor(8) as ex:
        res = list(ex.map(one, ports))
    json.dump(res, open(sys.argv[2], "w", encoding="utf8"), indent=1)
    from collections import Counter
    c = Counter(r["engine"] for r in res)
    print(f"{len(res)} ports surveyed")
    for k, v in c.most_common():
        print(f"  {k:13} {v}")
    print(f"  (32-bit build included, installs natively: {sum(1 for r in res if r.get('has_v7a'))})")

if __name__ == "__main__":
    main()
