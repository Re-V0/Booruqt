#!/usr/bin/env python3
# Copyright (c) 2026 re-v0
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""B.LAIR_QT Python plugin runner.

Do not edit - this is a small, fixed bridge between the app's C++ side and a
user- or third-party-authored plugin script. It loads the plugin named on
the command line and calls one of its two required functions, passing
parameters as JSON on stdin and printing the result as JSON on stdout.

A plugin is a single .py file implementing:

    def build_search_request(base_url, tags, page, per_page):
        '''Return {"url": "...", "headers": {...}}. "headers" is optional.'''
        ...

    def parse_response(body, base_url):
        '''body is the raw response text (already UTF-8 decoded). Return a
        list of post dicts, each with at least "id" and "file_url" -
        "preview_url", "sample_url", "post_page_url", "tags" (a list of
        strings), "rating", "score", "width", "height" are all optional.'''
        ...

Optionally a plugin may also implement:

    def supports_tag_exclusion():
        '''Return True if the site understands "-tag" in a search. The tag
        blacklist is then applied in the query itself instead of only
        filtering the returned posts. Defaults to False when absent.'''
        ...

Any exception raised by the plugin is printed to stderr (surfaced to the
user in the app's network log) and this process exits non-zero.
"""
import sys
import json
import base64
import importlib.util
import traceback


def load_plugin(path):
    spec = importlib.util.spec_from_file_location("blairqt_plugin", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    if len(sys.argv) != 3:
        print("usage: plugin_runner.py <script.py> <build_request|parse_response|capabilities>",
              file=sys.stderr)
        sys.exit(1)

    script_path, mode = sys.argv[1], sys.argv[2]
    params = json.load(sys.stdin)
    plugin = load_plugin(script_path)

    if mode == "build_request":
        result = plugin.build_search_request(
            params["base_url"], params["tags"], params["page"], params["per_page"]
        )
    elif mode == "parse_response":
        raw = base64.b64decode(params["body_base64"])
        body = raw.decode("utf-8", errors="replace")
        result = plugin.parse_response(body, params["base_url"])
    elif mode == "capabilities":
        supports = getattr(plugin, "supports_tag_exclusion", None)
        result = {"supports_tag_exclusion": bool(supports()) if callable(supports) else False}
    else:
        raise ValueError(f"unknown mode: {mode}")

    print(json.dumps(result))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.exit(1)
