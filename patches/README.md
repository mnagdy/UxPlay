# GStreamer HLS key validation

`gstreamer-1.26.2-hls-key-validation.patch` fixes the adaptive HLS demuxer's handling of failed or malformed AES-128 key downloads. It requires a completed successful response and exactly 16 readable key bytes before caching a key. Other responses take the existing `DECRYPT_NOKEY` error path. It does not provide missing keys or add support for another encryption scheme.

Base source: [gst-plugins-good 1.26.2](https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-1.26.2.tar.xz), SHA-256 `d864b9aec28c3a80895468c909dd303e5f22f92d6e2b1137f80e2a1454584339`. The patch is for that release; it is not automatically applied to another version.

The receiver's source build does **not** install this plugin. Keep the system plugin and the working receiver intact while validating an isolated build. A normal development environment needs Meson, Ninja, GStreamer development packages, libxml2 development headers and OpenSSL development headers. From an extracted source directory, apply the patch and build only this plugin:

```sh
patch -p1 < /path/to/UxPlay/patches/gstreamer-1.26.2-hls-key-validation.patch
meson setup build --prefix=/absolute/path/to/isolated-install \
  --buildtype=release --wrap-mode=nofallback \
  -Dauto_features=disabled -Dadaptivedemux2=enabled -Dhls-crypto=openssl \
  -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dnls=disabled
meson compile -C build
meson install -C build
```

Generate legal synthetic encrypted media on a machine with FFmpeg and OpenSSL, then copy that fixture directory to the Pi if needed:

```sh
python3 tests/run_hls_key_test.py --generate-only /absolute/path/to/key-fixtures
python3 tests/run_hls_key_test.py /absolute/path/to/test_direct_renderer_hls \
  --fixtures /absolute/path/to/key-fixtures \
  --gst-plugin-path /absolute/path/to/isolated-install/lib/aarch64-linux-gnu/gstreamer-1.0
```

Use the actual plugin directory selected by Meson on the build machine. The driver requires `gst-inspect-1.0` and verifies that `hlsdemux2` resolves to that exact private plugin filename before opening media. A plain `GST_PLUGIN_PATH` override can still select the system plugin when both registrations exist. To prevent that, the driver uses a temporary registry and a temporary directory of symlinks to the other system plugins, excluding the system `adaptivedemux2`. It discovers the installed plugin directory with `gst-inspect`, or uses explicitly supplied `GST_PLUGIN_SYSTEM_PATH_1_0` / `GST_PLUGIN_SYSTEM_PATH` directories. These overrides apply only to child processes; installed files and global configuration are unchanged.

The driver disables hardware video decoders and checks six cases: valid key, empty key, HTTP 404 with a 16-byte body, short key, long key and redirect to a valid key. Successful cases must produce real audio and video; invalid keys must fail with `DECRYPT_NOKEY` and no NULL-buffer assertions. Successful playback also checks that its key is reused. Retrying a failed key in the same demux/cache is not covered by this driver.

Before any receiver deployment, verify the loaded plugin filename and version, retest normal and missing-audio HLS and stream replacement, and package the plugin with an explicit rollback path. These tests alone do not establish Channel 4 compatibility or identify its actual encryption method.

## Isolated continuous-display prototype

The [display milestone](../docs/continuous-display.md) describes the separate
Wayland prototype and its acceptance gates. These patches are applied only by
the user-local build recipes, not by the normal receiver build:

- `weston-p030.patch`: backports P030 format metadata to Weston 14.0.2.
- `weston-kiosk-no-seat.patch`: restores the previous kiosk surface when a
  foreground client closes without an input seat.
- `mpv-dmabuf-initial-resize.patch`: unmodified upstream fix
  `43ce03c519f6ca7e61c73616d4aa10ecb4a380d2`, deferring an initial resize until
  video geometry exists. It fixes the reproduced fullscreen startup crash.
- `mpv-dmabuf-drivers.patch`: upstream
  `2bc3bddfb990065b9d7e6270ee07b3063549f8c2`, with context adapted to mpv 0.40,
  restricting the output to its supported hardware interfaces. This patch alone
  did not fix the observed resize crash.
- `mpv-ffmpeg7-rpi-formats.patch`: a local test-reference adjustment for five
  additional formats in the pinned Raspberry Pi FFmpeg package. It preserves
  every upstream reference entry and does not suppress test failures or change
  player code.

`scripts/pi-build-weston-trial` and `scripts/pi-build-mpv-wayland-trial` pin
sources, patches and extracted dependencies. The mpv recipe checks the Pi header
package and reference hashes and runs its normal complete unit suite. Headless
import and playback checks do not establish physical HDMI picture, sound,
smooth 4K output or absence of visible flashes.

## Private Wayland fullscreen fix

`gst-waylandsink-fullscreen.patch` is the unmodified upstream GStreamer fix
[`96b800f82a3084eec8e4e970a7f5ce6743967e45`](https://github.com/GStreamer/gstreamer/commit/96b800f82a3084eec8e4e970a7f5ce6743967e45).
It stores the fullscreen property before the window exists and only sends a
fullscreen request when the window has been created. The constructor already
uses the stored property. The sink needs `fullscreen=true` under the kiosk
compositor; otherwise GStreamer 1.26.2 can overwrite the compositor's configured
dimensions with the source dimensions and reject portrait or oversized video.

`scripts/pi-build-waylandsink-trial` builds only `libgstwaylandsink.so` against
the installed 1.26.2 GStreamer ABI. It pins the matching Raspberry Pi package
sources and development-header archive, applies the package patch series and
upstream fix, and links the existing runtime libraries. This preserves the Pi's
Wayland buffer-stride support. It installs no system packages and starts no
playback or services. Its manifest records source, patch and artifact hashes,
the linked Wayland library and the exact plugin selected by `gst-inspect`.
Selection uses a fresh private registry and a directory of system-plugin
symlinks excluding the duplicate system `waylandsink`.
