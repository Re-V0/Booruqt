# Maintainer: re-v0
pkgname=blairqt
pkgver=1.0.0.1
pkgrel=1
pkgdesc="Native Qt6 booru gallery client - browse, download and locally index booru-style image boards"
arch=('x86_64')
url="https://gitlab.com/re-v0/blairqt"
license=('MPL-2.0')
depends=('qt6-base' 'qt6-multimedia')
optdepends=('python: run Python-scripted source plugins ("python" flavor)'
            'python-requests: Newgrounds example plugin'
            'python-argon2-cffi: Newgrounds example plugin'
            'python-flask: HTML-scrape bridge (tools/html_scrape_bridge.py)'
            'python-cloudscraper: HTML-scrape bridge'
            'python-beautifulsoup4: HTML-scrape bridge')
makedepends=('cmake' 'ninja' 'qt6-tools' 'git')
# The plugin tests run plugin_runner.py.
checkdepends=('python')
# A git tag, not the release tarball: the tarball contains this PKGBUILD, so
# its checksum could never be written into the PKGBUILD of the same tag.
source=("$pkgname::git+$url.git#tag=v$pkgver")
sha256sums=('SKIP')

build() {
    cmake -S "$pkgname" -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

check() {
    # The suite needs no network and no visible window.
    QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
}

package() {
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 "$pkgname/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
