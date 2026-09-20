# Keyboard and mouse input

The default keyboard/mouse backend is `reboxed`. The inherited synchronous
implementation remains available temporarily with `--input-backend=legacy`.

## Event path

SDL receives X11 keyboard, mouse, focus, visibility, and window lifecycle
events. The revised backend converts SDL scancodes and mouse buttons in
separate keyboard and pointer state machines. Both submit timestamped batches
to one ordered dispatcher. Its worker writes directly to the host uinput
devices which are exposed to Android as `/dev/input/event*`; Android InputReader
then performs normal Linux input processing.

The revised path does not send the same event through the obsolete input
socket. The legacy backend continues to exercise the old socket-plus-uinput
behavior for comparison.

## Ordering and queue policy

Each keyboard and pointer boundary receives a source sequence number and a
`steady_clock` timestamp. The shared dispatcher assigns a transport sequence,
records enqueue time, and preserves FIFO order across keyboard and mouse
batches. Every logical batch ends with `SYN_REPORT`.

The queue has 384 slots. Only 128 may be consumed by non-release events; 256
are reserved so every possible Linux keyboard key and supported mouse button
can be released after a stall. Consecutive pending pointer-motion batches are
coalesced without crossing a key, button, or wheel boundary. Motion, wheel,
and repeat state older than 100 ms is discarded instead of replayed. Key and
button transitions are not age-dropped because doing so would break down/up
pairing.

Host key repeat is ignored by default and Android InputReader owns repeat
timing. `--key-repeat=forward` deliberately sends Linux repeat value `2` for
comparison.

## Held state and failures

Keyboard and pointer state machines track host-held and successfully queued
state separately. Duplicate downs and unmatched ups are ignored. Focus loss,
minimize/hide, window close, SDL quit, and backend shutdown enqueue forced
release batches. A uinput write handles partial writes, interruption, and a
short nonblocking retry. On permanent failure the uinput device is destroyed,
causing Android InputReader to remove the device and release its state; queued
events and internal held state are then cleared, so a restarted session cannot
replay them.

## Coordinate mapping

Absolute pointer positions consume the renderer's published viewport snapshot
and use the same `CoordinateTransform`; input never recalculates aspect fit
from SDL window dimensions.
Android resolution, host window size, aspect-ratio letterboxing, and rotation
are independent inputs. Coordinates in letterbox bars are rejected. Absolute
SDL logical coordinates are first scaled into the exact EGL drawable space.
Pointer motion, presses, and wheels are suppressed while a resize generation
has no valid rendered viewport; releases still pass through to avoid stuck
buttons.
results are encoded on a stable 0..65535 uinput axis, allowing Android
InputReader to rescale them after an Android resolution change without
recreating the device. Relative pointer lock maps deltas by the content scale
and emits `REL_X`/`REL_Y`.
Portrait, landscape, and 0/90/180/270-degree transform math are independently
testable. The currently supported session orientations use the appropriate
Android framebuffer dimensions with a zero-degree presentation transform.

Mouse-to-single-touch emulation remains separate and unchanged; it is outside
the rewritten mouse pointer state machine. Use `--no-touch-emulation` when
validating native mouse buttons and absolute/relative pointer behavior.

## Diagnostics

Startup logs report backend, queue capacity, release reserve, repeat policy,
and transport. Sampled boundary logs contain source sequence, transport
sequence, stream, kind, queue depth, and source-to-injection latency. Shutdown
summaries report submitted/delivered/dropped/coalesced events, transport
failures, maximum queue depth and latency, repeats, duplicates, and forced
releases.
