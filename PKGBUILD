# Maintainer: Andy Stewart <lazycat.manatee@gmail.com>
pkgname=omarchy-screenshot
pkgver=0.1.12
pkgrel=1
_rapidocrlite_ver=5.0.0a16
pkgdesc="Qt 6 screenshot and annotation tool for Omarchy and Hyprland"
arch=('x86_64' 'aarch64')
url="https://github.com/manateelazycat/omarchy-screenshot"
license=('GPL-3.0-only' 'MIT' 'Apache-2.0' 'BSD-3-Clause')
depends=('qt6-base' 'qt6-declarative' 'qt6-wayland' 'layer-shell-qt' 'grim' 'wl-clipboard' 'hyprland' 'wayland')
makedepends=('cmake' 'pkgconf' 'qt6-tools' 'python' 'wayland-protocols')
source=("$pkgname-$pkgver.tar.gz::$url/archive/v$pkgver.tar.gz"
        "rapidocrlite-$_rapidocrlite_ver.tar.gz::https://github.com/RapidAI/RapidOCRLite/archive/v$_rapidocrlite_ver.tar.gz"
        "PP-OCRv6_det_tiny.onnx::https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/det/PP-OCRv6_det_tiny.onnx"
        "PP-OCRv6_rec_tiny.onnx::https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_tiny.onnx"
        "ppocrv6_tiny_dict.txt::https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/paddle/PP-OCRv6/rec/PP-OCRv6_rec_tiny/ppocrv6_tiny_dict.txt")
sha256sums=('SKIP'
            '56361c81bab92d8c294b7817750793271d2078542d2a2015e8388029001bd83c'
            'f42c0fbd294d95eac1a550e131b277dac97462c8025fa4b6c3cec1b7894bd3d5'
            'e16e242de5937ad92609223f19bc2aff3727ee40b095f996907c24749bad251b'
            'c5cbe34ef40c29c4df07ed012bf96569cb69a2d2a01a07027e9f13cb832bd9cd')

build() {
    cmake -S "$pkgname-$pkgver" -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DFETCHCONTENT_SOURCE_DIR_RAPIDOCRLITE="$srcdir/RapidOCRLite-$_rapidocrlite_ver" \
        -DOMARCHY_SCREENSHOT_OCR_MODEL_DIR="$srcdir"
    cmake --build build --parallel
}

check() {
    ctest --test-dir build --output-on-failure --no-tests=error
}

package() {
    DESTDIR="$pkgdir" cmake --install build --component omarchy-screenshot
    install -Dm644 "$pkgname-$pkgver/LICENSE" \
        "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
