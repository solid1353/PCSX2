# Input recording capture

The fork changes how input recordings are opened from the command line and
adds marker and frame-exact video capture during replay. The implementation is in
`pcsx2/Recording/` and the option parsing in `pcsx2-qt/QtHost.cpp`.

Input recording notifications appear only for sessions started in-game with
**Toggle Input Recording** or the Tools menu. Launch-started sessions,
including `-input-recording-create`, `-input-recording`, capture scripts, and
agent replay, keep input recording messages in the console log without OSD
notifications for their whole life, including mode changes, errors, stopping,
and power-on restarts. A later hotkey or menu session shows notifications again.

## Recording paths

`-input-recording <path>` replays a recording and
`-input-recording-create <path>` starts a new power-on recording. An absolute
path is used exactly. A relative path is resolved below the primary
`InputRecordings` folder and must not contain `..`; additional content folders
are not searched.

`-input-recording-folder <path>` overrides the primary `InputRecordings`
folder for this process without changing settings. A relative folder path
resolves from the working directory of the command that launched PCSX2.
The override applies to hotkey saves, the Tools → Input Recording file
dialogs, and relative recording launch paths, regardless of option order.
Without the option, the configured folder is used.
The menu's New dialog proposes `recording.p2m2` there (with a numbered suffix
if needed), so it can save into a missing folder without browsing first.

Startup and replay do not create the recordings folder, with or without
the override. Missing parent directories are created only when a recording
file is about to be written, through the hotkey, the menu, or
`-input-recording-create`. Absolute recording filenames still select their
exact destination, whose missing parents are created at that point too.

A recording that starts from a savestate keeps that savestate in the
directory `<recording>_SaveState` next to the recording.

During a power-on recording in record mode, **System → Restart** fully shuts
down and boots the VM with the original launch's disc or ELF and boot options,
with the same process PNACHs, memory card mode, and settings applied as on a
fresh launch. The recording stays active during boot so its fixed RTC applies.
Volatile memory cards reload their source contents; persistent card writes
remain on disk, so reproducing a run still requires the same initial card
contents. The same recording file is reopened at frame 0 in record mode,
preserving its notification policy and whether emulation was paused.
Both frame counters, the total frame count, and the undo count return to zero,
and all previously recorded pad samples are removed. The file keeps its game,
author, and emulator metadata and contains only the run after the last restart,
even when that run is shorter. A reset during playback keeps the existing replay
behavior. Savestate-based recordings and input-only recordings are unchanged.

## Recording from the current frame

Bind **Toggle Input Recording** in Hotkey Settings → System. It has no
default binding. With a game running and no recording or replay active,
it starts logging the current pad input without resetting the VM or saving
a state. Recording tools need not be enabled. The screen shows **Recording
input** throughout the recording, even when the ordinary input recording
overlay setting is off.

Press it again to close the file. The saved-file message names
`<game> - YYYY-MM-DD HH.MM.SS.p2m2` in the configured primary
`InputRecordings` folder, or the process folder selected with
`-input-recording-folder <path>`. If that name already exists, a numbered suffix
is added. Both pad ports are sampled once per input frame; resets and
state loads do not rewind this log. Other recording or replay sessions
must be stopped before starting one, and capture scripts cannot accompany it.

These files contain input only, with no starting state. PCSX2 refuses to
replay them with **This input recording has no starting state and cannot
be replayed.** They are intended for reading the captured input and copying
it into a [capture script](capture_script.md).

## Recording file format

All multi-byte integers are little-endian. The fixed header occupies 570
bytes. Text fields are NUL-terminated byte arrays.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Format version: 1 for existing power-on/savestate recordings; 2 for input-only recordings. |
| 1 | 50 | Emulator version. |
| 51 | 255 | Author. |
| 306 | 255 | Game name. |
| 561 | 4 | Total frames. For input-only recordings this is the number of stored frames, indexed 0 through count−1. |
| 565 | 4 | Undo count; zero for input-only recordings. |
| 569 | 1 | Starting-state type: 0 = power-on, 1 = companion savestate, 2 = no starting state. Value 2 requires version 2. |
| 570 | 36 per frame | Two consecutive 18-byte pad samples, port 0 then port 1. |

