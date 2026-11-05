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

A save state's GS dump records the 4 frames after the save, so it is written
only once those frames run. Saving to a slot then shows "Saved GS dump to slot
N." when the dump is written, "Failed to save GS dump to slot N." when writing
it fails, or "GS dump for slot N was discarded." when loading a state,
resetting, or closing the game drops it first (`pcsx2/VMManager.cpp`).

- [Launch options](launch_options.md): command-line options for windows,
  speed, settings, memory cards, PINE, and agent replay.
- [Input recording capture](input_recording_capture.md): recording paths,
  read-only and concurrent playback, and L3+R3 marker capture.
- [Content folders and aliases](content_folders.md): additional content
  folders, game settings lookup, content alias bundles, and canonical local
  identity.
- [PNACH files](pnach.md): PNACH discovery, section markers, CRC lists, and
  command-line PNACH files and lines.
- [PINE agent control](pine_agent_control.md): the fork's PINE opcodes for VM
  control, agent input, and replay analysis.
