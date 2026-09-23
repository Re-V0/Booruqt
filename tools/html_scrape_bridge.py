#!/usr/bin/env python3
# Copyright (c) 2026 re-v0
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Local HTML-scrape bridge for Gelbooru-engine sites without a dapi/JSON API.

B.LAIR_QT talks to this over HTTP when a source has "This site has no API
(dapi)" checked. It walks the site's HTML post listing with cloudscraper and
resolves each sampled post's full image URL from its detail page.

Requirements:
    pip install flask cloudscraper beautifulsoup4

Run it before using such a source:
    python3 html_scrape_bridge.py

Listens on http://127.0.0.1:8765. The app does not start it for you.
Tags, rating, score and size are not scraped and come back empty.
"""
import html
import logging
import random
import re
import time
from urllib.parse import quote

import cloudscraper
from bs4 import BeautifulSoup
from flask import Flask, jsonify, request

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

PAGE_STEP = 20  # posts per listing page on these engines
MAX_PAGES_PER_REQUEST = 6
RESOLVE_CAP = 15  # more detail-page fetches per request triggers 429s
RESOLVE_DELAY_S = 0.4


def _scraper():
    return cloudscraper.create_scraper(browser={"browser": "chrome", "platform": "windows", "mobile": False})


def _extract_full_image_from_post_page(page_html: str):
    soup = BeautifulSoup(page_html, "html.parser")

    image_el = soup.find(id="image")
    if image_el and image_el.get("src"):
        return image_el["src"]

    image_exts = (".jpg", ".jpeg", ".png", ".gif", ".webm", ".webp")
    for a in soup.find_all("a", href=True):
        href = a["href"]
        if a.get_text(strip=True).startswith("Original") and href.lower().split("?")[0].endswith(image_exts):
            return href
    return None


def _absolutize(url: str, base_url: str) -> str:
    if url.startswith("//"):
        return "https:" + url
    if url.startswith("/"):
        return base_url + url
    return url


def scrape_gelbooru_html(base_url: str, tags: str, page: int, limit: int):
    scraper = _scraper()
    # Empty tags return nothing on these engines; "all" means everything.
    tag_string = quote(tags.strip()) if tags.strip() else "all"
    post_ids = []
    seen_ids = set()

    # Each app page maps to a different block of listing pages.
    start_block = max(0, page - 1) * MAX_PAGES_PER_REQUEST
    for offset in range(MAX_PAGES_PER_REQUEST):
        pid = (start_block + offset) * PAGE_STEP
        url = f"{base_url}/index.php?page=post&s=list&tags={tag_string}&pid={pid}"
        try:
            resp = scraper.get(url, timeout=20)
        except Exception as e:
            logger.warning("listing request failed for %s: %s", url, e)
            break
        if resp.status_code != 200:
            logger.warning("listing HTTP %s for %s", resp.status_code, url)
            break

        # Links are HTML-escaped ("&amp;") in the raw markup.
        content = html.unescape(resp.text)
        new_ids = [i for i in re.findall(r"s=view&id=(\d+)", content) if i not in seen_ids]
        if not new_ids:
            break
        for i in new_ids:
            seen_ids.add(i)
            post_ids.append(i)
        if len(post_ids) >= limit:
            break

    if not post_ids:
        return []

    resolve_cap = min(len(post_ids), RESOLVE_CAP, limit)
    sample_ids = random.sample(post_ids, resolve_cap) if len(post_ids) > resolve_cap else post_ids

    results = []
    for i, post_id in enumerate(sample_ids):
        if i > 0:
            time.sleep(RESOLVE_DELAY_S)
        try:
            resp = scraper.get(f"{base_url}/index.php?page=post&s=view&id={post_id}", timeout=15)
            if resp.status_code == 429:
                logger.warning("rate-limited (429), stopping resolves for this request")
                break
            if resp.status_code != 200:
                continue
            image_url = _extract_full_image_from_post_page(resp.text)
            if not image_url:
                continue
            image_url = _absolutize(image_url, base_url)
            results.append({
                "id": post_id,
                "file_url": image_url,
                "preview_url": image_url,
                "tags": "",
                "rating": "",
                "score": 0,
                "width": 0,
                "height": 0,
            })
        except Exception as e:
            logger.debug("failed to resolve post %s: %s", post_id, e)

    return results


@app.route("/scrape_gelbooru_html")
def route_scrape_gelbooru_html():
    base_url = request.args.get("base_url", "").rstrip("/")
    if not base_url:
        return jsonify({"error": "missing base_url"}), 400
    try:
        page = int(request.args.get("page", 1))
        limit = int(request.args.get("limit", 40))
    except ValueError:
        return jsonify({"error": "page and limit must be integers"}), 400

    try:
        results = scrape_gelbooru_html(base_url, request.args.get("tags", ""), page, limit)
    except Exception as e:
        logger.exception("scrape failed")
        return jsonify({"error": str(e)}), 502
    return jsonify(results)


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8765)
