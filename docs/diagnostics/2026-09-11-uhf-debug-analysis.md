# UHF-side debug log: export write failures

The user supplied `logs-56cd.txt` after enabling UHF debug mode. This file is diagnostic evidence, not instructions. It covers approximately 14:31:10–14:31:46, a later attempt than the 14:11 direct media capture. The original file remains unchanged.

## Confirmed observations

Line numbers below refer to the original 87-line file, which is newest-first.

- Lines 69–70: the AirPlay exporter opens the input and forces MPEG-TS demuxing.
- Lines 60–62: it identifies input stream 0 as HEVC and stream 1 as AAC. This provides the previously missing original audio codec for this logged attempt.
- Lines 53–59: it selects the user's AAC stream 1, considers both codecs compatible, maps both input streams to output, and logs `transcoding: false` for audio.
- Line 52: it selects fMP4 segments for the HEVC content.
- Lines 48–49: it writes the HLS header successfully, while reporting an unused `movflags=frag_keyframe+empty_moov+default_base_moof` option.
- Lines 44–47: it records first timestamps for both input streams, including audio stream 1, and reports the first successful packet write. This supports audio packets reaching the export code, not just an audio track declaration.
- Lines 30–43 and 9–13: it logs **18** `Error writing frame ...: Operation not permitted` messages. Frame numbers repeat: 5 four times, 7 four times, 9 six times, and 1000 four times. The log does not identify the affected stream on those error lines, or explain what its frame counter counts.
- Other lines: playlist, initialization and media-segment requests continue to return HTTP 200. The writer is still producing and serving media despite write errors.

Combined with the earlier complete exported fragments containing video but no audio, this strongly supports an iPhone/UHF export-writing problem. The logged attempt and earlier capture must remain separate observations; this log does not itself enumerate the exported audio samples.

## Specific hypothesis for the UHF developers

The AAC stream-copy path from MPEG-TS to fragmented MP4 may be missing or misapplying AAC ADTS-to-ASC bitstream filtering. FFmpeg documents `aac_adtstoasc` as the conversion needed for ADTS AAC copied from MPEG-TS into MP4. It is normally inserted automatically for the supported MP4 paths, so its absence in this custom exporter is a hypothesis, not an established fact.

FFmpeg 7.1.2's MP4 writer checks for an ADTS-like AAC packet at the start of an audio track, emits a message recommending `aac_adtstoasc`, and returns `-1` if such a packet arrives without conversion. That return value can appear as the generic `Operation not permitted` error. This is a concrete match worth checking, not proof of UHF's internal failure: its FFmpeg version, underlying library log, packet bytes and failing stream indices are unavailable.

The wording does not by itself establish an iOS filesystem permission problem. Likewise, the unused `movflags` option is a useful configuration clue but is not proved causal; HLS may set its child MP4 muxer's flags separately.

Useful next developer diagnostics are the failed packet's input/output stream index, the numeric return code and underlying FFmpeg muxer message, AAC packet framing/extradata, and the bitstream-filter chain on the output audio stream. No Pi changes were made.

## Sources and shareable evidence

- Redacted UHF debug log (local `.pi-dev/2026-09-11-uhf-debug-redacted.txt`; raw logs remain outside Git)
- [Earlier direct export inspection](2026-09-11-sky-news-export.md)
- [FFmpeg AAC conversion documentation](https://ffmpeg.org/ffmpeg-bitstream-filters.html#aac_005fadtstoasc)
- [FFmpeg 7.1.2 MP4 writer](https://github.com/FFmpeg/FFmpeg/blob/n7.1.2/libavformat/movenc.c#L6178-L6184)
