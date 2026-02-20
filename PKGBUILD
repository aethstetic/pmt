pkgname=pmt
pkgver=0.1.0
pkgrel=1
pkgdesc='Terminal UI package manager for Arch Linux'
arch=('x86_64')
license=('MIT')
depends=('pacman' 'openssl')
makedepends=('gcc')
provides=('pmt')
conflicts=('pmt')

# compiles the binary from source
build() {
    cd "$startdir"
    make clean
    make
}

# installs the binary into the pacman package root
package() {
    cd "$startdir"
    make DESTDIR="$pkgdir" install
}
