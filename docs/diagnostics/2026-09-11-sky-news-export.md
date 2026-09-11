# Sky News: audio absent from the iPhone export

## Finding

At 14:11 BST on 11 September 2026, three complete media fragments downloaded directly from UHF's HTTP server on the iPhone contained **750 HEVC video samples and zero audio samples across 30 seconds**. The initialization metadata advertised a second track containing stereo AAC at 48 kHz, but the fragments supplied no samples for that track.

This is stronger evidence than the earlier Pi decoder-queue logs. The missing audio is present as a defect in the exported media before the Pi player parses or decodes it. The user confirms the same channel plays with both picture and sound inside UHF on the iPhone. A decoded still from the captured media visibly shows the Sky News logo and its 14:09 clock, independently confirming the channel.

The practical next step is a UHF/iPhone AirPlay-export bug report or focused export-setting test, rather than further Pi audio-driver, decoder or buffering changes. This capture does not identify whether UHF's own conversion code, an iOS export component, or a negotiated export setting causes the omission. It does establish that receiver audio decoding/output cannot recover the missing soundtrack from these files.

## Capture method and scope

The current mpv child had connections to the iPhone's local HTTP server. Its `/stream.m3u8` playlist was still available. A separate HTTP client downloaded the playlist, its initialization data, and its last three listed media fragments, saving response bodies unchanged before any media processing. All requests returned HTTP 200 and every downloaded body matched its declared Content-Length. SHA-256 hashes were recorded.

This was a separate download from the same phone export, **not passive capture of the exact TCP connection used by mpv**. It bypassed UxPlay, GStreamer and mpv completely. There was no receiver restart, playback replacement, settings change, or new decoder build. No privileged network sniffer was available; none was installed.

The playlist was a media playlist, with no separate audio-rendition reference, encryption directive or byte-range directive. It had 55 listed segments and no ENDLIST. The sampled sequences were 52–54. The result applies directly to these three exported fragments, not every channel or every moment of this stream.

## Independent checks

| Playlist sequence | Duration | Video samples, track 1 | Audio samples, track 2 | Media payload bytes |
| --- | --- | --- | --- | --- |
| 52 | 10 s | 250 | 0 | 1,877,896 |
| 53 | 10 s | 250 | 0 | 1,883,593 |
| 54 | 10 s | 250 | 0 | 1,872,523 |

A direct MP4 box inspection found only video track fragments (`traf` track ID 1). It counted samples from `trun` tables without decoding. A second pass summed the declared sample sizes: video samples accounted for **every byte** in each `mdat` payload, leaving no unaccounted media payload that could hold audio. The initialization declares track 1 as `vide`/`hev1` and track 2 as `soun`/`mp4a`.

Separately, FFprobe identified HEVC plus advertised AAC stereo at 48 kHz, counted 250 video packets per fragment, and found no audio packets. Inspection completed without FFprobe errors. A separate audio-only packet enumeration also returned zero packets for each file.

## Retained evidence

- Sanitized capture summary and hashes (local `.pi-dev/sky-news-raw-export-20260911.json`)
- Raw init and three unchanged media fragments are retained privately on the Mac under `/private/tmp/sky-news-raw-evidence-20260911/` and on the Pi under `/tmp/uhf-wire-media-22w8f1al/`. These temporary files may be removed by system cleanup; they are deliberately outside Git.
- A channel-verification still is retained at `/private/tmp/uhf-sky-news-captured-frame-20260911.jpg`.
- [Earlier log-only assessment](2026-09-11-sky-news.md) remains as the evidence history. Its uncertainty about whether this Sky News export contains audio is resolved by this direct media capture.
