#!/bin/sh
# Copyright (c) 2026 re-v0
# SPDX-License-Identifier: MPL-2.0
#
# Builds a FreeBSD binary package (.pkg) from an installed staging tree.
# Runs on FreeBSD, after:
#   cmake --install build --prefix /usr/local   (with DESTDIR=<stage dir>)
#
# Usage: make-pkg.sh <version> <stage dir> <output dir>
#
# The manifest is written here instead of using CPack's FREEBSD generator:
# CPack derives a dependency's package name from its port origin, which is
# wrong for flavored ports (the SQLite driver is the "sqlite" flavor of
# databases/qt6-base_sqldriver, packaged as qt6-base_sqldriver-sqlite).

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <version> <stage dir> <output dir>" >&2
    exit 1
fi

version="$1"
stage="$2"
out="$3"

# Dependencies at the versions this package was built against.
qt_version=$(pkg query '%v' qt6-base)
multimedia_version=$(pkg query '%v' qt6-multimedia)
sqlite_driver_version=$(pkg query '%v' qt6-base_sqldriver-sqlite)

manifest=$(mktemp)
plist=$(mktemp)

cat > "$manifest" <<EOF
name: blairqt
version: "$version"
origin: graphics/blairqt
comment: "Native Qt6 booru gallery client with a local, tag-indexed library"
desc: "Browse, search and download booru image boards. Downloads go into a local library that is searchable by tag."
maintainer: "bird_of_hermes77@disroot.org"
www: "https://gitlab.com/re-v0/blairqt"
prefix: "/usr/local"
licenselogic: single
licenses: [MPL20]
categories: [graphics]
deps: {
  qt6-base: { origin: "devel/qt6-base", version: "$qt_version" },
  qt6-multimedia: { origin: "multimedia/qt6-multimedia", version: "$multimedia_version" },
  qt6-base_sqldriver-sqlite: { origin: "databases/qt6-base_sqldriver", version: "$sqlite_driver_version" }
}
EOF

# Every installed file, as an absolute path inside the package.
(cd "$stage" && find . -type f -o -type l) | sed 's|^\./|/|' | sort > "$plist"

mkdir -p "$out"
pkg create -M "$manifest" -p "$plist" -r "$stage" -o "$out"

rm -f "$manifest" "$plist"
