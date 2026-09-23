# Maintainer: re-v0
pkgname=blairqt
pkgver=0.3.0
pkgrel=7
pkgdesc="Native Qt6 booru gallery client - browse, download and locally index booru-style image boards"
arch=('x86_64')
url="https://gitlab.com/re-v0/blairqt"
license=('MPL-2.0')
depends=('qt6-base')
optdepends=('python: run Python-scripted source plugins ("python" flavor)'
            'python-flask: HTML-scrape bridge (tools/html_scrape_bridge.py)'
            'python-cloudscraper: HTML-scrape bridge'
            'python-beautifulsoup4: HTML-scrape bridge')
makedepends=('cmake' 'ninja')
source=()
sha256sums=()

build() {
    cmake -S "$startdir" -B "$startdir/build" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build "$startdir/build"
}

package() {
    DESTDIR="$pkgdir" cmake --install "$startdir/build"
    install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
