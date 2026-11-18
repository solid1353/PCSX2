# Input recording capture

The fork changes how input recordings are opened from the command line and
adds marker and frame-exact video capture during replay. The implementation is in
`pcsx2/Recording/` and the option parsing in `pcsx2-qt/QtHost.cpp`.

## Recording paths

`-input-recording <path>` replays a recording and
`-input-recording-create <path>` starts a new power-on recording. An absolute
path is used exactly. A relative path is resolved below the primary
`InputRecordings` folder and must not contain `..`; additional content folders
are not searched. PCSX2 does not create missing parent directories.

A recording that starts from a savestate keeps that savestate in the
directory `<recording>_SaveState` next to the recording.

## Read-only playback

Every replay, from the command line or the menu, opens the recording
read-only with shared read access, so several PCSX2 processes can replay the
same file at once. Record mode cannot be enabled during a replay, and the
recording's header and frame data are never rewritten.

When a replay reaches its last frame, PCSX2 pauses. Resuming at that point
ends the replay. With a capture directory, PCSX2 instead closes the recording
after the final frame and shuts the VM down, which exits a batch-mode
process.

## Frame-exact video

`-input-recording-video <path.mkv> -input-recording-video-frames <first>-<last>`
writes one Matroska file during `-input-recording` playback. Both options are
required, together with a boot filename or `-bios`. The parent directory must
already exist; an existing output file is replaced. Relative output paths
resolve from the process's working directory.

The range is inclusive and uses `InputRecording::m_frame_counter`, the same
counter shown by the `Frame: N/total` OSD. Valid counters are `0` through
`total`, including single-frame ranges such as `120-120`. Counter zero is the
initial power-on interval, before the first replay input poll. Each counter
selects the image at the VSync ending that interval and every SPU2 sample
generated during it. `120-179` therefore writes exactly 60 video frames.
Only power-on recordings are accepted, and ranges beyond the recording fail.

Capture uses fixed 640 x 480 frames, single-threaded lossless FFV1 in RGB,
and stereo PCM float audio at the emulated console's sample rate. It ignores
the desktop capture codec, resolution, bitrate, and encoder-parameter settings.
Codec and container bitexact flags exclude wall-clock muxing metadata. The
recording's existing fixed RTC applies; it advances with emulated VSyncs.
Use the same emulator build, rendering settings, BIOS, ISO, PNACHs, and initial
card contents for repeated takes. Persistent card writes can change the next
run's initial state; discarded or volatile writes preserve the source card.

`-surfaceless`, `-mute`, speed options (including Unlimited), launch PNACHs,
and all memory card modes work with video capture. Audio is collected directly
from SPU2 before host buffering, volume, mute, or time stretching. Start,
sample delivery, and stop are ordered with the selected GS VSyncs; skipped
presentation does not omit video frames. GS resets and renderer switches keep
the same output file and encoder timestamps. Concurrent processes can share the
read-only recording and must use separate output paths.

After the final selected frame, PCSX2 drains the encoder, closes the file,
logs `Input recording video capture complete: N frames written to 'path'.`,
and shuts down the VM. `-batch` (also implied by `-surfaceless`) then exits.
Capture failures log an error, shut down, and exit with a nonzero process status,
including startup failures and interrupted captures. Successful batch capture
exits with status zero. Capture playback starts running even if **Start Paused**
is enabled, and does not require the recording tools setting. Capture requires
the FFmpeg runtime with FFV1 and PCM float encoders. It cannot accompany
`-agent-replay`.
Marker capture may accompany video, but savestate creation can affect a game's
rendering; use video alone for takes that must avoid those side effects.
Repeated decoded video/audio equality, capture timing, and renderer switches
still need proper runtime testing; unit tests cover parsing and validation,
not encoder or emulator output.

## Marker capture

Capture is enabled by any of these options, which require
`-input-recording` playback (a capture directory without playback is
ignored):

| Option | Effect |
| --- | --- |
| `-input-recording-capture-directory <path>` | Writes captures to this directory and exits when the replay ends. |
| `-input-recording-capture-mode full\|screenshots\|savestates` | Selects the outputs; the default is `full`. |
| `-input-recording-capture-markers <spec>` | Captures only the listed 1-based marker numbers, such as `1,3-5`. |

A marker spec is a comma-separated list of positive numbers and ascending
`first-last` ranges; overlapping and adjacent ranges merge. Capture works only
with power-on recordings; a recording that starts from a savestate fails to
start.

A marker is the rising edge of L3 and R3 held together on port 1 or port 2 in
the replayed input, so holding the chord produces one marker. Markers are
numbered in replay order, including unselected ones, and each capture is named
with its marker number padded to three digits: `001`, `002`, and so on.

| Mode | Output for marker `NNN` |
| --- | --- |
| `full` | `NNN.png` and savestate `sstates/NNN/` |
| `screenshots` | `NNN.png` |
| `savestates` | savestate `sstates/NNN/` |

With a capture directory, PNGs go directly in it and savestates in its
`sstates/` subdirectory. Without one, savestates go to
`<Savestates folder>/<recording name>/NNN` and PNGs to
`<Snapshots folder>/<recording name>/NNN.png`. PCSX2 creates these
directories but does not empty them; an existing savestate with the same
name is replaced.

Savestates are directories containing the state components and a
`Screenshot.png`. In `full` mode the standalone PNG is the same encoded image
as the savestate's `Screenshot.png`. If the memory card is busy, the savestate
is skipped and logged, but a `full` capture still writes its PNG. Screenshots
use the savestate screenshot encoder and size.

While `SaveStateGSDump` in `[EmuCore]` is on (the default, with no UI control),
the GS thread keeps the last four completed frames: the GS state at the VSync
that starts each one and the GS data sent during it
(`pcsx2/GS/GSState.cpp`). Every savestate, ordinary or recording capture,
writes them as a GS dump that starts from the oldest kept state and ends on the
last presented frame, the one in the savestate's screenshot. The Zstandard
stream is saved as `GS.gs.zst` inside the savestate directory once the
savestate is written successfully, whether or not emulation is paused. The
configured GS dump compression does not affect these dumps.

Resetting the VM or loading a state empties the kept frames, so a savestate made
before a frame has completed since then has no dump, and one made sooner than
four frames after has fewer. Stopping or resetting the VM, or loading another
state, before the savestate is written discards its dump. Dump failures do not
affect the saved state, and incomplete output is discarded. Loading reads only
known state components and ignores the dump.
Directory backups and copies include it, and replacing or deleting a slot removes
it with the other state files.

## Tests

- `tests/ctest/core/gs_dump_tests.cpp` covers completing an in-memory Zstandard
  stream at a compression-buffer boundary, writing recorded packets the same as
  direct ones, and discarding unfinished dumps.
- `tests/ctest/core/input_recording_tests.cpp` covers concurrent read-only
  playback, the record-mode lock during playback, capture mode and marker
  parsing, marker selection, capture directories, and inclusive video frame
  ranges (including zero, equal endpoints, malformed ranges, and overflow),
  plus rejection of invalid capture options and savestate recordings before boot.
- `tests/ctest/core/patch_tests.cpp` (`InputRecordingPath` tests) covers exact
  absolute paths and relative paths below the primary folder.
