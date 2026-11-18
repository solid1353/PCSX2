# Scripted takes

`-capture-script "<statements>" <boot.iso>` runs a take inside the emulator,
starting at power-on. The argument contains the script itself, not a filename.
Statements are separated by `;`; a final `;` is optional. Pass the entire
script as one argument, including on macOS. Names are lowercase.

```text
-surfaceless -mute -unlimited -capture-script "wait [[0x00607604]+8]>>16 == 4 within 7200; run 60; capture D:\out\take.mkv; run 30; hold cross; run 4; hold none; run 300; stop" "game.iso"
```

## Statements

| Statement | Effect |
| --- | --- |
| `wait <condition> within <frames>` | Execute frames and check the condition at each completed VSync. The last allowed frame may satisfy it; otherwise fail at that boundary. Always executes at least one frame. |
| `run <frames>` | Execute that many VSync intervals. Zero is allowed. |
| `hold <buttons>` | Replace pad 0's held buttons for the next interval and all following intervals until another `hold`. |
| `sticks <rx> <ry> <lx> <ly>` | Replace pad 0's right X/Y and left X/Y stick bytes for the next interval and following intervals until another `sticks`. Each axis is an integer in 0–255, decimal or hexadecimal. |
| `capture <path.mkv>` | Arm video and audio capture for the next interval. The parent folder must exist; replace an existing file. Relative paths use the working directory. Spaces in the path are accepted; optional double quotes around the path are removed. |
| `stop` | Drain and finalize the active capture before executing another statement. |

Buttons are `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`,
`right`, `l1`, `l2`, `l3`, `r1`, `r2`, `r3`, `start`, and `select`, joined
with `+`. `hold none` releases the buttons without changing the sticks.
Pad 0 starts neutral, with all stick axes at 127, and pressure buttons use
full pressure. Pad 1 stays neutral.
Host pad bindings and macros cannot supply input during a script.

Stick values and their order exactly match recording pad bytes 2–5:
right X, right Y, left X, left Y. Zero is left/up, 255 is right/down, and
127 is centered. `sticks 127 127 127 127` centers both sticks. Button and
stick statements are independent, so both can remain held across `run` and
`wait` statements. For example:

```text
sticks 127 127 255 127; hold cross; run 30; hold none; sticks 127 127 127 127
```

Instant statements consume no frames. `capture take.mkv; run N; stop` writes
exactly N frames and the SPU2 samples produced during those intervals. A
capture must have a matching `stop`; nested captures, stopping without capture,
and an empty capture fail. Multiple captures in one script are supported.

## Conditions

A comparison is `<read> <operator> <number>`. Operators are `==`, `!=`, `<`,
`<=`, `>`, and `>=`. Join comparisons with `&&` and `||`; `&&` binds tighter.

Reads are `[address]` (u32), `h[address]` (u16), `sh[address]` (s16), and
`b[address]` (u8), using little-endian EE RAM. An address is a number or a
nested read followed by any number of `+number` offsets. Each read may have
one `>>number` and one `&number`, evaluated in their written order. Shifts
are 0–31 and arithmetic for signed reads. Masks are unsigned 32-bit values.
Numbers are decimal or `0x` hexadecimal; a comparison's right side also
accepts negative numbers for signed reads.

Null pointer dereferences, overflowing addresses, and reads that cross or
lie outside exposed EE RAM make their comparison false, including `!=`.
A literal address zero is valid RAM. Pointer aliases outside that RAM range
are not mapped. Nesting is limited to 64 reads.

```text
wait [[[0x00607620]+0x34]] != 0 && [[[0x00607620]+0x34]] != 1 && [[[0x00607620]+0x34]] != 6 within 7200
```

## Capture and completion

Capture uses the same internal render resolution, configured capture
encoders and per-frame SPU2 audio as
[`-input-recording-video`](input_recording_capture.md#frame-exact-video).
Audio is sampled before host mute, volume, buffering, and time stretching.
Input changes, conditions, audio, and capture boundaries depend on emulated
VSyncs, not polling or wall-clock time. Scripts use the recording RTC default
(2020-03-04 00:00:00) unless a manual RTC is configured.

`-surfaceless`, `-mute`, `-pnach`, `-pnach-line`, all memory card modes,
and speed options including `-unlimited` can accompany this option. As with
input recording video, repeated takes require the same build, rendering
settings, BIOS, ISO, PNACHs, and initial card contents. Persistent card writes
can change the next launch's input; discard or volatile modes preserve the
source card. Input recording/replay and savestate boot options cannot be
combined with a capture script.

Completion logs `Capture script complete: N emulated frames.` and exits zero
in `-batch` or `-surfaceless` mode. Otherwise the game keeps running with the
last scripted pad state. Parse errors, wait timeouts, boot errors, capture
errors, or interruption before completion log an error and exit nonzero.
The complete script is parsed before boot. Capture startup fails if the first
selected frame has no valid video mode.
