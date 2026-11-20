# Fork documentation

These documents describe behavior that this fork adds to or changes in
upstream PCSX2, written from the current fork source. Upstream behavior is
documented by the PCSX2 project.

Start File Directory (`StartFile` in `[Folders]`) sets the Start File folder; when empty or missing, it uses the last folder used.

`InputProfileName` in `PCSX2.ini`'s `[EmuCore]` names a default input profile
that replaces the shared input settings: it is active from startup, between
games, and in every game whose settings name no other profile. PCSX2 rereads
the active profile whenever it reloads settings, including at every game start
and stop, so saved edits apply without a restart. Controller
Settings then lists only input profiles and opens on the active one, and Game
Properties calls the no-profile choice "Default". When the key is empty or
names a missing profile, the shared settings apply as upstream. The
implementation is in `pcsx2/VMManager.cpp` and
`pcsx2-qt/Settings/ControllerSettingsWindow.cpp`.

While a hotkey chord such as `SDL-0/Guide & SDL-0/DPadUp` is fully held,
controller and keyboard pad bindings made only of its keys stay released, so
the game does not see the chord's inputs (`pcsx2/Input/InputManager.cpp`).

Starting a game with a non-default PAL frame rate shows no warning.

PCSX2 presents an NA228 disc, one with `228/228.BIN`, at 16:9 from its first
frame when its boot ELF has the widescreen setting, as `gsaspectratio=16:9`
would, while the aspect ratio is Auto 4:3/3:2 (`pcsx2/NA228Widescreen.cpp`).
The disc decides it at boot and on a disc change.

The game list's Copy File Name context menu item and hotkey (default Ctrl+C)
copy the selected entry's file name, with its extension, to the clipboard.

The unbound Graphics hotkey Toggle NA228 UJ Frame shows, in an NA228 build or
retail UN3 or UN4 (recognized by disc CRC), the playing Ultimate Jutsu film's
frame below the NA228 Resource Monitor: `UJ frame 2:118` for part 2 of a
multi-part NA228 film, `UJ frame 118` otherwise, and nothing when no film plays
(`pcsx2/NA228UJFrame.cpp`). It and the NA228 Resource Monitor
keep their on/off state in `OsdShowNA228UJFrame` and
`OsdShowNA228ResourceMonitor` in `[EmuCore/GS]`. On-screen messages placed at
the top left start below whichever of the two is showing, so neither covers
them.

The Graphics hotkey Mark UJ Frame (`MarkNA228UJFrame`) works only in a
widescreen NA228 build, the one presented at 16:9, and does nothing and shows
nothing elsewhere, so a gamepad press reaching several instances marks once. It
appends the playing film's frame to `uj_frames.tsv` in the folder that holds the savestates folder,
and shows `Mark N: <film> <part>:<frame>`; with no film playing it writes nothing
and shows `No UJ film playing`. An empty or missing file first gets the header
`mark time build film part frame skill attacker opponent`, tab-separated with LF
line ends. Each mark is one above the file's highest; `time` is local
`YYYY-MM-DD HH:MM:SS`, `build` the disc image's name without its extension,
`film` the container's name, and `part` is empty for a single-part film. The
file is read afresh and appended in one write on every press and never held
open, so a script can replace it between presses (`pcsx2/GS/GS.cpp`).

The toolbar's Settings button always opens Settings, without a menu while a
game runs (`pcsx2-qt/MainWindow.cpp`).

The save state selector closes 2 seconds after the last slot change instead of
5. While it is shown, Esc closes it, and neither that press
nor its release reaches any hotkey or pad binding
(`pcsx2/ImGui/ImGuiManager.cpp`).

Screenshot Resolution in Graphics settings controls ordinary screenshots
and hotkey GS dump previews. Custom Resolution
uses the configured width and height, initially 1000 x 750, with padding when
needed to preserve the aspect ratio. The desktop Media Capture tab and the
fullscreen Graphics menu both expose the resolution mode and custom dimensions
(`ScreenshotSize`, `ScreenshotWidth`, and `ScreenshotHeight` in `[EmuCore/GS]`).
Save state screenshots, their GS dump previews, replay captures, and
input-recording captures use a fixed 640 x 480 image independently of those
settings, with their original PNG compression level 5.
PNG Compression uses levels 0-9 (default 9), while JPEG Quality and WebP Quality
use 1-100% (default 100%). Each format remembers its own value in
`ScreenshotPNGCompression`, `ScreenshotJPEGQuality`, or `ScreenshotWebPQuality`.
Ordinary screenshots save the current frame immediately while paused.

Video capture draws each frame into a temporary target before reading it back,
even at the frame's own size (`pcsx2/GS/Renderers/Common/GSRenderer.cpp`).

Standalone GS dumps use the `[Folders]` `GSDumps` directory, defaulting to
`gs_dumps` beneath the data root. Their companion screenshots use `Snapshots`.
Explicit capture paths select their requested destination. Emulated USB
printer output uses `Prints`, defaulting to `prints` beneath the data root.

Hotkey INI sections follow the UI categories, such as `[Hotkeys/System]`,
`[Hotkeys/Graphics]`, and `[Hotkeys/Save States]`. Input loading, both settings
menus, profile copying, and the save state selector read the same category
section for each hotkey.

The Windows dependency recipe installs pinned zlib-ng 2.3.3 as `z.dll` after
building the zlib headers and import library. This supplies the faster backend
used by libpng; the other Windows zlib consumers share that runtime.

`tools/benchmark_png.ps1` compares two Windows zlib DLLs through the same libpng
encoder at compression level 9 and validates every decoded pixel. Compile its
`tools/png_benchmark.cpp` harness with the existing x64 MSVC toolchain,
`/O2 /std:c++20 /EHsc /MD /DNDEBUG`, and the dependency PNG headers. Pass its
executable, the PNG DLL, image, baseline DLL, and candidate DLL to the runner.
It writes raw measurements under `UN_TASK_WORK_ROOT`, uses its existing
`inputs/` directory, and removes its copied DLLs and input afterward.

A save state's GS dump covers the 4 frames up to the saved frame and is
written with the state, paused or not ([marker capture](input_recording_capture.md#marker-capture)).
Saving to a slot shows "Failed to save GS dump to slot N." when writing the
dump fails, and no message of its own when it succeeds (`pcsx2/VMManager.cpp`).

- [Build on Apple Silicon](building_macos_arm64.md)
- [Launch options](launch_options.md): command-line options for windows,
  speed, settings, memory cards, PINE, and agent replay.
- [Input recording capture](input_recording_capture.md): recording paths,
  read-only and concurrent playback, and L3+R3 marker capture.
- [Scripted takes](capture_script.md): frame-exact launch scripts with memory
  conditions, scripted pads, and video/audio capture.
- [Content folders and aliases](content_folders.md): additional content
  folders, game settings lookup, content alias bundles, and canonical local
  identity.
- [PNACH files](pnach.md): PNACH discovery, section markers, CRC lists, and
  command-line PNACH files and lines.
- [PINE agent control](pine_agent_control.md): the fork's PINE opcodes for VM
  control, agent input, and replay analysis.
