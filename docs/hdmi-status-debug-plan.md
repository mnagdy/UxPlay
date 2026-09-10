# HDMI status screen and developer overlay

Planning proposal — 10 September 2026. Companion to the [mpv integration plan](airplay-video-mpv-plan.md). This adds receiver feedback to the planned scope; it does not implement or deploy a display service or an OS image.

The user currently sees a terminal login on HDMI until playback starts. Once the receiver service starts, show its actual state on the projector. During playback, an optional developer mode shows what arrived, which decoder is running and where progress stopped. The same vocabulary and measurements must work with GStreamer and mpv.

## Controls and normal behavior

Proposed startup option: `-screen-info off|status|debug`, with the corresponding configuration line `screen-info status` or `screen-info debug`. These commands are now implemented in the development build; see [implementation status and limits](mpv-screen-development.md). Command-line selection overrides configuration. Follow the initial backend switch's restart-based configuration behavior.

| Mode | HDMI behavior |
| --- | --- |
| `off` | Existing behavior, for comparison or recovery. |
| `status` | Receiver name, startup/readiness, incoming request, loading, pause/buffering and actionable errors. Successful playback has no persistent information panel. |
| `debug` | All status feedback plus a persistent compact stream overlay and the latest diagnostic milestones. |

Select `status` in the Pi receiver's release configuration. Preserve `off` for unconfigured upstream/desktop installations so this feature does not unexpectedly acquire their display. Development test configurations select `debug` explicitly. This flag changes presentation, not decoding policy, AirPlay capabilities or retry behavior; it must not enable verbose URL-bearing logs.

The initial display is a quiet, high-contrast screen with large text, the receiver's configured name and a short instruction: **“Choose Projector in your app's AirPlay menu.”** Use the real name rather than hard-coding Projector. No keyboard or mouse interaction is required on the Pi. A low-rate idle display should consume little CPU and no audio device.

## Status messages and what they mean

| Screen text | Required evidence / behavior |
| --- | --- |
| **Starting receiver** | Display is available; receiver setup is still in progress. |
| **Waiting for network** | A required usable interface/address is unavailable. Update from observed network state. |
| **Ready to receive** | Required listener(s) are bound, discovery registration has succeeded, selected backend preflight has passed and no blocking session/device recovery is outstanding. This does not claim a phone elsewhere on the LAN has discovered it. |
| **Incoming request** | An actual AirPlay play/mirroring/audio session request was received. Discovery probes and status polling alone must not trigger it. Show a sender/app name only if supplied and safe to display. |
| **Preparing stream** | Negotiating or collecting playlists; show the current substage in debug mode. Codec/resolution remain “Detecting…” until known. |
| **Opening video** | Renderer/decoder/output is being initialized. Keep elapsed time visible. |
| **Buffering** / **Waiting for stream data** | Observed cache shortage or lack of media progress. Show a real percentage when available; otherwise elapsed wait time. Never manufacture a progress bar. |
| **Playing** / **Screen mirroring** / **Audio connected** | Corresponding output progress has been observed. Renderer-ready or child-alive alone is insufficient. Audio-only mode retains a status surface without stealing ALSA. |
| **Paused** / **Seeking** | Reflect requested pause separately from buffering and show seek completion from player evidence. |
| **Switching streams** / **Stopping** | Old playback is being released; incoming session identity and state remain separate. |
| **Playback failed** | Show a short safe reason, the failed stage and whether another stream can be tried. Keep it readable until replacement or confirmed recovery; retain the last error in debug history. |
| **Receiver unavailable** / **Recovery required** | Discovery/listener/backend failure or a device that could not be released. Never show Ready while the receiver remains blocked. |

On successful playback, clear the large status message promptly and remove the normal-mode information panel. Show concise pause/rebuffering feedback only while relevant. On a clean stop, restore Ready only after output release and receiver readiness are confirmed. Coalesce fleeting states so the screen does not flash; retain their original timestamps in debug history. Every message belongs to a receiver or session generation so late events cannot overwrite a replacement session.

## Developer overlay

Keep the principal panel at the top left with approximately 5% overscan-safe margins. Use readable projector-sized text and a solid/translucent backing with proven contrast. Keep subtitles and the center of the picture clear. Target six to eight compact lines; an optional recent-event strip can occupy the top-right or idle screen rather than flooding the picture with logs.

Show these grouped fields when known:

- **Session:** direct AirPlay video, mirroring or audio-only; safe sender/app label; session ID; live versus on-demand; source route such as phone HTTP or negotiated playlist cache.
- **Video:** codec, profile, resolution, declared frame rate and bit depth. Distinguish input resolution from HDMI output mode. Treat HDR metadata as a declaration, not verified HDR presentation.
- **Playback:** selected backend, requested decode policy, actual decoder, confirmed hardware/software path and decoded-buffer memory/copy path where observable. Show “Hardware requested; detecting active decoder…” until verified. DMA-BUF alone must not be labeled end-to-end zero-copy.
- **Progress:** elapsed time since request, current startup stage, last media progress age, observed output/frame count or frame rate, dropped frames and buffer level/duration where available. Label estimates; unavailable values are “Unknown” or “Not reported.”
- **Audio:** codec, channels/sample rate when known, selected output, mute/volume, and whether samples have reached the observed audio stage. “No audio received yet” does not claim that an entire source has no soundtrack.
- **Device:** output mode, CPU load, temperature/throttling when available; poll slowly. Put release/build identity on the idle/debug detail view instead of occupying playback space permanently.
- **Recent milestones:** bounded history such as request → playlist ready → media received → decoder selected → first output → buffering/error. Show timestamp offsets and a short error ID matching the journal.

