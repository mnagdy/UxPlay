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
