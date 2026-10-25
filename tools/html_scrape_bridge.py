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
returns the posts in listing order, each with the listing's thumbnail. The
full image URL needs one detail-page request per post, which these sites
rate-limit, so only the first RESOLVE_CAP posts of a page get one; the rest
show a thumbnail but cannot be downloaded from that page.

Requirements:
    pip install flask cloudscraper beautifulsoup4

Run it before using such a source:
    python3 html_scrape_bridge.py

Listens on http://127.0.0.1:8765. The app does not start it for you.
Tags, rating, score and size are not scraped and come back empty.
"""
import html
import logging
import re
import time
from urllib.parse import quote

import cloudscraper
from bs4 import BeautifulSoup
from flask import Flask, jsonify, request

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

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


def _parse_listing(page_html: str, base_url: str):
    """(post_id, thumbnail_url) for every post on a listing page, in order."""
    soup = BeautifulSoup(page_html, "html.parser")
    items = []
    for link in soup.find_all("a", href=True):
        match = re.search(r"s=view&id=(\d+)", html.unescape(link["href"]))
        if match is None:
            continue
        image = link.find("img")
        if image is None or not image.get("src"):
            continue  # a text link to a post, not a thumbnail
        items.append((match.group(1), _absolutize(image["src"], base_url)))
    return items


def scrape_gelbooru_html(base_url: str, tags: str, page: int, limit: int):
    scraper = _scraper()
    # Empty tags return nothing on these engines; "all" means everything.
    tag_string = quote(tags.strip()) if tags.strip() else "all"

    # App page N shows listing posts (N-1)*limit ... N*limit-1. pid is the
    # listing's offset in posts, so consecutive app pages never overlap.
    pid = max(0, page - 1) * limit
    posts = []
    seen_ids = set()
    for _ in range(MAX_PAGES_PER_REQUEST):
        if len(posts) >= limit:
            break
        url = f"{base_url}/index.php?page=post&s=list&tags={tag_string}&pid={pid}"
        try:
            resp = scraper.get(url, timeout=20)
        except Exception as e:
            logger.warning("listing request failed for %s: %s", url, e)
            break
        if resp.status_code != 200:
            logger.warning("listing HTTP %s for %s", resp.status_code, url)
            break

        items = _parse_listing(resp.text, base_url)
        added = 0
        for post_id, thumbnail_url in items:
            if post_id in seen_ids:
                continue
            seen_ids.add(post_id)
            posts.append((post_id, thumbnail_url))
            added += 1
        if added == 0:
            break  # past the last page
        pid += len(items)

    posts = posts[:limit]

    results = []
    resolved = 0
    rate_limited = False
    for post_id, thumbnail_url in posts:
        file_url = ""
        if resolved < RESOLVE_CAP and not rate_limited:
            if resolved > 0:
                time.sleep(RESOLVE_DELAY_S)
            resolved += 1
            try:
                resp = scraper.get(f"{base_url}/index.php?page=post&s=view&id={post_id}", timeout=15)
                if resp.status_code == 429:
                    logger.warning("rate-limited (429), not resolving more posts in this request")
                    rate_limited = True
                elif resp.status_code == 200:
                    image_url = _extract_full_image_from_post_page(resp.text)
                    if image_url:
                        file_url = _absolutize(image_url, base_url)
            except Exception as e:
                logger.debug("failed to resolve post %s: %s", post_id, e)

        results.append({
            "id": post_id,
            "file_url": file_url,
            "preview_url": thumbnail_url,
            "tags": "",
            "rating": "",
            "score": 0,
            "width": 0,
            "height": 0,
        })

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