A reader identifies an input-only recording by **version 2 at byte 0 and
starting-state type 2 at byte 569**. It can read every pad frame without any
game state. Existing version-1 fields, offsets, and pad data remain unchanged;
for their total frame counter semantics, see [frame-exact video](#frame-exact-video).

Each 18-byte pad sample contains:

| Bytes | Meaning |
| --- | --- |
| 0 | Active-low button bits 0–7: select, L3, R3, start, up, right, down, left. |
| 1 | Active-low button bits 0–7: L2, R2, L1, R1, triangle, circle, cross, square. |
| 2–5 | Raw unsigned stick axes: right X, right Y, left X, left Y; 0–255 with center 127. |
| 6–17 | Pressure bytes 0–255: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2. |

Copy pad bytes 2–5 directly into `sticks <rx> <ry> <lx> <ly>` in a capture
script. Each following frame begins 36 bytes after the previous frame.

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

Capture uses the first selected frame's internal render resolution, including
the rendering upscale, through the same size selection as PINE
`StartVideoCapture` with automatic resolution. GSCapture rounds each dimension
up to a multiple of eight, as it does for PINE (for example, 2560 x 2240 stays
2560 x 2240). The size is fixed for the whole file; later internal resolution
changes scale into that starting size without restarting capture. Window size
and surfaceless mode do not choose the output size.

Capture encodes with the configured video and audio capture settings, as PINE
`StartVideoCapture` does: codec, bitrate, pixel format, encoder parameters and
hardware encoding. Only the output size comes from the internal resolution
above, never the configured capture resolution. Each frame's SPU2 samples are
encoded in the audio codec's frame size; the last incomplete audio frame is
written when the codec accepts a short final frame.
The recording's existing fixed RTC applies; it advances with emulated VSyncs.
Use the same emulator build, rendering settings, BIOS, ISO, PNACHs, and initial
card contents for repeated takes, so they show the same frames. Persistent card
writes can change the next run's initial state; discarded or volatile writes
preserve the source card.

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
the FFmpeg runtime with the configured encoders. It cannot accompany
`-agent-replay`.
Marker capture may accompany video, but savestate creation can affect a game's
rendering; use video alone for takes that must avoid those side effects.
Capture timing, resolution changes, and renderer switches still need proper
runtime testing; unit tests cover parsing,
validation, and resolution selection, not encoder or emulator output.

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
  plus rejection of invalid capture options and savestate recordings before boot,
  and internal capture resolution selection independent of desktop capture
  settings.
- The same tests cover the input-only format marker, raw pad round trips,
  rejection of input-only replay, invalid origin markers, and truncated samples.
  They also cover shutdown/boot ordering for power-on recording restarts,
  recording activity during boot for the RTC, retained launch parameters,
  failed restarts, discarded deferred actions, removal of old samples and
  counters, shorter replacement runs, and unchanged playback/input-only resets.
  Memory card tests cover reloading volatile source contents on a recording
  restart while ordinary reopens retain RAM writes.
  Recording creation also covers missing parents for every recording origin
  and directory-creation failures.
  Notification tests cover console logging and OSD delivery for quiet and
  interactive sessions through restarts, mode changes, close, and errors,
  including restoring notifications for a later hotkey recording.
- `tests/ctest/core/emu_folders_tests.cpp` covers process recording folder
  overrides across settings reloads, unchanged settings, relative path
  resolution, and lazy directory creation for configured and overridden paths.
- `tests/ctest/core/capture_script_tests.cpp` covers raw stick values, axis
  order, validation, persistence, and frame boundaries independently of buttons.
- `tests/ctest/core/patch_tests.cpp` (`InputRecordingPath` tests) covers exact
  absolute paths and relative paths below the primary folder.
