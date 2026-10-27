# Copyright 2026 re-v0
# Distributed under the terms of the Mozilla Public License v2.0

EAPI=8

inherit cmake git-r3 optfeature xdg

DESCRIPTION="Native Qt6 booru gallery client with a local, tag-indexed library"
HOMEPAGE="https://gitlab.com/re-v0/blairqt"
EGIT_REPO_URI="https://gitlab.com/re-v0/blairqt.git"

LICENSE="MPL-2.0"
SLOT="0"
# Live ebuild: never keyworded.
KEYWORDS=""
IUSE="+python test"

RESTRICT="!test? ( test )"

# Sql without sqlite would build, then fail to open the library at runtime.
RDEPEND="
	dev-qt/qtbase:6[concurrent,gui,network,sql,sqlite,widgets]
	dev-qt/qtmultimedia:6
	python? ( dev-lang/python:* )
"
DEPEND="${RDEPEND}"
# lrelease/lupdate only; nothing from qttools is linked in. The plugin
# tests run plugin_runner.py, so USE=test needs python even with -python.
BDEPEND="
	dev-qt/qttools:6[linguist]
	test? ( dev-lang/python:* )
"

src_configure() {
	local mycmakeargs=(
		# cmake.eclass does not set this; without it the test binary is built
		# even for USE=-test.
		-DBUILD_TESTING=$(usex test)
	)

	cmake_src_configure
}

src_test() {
	# No X, no Wayland, no window: the suite is pure logic.
	local -x QT_QPA_PLATFORM=offscreen
	cmake_src_test
}

pkg_postinst() {
	xdg_pkg_postinst

	optfeature "the HTML-scrape bridge for booru sites without an API" \
		"dev-python/flask dev-python/beautifulsoup4"

	elog "The scrape bridge also wants cloudscraper, which is not in ::gentoo."
	elog "Install it into a virtualenv if you need Cloudflare-gated sites."
}
