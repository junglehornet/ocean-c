#!/usr/bin/env bash
# The distro libcurl.a enables SSH/GSSAPI backends whose static dependencies
# are not all shipped by Ubuntu. Build a private HTTP/TLS dependency closure
# for the standalone installer; normal Ocean programs/bundles use system curl.
set -euo pipefail
prefix="${1:?usage: build-static-curl.sh PREFIX}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
version=8.23.0
curl -fsSL --retry 3 "https://curl.se/download/curl-$version.tar.xz" -o "$work/curl.tar.xz"
echo '8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5  '"$work/curl.tar.xz" | sha256sum --check
tar -xf "$work/curl.tar.xz" -C "$work"
cmake -S "$work/curl-$version" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_C_COMPILER=clang \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DCURL_USE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=ON \
  -DENABLE_THREADED_RESOLVER=ON \
  -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
  -DCURL_USE_GSSAPI=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_LIBSSH2=OFF \
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF \
  -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF \
  -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
cmake --build "$work/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
cmake --install "$work/build"

# Exercise the exact static dependency closure before the expensive Ocean build.
cat > "$work/probe.c" <<'C'
#include <curl/curl.h>
int main(void) {
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    return !v->ssl_version || !(v->features & CURL_VERSION_SSL) ||
           !(v->features & CURL_VERSION_ASYNCHDNS);
}
C
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
read -r -a flags <<< "$(pkg-config --static --libs libcurl)"
clang -static -I"$prefix/include" "$work/probe.c" "${flags[@]}" -o "$work/probe"
"$work/probe"