Refresh changing numbers at about 1 Hz, with immediate updates for state changes and failures. Do not perform synchronous network calls or resource-heavy probes on the rendering path. Preserve the same measured facts in a bounded structured snapshot usable by logs and both screen presenters; do not derive state by scraping console text.

Do not display signed stream URLs, IPTV credentials, cookies, keys, authentication material or raw errors containing them. Show short allowlisted error descriptions and stage names. Escape/truncate app metadata, including Pango/ASS markup, line breaks and control characters; stream-supplied text must not become display commands or property expansions. A private IP address is optional debug information, not a normal-screen requirement.

## Rendering and HDMI ownership

Reuse the planned receiver/session coordinator as the source of status. Add a small status snapshot/presenter boundary that is independent of the direct-video backend and also receives mirroring, audio-only, network and discovery events. It must exist before a video request arrives and after that session ends.

For waiting/startup/error screens, first evaluate a lightweight GStreamer status surface using a static or low-rate frame and text. It is a display owner managed by the same coordinator, not another independent player competing for KMS. While audio-only playback runs, this surface may own display alongside GStreamer's separate audio owner; coordinate the existing cover-art renderer so two display pipelines do not compete.

For mpv playback, use its native OSD through the private control connection; investigate `osd-overlay` for persistent layout and `show-text` for short messages. Verify output availability during load and buffering on the selected KMS path. mpv's OSD supports application-controlled overlays, but it cannot display before its video output exists. [mpv OSD commands](https://mpv.io/manual/stable/#osd-commands).

For GStreamer playback and mirroring, first qualify an overlay-capable sink/composition path that retains the tested decoder/buffer behavior. `textoverlay` is a candidate for software-mappable frames, not an assumption that hardware buffers can be drawn into cheaply. Inspect negotiated memory and any new conversions/copies. A DRM-plane solution, if needed, must share explicit DRM ownership; launching a competing KMS process is not an overlay implementation. [GStreamer textoverlay](https://gstreamer.freedesktop.org/documentation/pango/textoverlay.html).

The overlay must work for both backends on the declared supported profiles. Do not silently switch hardware playback to software to draw debug text. If the existing GStreamer KMS path cannot composite efficiently, treat the GStreamer overlay as an explicit implementation dependency and report the limitation in the plan/test result; do not claim backend parity from the mpv overlay alone. Avoid installing a compositor as part of this first design unless that proves necessary and is separately reviewed.

Keep a loading/status surface until the next renderer requires the display, then release it and transfer ownership. Once the player owns the output, its presenter supplies loading/debug feedback where possible. Measure the transition's blank interval and prevent terminal/login flashes during ordinary handovers. Do not promise a perfectly uninterrupted picture without verifying the KMS transition.

Run this under the actual receiver service account and virtual-terminal environment. Plan a dedicated graphics VT/display lifecycle that keeps the login prompt off the active HDMI once the receiver owns it, preserves SSH and a recoverable console, and restores the prior setup on rollback. Do not blindly disable all getty services, enable automatic shell login or edit global boot settings. Cover service start, restart, stop, ordinary crash and display disconnect/reconnect. Firmware/early-kernel boot screens before the receiver starts are outside this milestone.

If the kernel/DRM device is wedged, drawing a fresh HDMI error may itself be impossible. Preserve bounded off-screen diagnostics and recovery state; show the error only if display ownership is usable. A frozen last frame must never be cited as proof that the receiver remains responsive.

## Delivery and acceptance

1. Add mode parsing, the common status snapshot and idle/startup/error screen for the existing GStreamer receiver. Verify Ready/incoming/failed transitions from actual events. This can be delivered independently of HEVC success.
2. Add the GStreamer debug overlay and mirroring/audio-only coverage; qualify its effect on the current working HD path before changing decoding.
3. Wire the same status/presenter contract into mpv and verify every handover, including failures before the first video frame.
4. Include both display modes in HEVC/4K qualification, recording the exact overlay mode with every performance result.

Acceptance includes:

- After service startup, an actionable receiver screen replaces the login prompt; Ready is withdrawn on observed readiness loss.
- A real incoming request becomes visible promptly; stalled negotiation, unavailable playlist, decoder failure, missing/late audio and output failure can be distinguished without a terminal.
- Debug information reflects the active session/backend and the actual decoder. Unknown fields stay unknown. Simulated mockup values are never used as runtime defaults.
- Normal mode clears successful-playback overlays; debug remains readable on the projector at the supported HDMI modes without covering subtitles or clipping long names.
- MPV/GStreamer, video/mirroring/audio-only and successive-phone handovers restore the right screen and do not leave stale metadata, competing DRM owners or leaked processes.
- Compare the same streams with `off`, `status` and `debug`: record CPU, frame delivery, dropped frames, startup time and buffer/memory negotiation. Require unchanged decoder selection and no new playback failures; report measured overlay overhead before accepting 4K results.
- Sanitization tests cover URLs, credential-bearing errors, long/unicode metadata, control characters and markup/property-injection attempts. State tests cover delayed old events and failure before any player is ready.

The preview accompanying this plan uses illustrative stream values. It demonstrates layout and states, not successful HEVC/4K playback on the Pi.
