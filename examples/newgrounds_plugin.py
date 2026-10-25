# Copyright (c) 2026 re-v0
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""B.LAIR_QT example plugin: Newgrounds Art Portal.

Reference plugin for the "python" source flavor. Newgrounds is not a booru
engine (no JSON API, no booru-style tags), so this scrapes the Art Portal
search page instead of parsing an API.

Requires extra Python packages (plugin_runner.py itself needs none):

    pip install argon2-cffi requests

Newgrounds answers with a "NG Guard" proof-of-work challenge (argon2id)
before serving real content:

    payload_bytes = base64url_decode(challenge["payload"])
    for nonce in 0, 1, 2, ...:
        digest = argon2id(payload_bytes + b":" + nonce, salt=b"\\x00"*8, **params)
        if leading_zero_bits(digest) >= challenge["bits"]:
            submit nonce to /_guard/api/v1/verify

A verified solve lets the same IP through for about 180 seconds.
build_search_request() records when it last solved one (a small file in the
system temp folder) and solves again only after 150 seconds, so quick
successive searches skip the wait.

Time budget: the app gives build_search_request() 60 seconds in total. The
network calls get 10 s each and the proof-of-work 30 s, which stays below it.

Limitations:
- Full-text search (terms=...), not real tag search.
- Rating/score/tags need a request per post, so they are left empty.
- file_url is the large listing thumbnail, not guaranteed original size.
"""
import base64
import os
import re
import tempfile
import time

import requests
from argon2.low_level import Type, hash_secret_raw

BASE = "https://www.newgrounds.com"
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0"

NETWORK_TIMEOUT_S = 10
POW_TIMEOUT_S = 30
# The server grants about 180 s; re-solve a little earlier to be safe.
SOLVE_VALID_S = 150
SOLVE_STAMP_PATH = os.path.join(tempfile.gettempdir(), "blairqt_ng_guard_solved")

_ITEM_RE = re.compile(
    r'<a href="(?P<href>https://www\.newgrounds\.com/art/view/(?P<artist>[^/"]+)/(?P<slug>[^/"?]+))"'
    r'[^>]*>(?P<inner>.*?)</a>',
    re.IGNORECASE | re.DOTALL,
)
_IMG_RE = re.compile(r'<img[^>]*src="(?P<thumb>https://art\.ngfiles\.com/thumbnails/\d+/(?P<id>\d+)_full\.\w+)')
_RATING_RE = re.compile(r'rated-([etma])"')


def _b64url_decode(s: str) -> bytes:
    s = s.replace("-", "+").replace("_", "/")
    s += "=" * (-len(s) % 4)
    return base64.b64decode(s)


def _leading_zero_bits(data: bytes) -> int:
    bits = 0
    for byte in data:
        if byte == 0:
            bits += 8
            continue
        bits += 8 - byte.bit_length()
        break
    return bits


def _solve_ng_guard():
    """Solves one NG Guard proof-of-work challenge, which grants this
    machine's outbound IP a short (~180s) grace window for normal requests -
    no cookie/token needs to be attached to the actual search request
    afterwards, confirmed by testing against the real server."""
    session = requests.Session()
    session.headers.update({"User-Agent": USER_AGENT})

    challenge = session.get(f"{BASE}/_guard/api/v1/challenge", timeout=NETWORK_TIMEOUT_S).json()
    payload_raw = _b64url_decode(challenge["payload"])
    params = challenge["params"]
    bits = challenge["bits"]

    start = time.time()
    nonce = 0
    while True:
        data = payload_raw + b":" + str(nonce).encode()
        digest = hash_secret_raw(
            secret=data,
            salt=b"\x00" * 8,
            time_cost=params["iterations"],
            memory_cost=params["memorySize"],
            parallelism=params["parallelism"],
            hash_len=params["hashLength"],
            type=Type.ID,
        )
        if _leading_zero_bits(digest) >= bits:
            break
        nonce += 1
        if time.time() - start > POW_TIMEOUT_S:
            raise RuntimeError(f"NG Guard proof-of-work timed out after {POW_TIMEOUT_S}s")

    solve_time_ms = int((time.time() - start) * 1000)
    response = session.post(
        f"{BASE}/_guard/api/v1/verify",
        json={
            "algo": challenge["algo"],
            "bits": bits,
            "demo": False,
            "nonce": str(nonce),
            "params": params if challenge["algo"] == "argon2id" else None,
            "payload": challenge["payload"],
            "sig": challenge["sig"],
            "solveTimeMs": solve_time_ms,
        },
        timeout=NETWORK_TIMEOUT_S,
    )
    response.raise_for_status()


def _last_solve_time():
    """Unix time of the last verified solve, or 0 if there is none."""
    try:
        with open(SOLVE_STAMP_PATH, "r") as stamp:
            return float(stamp.read().strip())
    except (OSError, ValueError):
        return 0.0


def _record_solve_time():
    try:
        with open(SOLVE_STAMP_PATH, "w") as stamp:
            stamp.write(str(time.time()))
    except OSError:
        pass  # only an optimisation; the next search simply solves again


def build_search_request(base_url, tags, page, per_page):
    if time.time() - _last_solve_time() > SOLVE_VALID_S:
        _solve_ng_guard()
        _record_solve_time()

    terms = tags.strip() or "art"  # empty query = 400 on Newgrounds' search
    url = f"{base_url}/search/conduct/art?terms={requests.utils.quote(terms)}&page={max(page, 1)}"
    return {"url": url, "headers": {"User-Agent": USER_AGENT}}


def parse_response(body, base_url):
    posts = []
    for item in _ITEM_RE.finditer(body):
        img_match = _IMG_RE.search(item.group("inner"))
        if not img_match:
            continue  # e.g. a non-thumbnail link that happens to match the href pattern

        rating_match = _RATING_RE.search(item.group("inner"))
        posts.append(
            {
                "id": img_match.group("id"),
                "file_url": img_match.group("thumb"),
                "preview_url": img_match.group("thumb"),
                "post_page_url": item.group("href"),
                "tags": [item.group("artist")],
                "rating": rating_match.group(1) if rating_match else "",
                "score": 0,
                "width": 0,
                "height": 0,
            }
        )
    return posts
