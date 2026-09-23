# Maintainer: re-v0
pkgname=blairqt
pkgver=0.3.0
pkgrel=1
pkgdesc="Native Qt6 booru gallery client - browse, download and locally index booru-style image boards"
arch=('x86_64')
url="https://gitlab.com/re-v0/blairqt"
license=('MPL-2.0')
depends=('qt6-base')
optdepends=('python: run Python-scripted source plugins ("python" flavor)'
            'python-flask: HTML-scrape bridge (tools/html_scrape_bridge.py)'
            'python-cloudscraper: HTML-scrape bridge'
            'python-beautifulsoup4: HTML-scrape bridge')
makedepends=('cmake' 'ninja' 'qt6-tools')
source=("$pkgname-$pkgver.tar.bz2::$url/-/archive/v$pkgver/$pkgname-v$pkgver.tar.bz2")
sha256sums=('af3d4853caa62616f7142d7a45593316fea29b9e993b385f70a73b903f4ba607')

build() {
    cmake -S "$pkgname-v$pkgver" -B build \
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
    install -Dm644 "$pkgname-v$pkgver/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
