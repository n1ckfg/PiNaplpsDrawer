# nap-xtz-client Architecture

An openFrameworks port of the browser client in `public/`. It renders NAPLPS
drawings, draws new ones in 3D with hand tracking, and exchanges both with
`nap-xtz-server`.

The browser version is three technologies stacked on a page — p5.js for the
canvas, Three.js plus MediaPipe-in-WASM for the drawing overlay, Beacon for the
wallet. This port keeps the behaviour and replaces the stack: `ofxNaplps` for
the canvas, `ofNode` and `ofxMediaPipe` for the drawing mode, and the server's
own REST API for the chain.

## What came from where

| Browser | Here |
| --- | --- |
| `js/telidon/naplps.js` | `ofxNaplps` — `NapDecoder`/`NapEncoder`, already a port of it |
| `js/telidon/TelidonP5.js` | `ofxNaplps` — `Telidon` |
| `js/net/client.js` | `NapClient.h/.cpp` |
| `js/drawing/drawing.js` | `DrawingMode.h/.cpp` |
| `js/drawing/controller.js` | `Controller.h/.cpp` |
| `js/drawing/tools.js` | `Tools.h/.cpp` — `Stroke`, `Frame` |
| `js/drawing/palette.js` | `Palette.h/.cpp` |
| `js/drawing/worldscale.js` | `WorldScale.h/.cpp` |
| `js/drawing/mouse.js` | `MouseController.h/.cpp` |
| `index.html` (canvas, slideshow, keys) | `ofApp.h/.cpp` |
| `js/tezos/tezos.js` (Beacon wallet) | *not ported* — see **Minting** |
| MediaPipe Tasks (WASM) | `ofxMediaPipe` |
| Three.js `Object3D` | `ofNode`, plus `NodeUtils.h` for the gaps |
| `<video>` element | `VideoSource` (from `ofxMediaPipe`'s example app) |

The one piece with no analogue in either tree is `NodeUtils.h`: Three's
`worldToLocal`/`localToWorld`, a world-space scale setter to match ofNode's
existing world-space position and orientation setters, and `rdpSimplify` — the
Ramer-Douglas-Peucker pass lifted from `index.html`, which is what keeps a
hand-drawn stroke inside the ~30 KB a Tezos token can hold.

## The two network directions

This app is a client *and* a server at once, which the browser version never is.

**Outbound (`NapClient`)** — the port of `client.js`. It connects to
`nap-xtz-server`, receives every drawing the server broadcasts, publishes the
ones made here, and reaches the chain through the server's REST API.

The browser speaks socket.io. This speaks the plain `ws` server the backend runs
alongside it (`PORT_WS`, default 4321), which accepts the same three message
types — `naplps`, `rpi_naplps`, `rpi_command` — and broadcasts the same JSON
back. Implementing the socket.io handshake in C++ would buy nothing.

It runs on one worker thread that both sends and receives, because Poco's
`WebSocket` is not safe to use from two threads at once; the receive has a short
timeout so the same loop can drain the send queue and notice a shutdown.
Reconnection is automatic, so the server does not have to be up first.

**Inbound (`ofxHTTP::SimpleWebSocketServer`, port 7112)** — unchanged from the
app this was before the port. `nap-xtz-server` opens a websocket *to* this app
and pushes drawings as its own canvas draws them; that is the Pinopticon "Pi"
role, and it still works. Both directions are live together.



## Live drawing

`DrawingMode` owns a `VideoSource`, an `ofxMediaPipe::Tracker`, an `ofCamera`,
and the scene: a `worldNode` with a `Frame` of strokes parented to it, two
`Controller`s, their `Palette`s, and a `MouseController`.

Inference does not run in `update()`. On a Pi the gesture model costs ~240 ms a
pass against a 16 ms frame budget, so `Tracker` runs it on a worker thread and
drops frames rather than queueing them; `update()` takes whatever result is
ready. Pose landmarking is disabled — it would roughly double the cost of a pass
for nothing this mode uses.

The gesture vocabulary is unchanged from the JS:

| Gesture | Acts as | Does |
| --- | --- | --- |
| `Pointing_Up` | trigger | draw a stroke |
| `Closed_Fist` | grip | grab the world; hold 1.6 s to open the palette |
| `Open_Palm` | — | release everything |
| `Thumb_Down` | button B | hold 2 s: one hand undo, two hands clear |
| `Thumb_Up` | button A | hold 2 s: one hand recentre, two hands exit |
| `Victory` | button C | both hands: instant full reset |

Two things make a classifier usable as a set of buttons, and both are ported
rather than reinvented. **Buttons latch**: a fist closes the grip and only an
open palm (or a one-second timeout) opens it, so the drawing is not dropped when
the label flickers mid-grab. **Motion gates input**: `Controller` scores its own
confidence from the acceleration of the tracked point, with hysteresis, and
ignores button presses while a hand is jittering. On top of that every
destructive action is a timed hold with a shrinking on-screen circle, so a
misfire is visible before it is irreversible.

`Controller` filters position twice, at different strengths: lightly for
drawing, where lag is felt immediately, and heavily for navigation, since a
jittery grab would shake the whole drawing. `WorldScale` reads only the
navigation position.

### Leaving drawing mode is the point

`stop()` projects every stroke through the same camera the user was looking
through and encodes the result with `NapEncoder`. Because NAPLPS has no line
thickness, a stroke is encoded as filled polygons rather than as its centreline
— `Stroke::toBrushQuads()`, one short quad per segment. It is not the ribbon
mesh that is drawn on screen, and the difference matters twice over. A quad
built on one segment's own perpendicular is convex, so every renderer fills it
alike whichever winding rule it uses, where a single long outline of the whole
stroke crosses itself at every tight turn and fills differently on each. And
every point in a polygon after the first is a *delta* from the one before it, so
ending the polygon after four points resets that running cursor before the
encoder's rounding error can grow into drift. The width is measured across the
view rather than in the stroke's own plane, which is what keeps a stroke drawn
towards the camera from arriving as a hairline. `ofApp` then shows the result on
the NAPLPS canvas and publishes it. That round trip — hands to vectors to every
other client — is what the app is for.

The vertical squeeze in `convertToNaplps()` is not arbitrary: the canvas renders
NAPLPS into a square space, so the 4:3 view is compressed by 480/640 and pushed
down by the remainder, matching the convention the browser's SVG importer uses.

## Minting

`tezos.js` signs with a Beacon wallet in the browser. There is no Beacon for
C++, so this app has no wallet and no keys. `m` instead asks the server to mint
with its own key via `POST /api/tezos/mint`, which answers with a clear error
when `TEZOS_SECRET_KEY` is not configured there. Everything else about the chain — except reading it, which this app now polls
TzKT for directly in a background thread — stays where it already was, on the
server.

## Rendering

Three.js is retained-mode: build a mesh, add it to a scene, let the renderer
walk it. openFrameworks is immediate-mode, so each object draws itself and the
scene graph is only used for transforms. `Frame::draw()` multiplies in the world
node's global matrix and draws each stroke's mesh; `Stroke` caches that mesh and
rebuilds it only when its points move, rather than the browser's approach of
rebuilding all geometry on every change.

The NAPLPS canvas keeps the FBO caching the earlier player had: a finished
drawing is static, so the FBO is only redrawn while the progressive draw is
still running or something marks it dirty.

### The canvas geometry

`ofApp::updateLayout()` originally reproduced the browser's dynamic scaling, but now uses fixed dimensions from `settings.xml` (`fbo_width` and `fbo_height`):

| Browser | Here |
| --- | --- |
| `scaleFactor = min(windowWidth/640, windowHeight/480)` | None (fixed `fboWidth` / `fboHeight`) |
| `createCanvas(640*sf, 480*sf)`, centred by `#main-canvas` CSS | `canvasSize`, `canvasOffset` |
| `scale(sf); translate(0, sH - sW)` | `drawSize = fboWidth`, `drawOffset.y = fboHeight - fboWidth` |

The artwork is rendered into a **square** as wide as the canvas and then pushed
up by the quarter that overhangs it, so what shows is the bottom three quarters
of that square. That is the same convention `convertToNaplps()` encodes to and
the browser's SVG importer writes (`y/sH*0.75 + 0.25`); a drawing round-trips
through hands, encoder and canvas at the size it was made.

The FBO is allocated at `fboWidth` x `fboHeight`.

### The empty state

With nothing loaded the canvas is black with `\\ DRAG ' n ' DROP //` across the
middle, in Telidon-Bold at the browser's 36px scaled to the window — the same
placeholder `index.html`'s `draw()` shows when `telidon` is empty. It is what is
on screen at startup while the chain read is out, and on entering live drawing.

## Keys

| Key | | Browser equivalent |
| --- | --- | --- |
| `d` | enter/leave live drawing | Live Drawing button |
| `s` | slideshow | "slideshow" link |
| `c` | canvas: publish drawing to every client; drawing mode: switch camera | — |
| `m` | mint the current drawing (server-side signing) | Mint to Tezos |
| arrows | next/previous sample file | — |
| `space` | redraw | — |
| `p` / `l` | progressive draw / label points | — |
| `i` | info overlay | — |
| `f` | fullscreen | — |

The Tezos chain read runs on a background thread that polls TzKT directly. During
slideshow mode, the player alternates between local files and chain drawings.

Additionally, the app uses a **Dead Man's Switch**: when the network goes quiet
for `slideTimeout` ms, the app automatically falls back to local `.nap` files
and shuffles them. The first network drawing to arrive takes the screen back.
A player on a wall with no reachable server or network activity will still
have something to show.

In drawing mode: `WASD` moves, `alt`+drag orbits, `alt`+`shift`+drag pans,
wheel zooms; the mouse draws with the left button and opens the palette with the
right, for working without a camera. `c` there swaps the Pi's ribbon camera for a
USB webcam and back — a different key from the canvas's `c`, because the camera
only exists in this mode and the publish action only exists in the other.

`VideoSource::switchTo()` is what makes that safe. It closes the old source
first, since a CSI pipe and a webcam can both be holding the same sensor, and
puts it back if the requested one will not open, so asking for a camera that
isn't there costs nothing; if the old one will not reopen either it re-probes
the normal order, which ends at Synthetic. A camera that dies *while* running is
the other half of it: a short read on the rpicam pipe (the camera unplugged, or
rpicam-vid never installed — neither is detectable at `popen()` time, because
the shell starts either way) falls through the same order with CSI excluded,
which is what stops it looping.

Either outcome is named in the drawing-mode HUD for four seconds. A refused
switch leaves the picture exactly as it was, which on its own reads as the key
not working rather than as the camera not being there.

## Where this deliberately differs

Four places do not copy the JS line for line, and all four are visible to the
user:

**The hand's depth.** `drawing.js` sizes the plane the hands move on from
`camera.position.z`, which is only the viewing distance while the camera sits on
the z axis — orbit a quarter turn and it collapses to zero, taking the hand
tracking with it. `updateHand()` uses the orbit radius, which is what that
expression was standing in for and survives orbiting.

**Clearing blinks the drawing.** `Frame.clearWithFlicker()` in the JS flickers a
`lineMesh` that nothing ever populates, so a clear blinks nothing and then
everything vanishes. Here the drawing itself blinks, which is what the shrinking
red circles have been announcing for two seconds.

**The palette suppresses drawing.** While a palette is open the hand is picking a
colour, so `updateDrawing()` skips that controller. In the JS a `Pointing_Up`
during the 1 s grip latch would draw a stroke through the open palette.

**A gesture floor.** The tracker reports `None` below a score of 0.3 rather than
naming its best guess. The browser takes the top category unconditionally, which
is a worse trade when a misread fires an undo.

The camera pip in the corner has no browser counterpart either — the browser
hides its `<video>` element outright, but with no DOM to fall back on this is
the only way to see why a hand isn't being picked up.

## Configuration

Configuration is read at startup from two files in `bin/data/`:
* `settings.json`: `server_host`, `server_ws_port` (4321), `server_http_port` (8080).
* `settings.xml`: `slide_timeout`, `slide_interval`, `fbo_width`, `fbo_height`, `debug_view`, `shader_name`, `tezos_contract`, `tzkt_base`, `tezos_poll_seconds`, `tezos_max_bytes`.

Defaults apply when a file or setting is absent.

## Addons

`ofxMediaPipe`, `ofxNaplps`, `ofxHTTP`, `ofxIO`, `ofxMediaType`,
`ofxNetworkUtils`, `ofxPoco`, `ofxSSLManager`, `ofxJSON`, `ofxCrypto`.

`bin/` must contain `libmediapipe_tasks_vision.so` (linked by `ofxMediaPipe` and
found at runtime through the `$ORIGIN` rpath in `config.make`), and `bin/data/`
must contain `gesture_recognizer.task`.

## Build status

**Builds and runs on this machine.** The five compile fixes from `REPORT_2.md`
are applied, and the Poco link flags sit in this project's `config.make` rather
than in `ofxPoco/addon_config.mk`, so an openFrameworks reinstall cannot quietly
undo them. The fixes themselves live in shared trees and are *not* carried by
this repo:

| Where | What |
| --- | --- |
| `ofxIO` `ThreadsafeLoggerChannel.{h,cpp}` | dropped the two `log()` overrides OF no longer declares |
| `ofxHTTP` `BaseRoute.h` | added `<queue>` and `<mutex>` |
| `ofxHTTP` `PostRoute.cpp`, `ofxIO` `JSONUtils.cpp` | `std::filesystem::extension(p)` becomes `path(p).extension()` |
| `ofxCrypto` `ofxCrypto.cpp` | qualified the bare `ostringstream`/`istringstream`/`stringstream` |
| openFrameworks `types/ofTypes.h` | added `<memory>` |

The first four are in `n1ckfg` forks and can go upstream. The `ofTypes.h` one is
OF's own code and will need reapplying after a reinstall.

Both network directions are verified against a real `ws` server. The app
completes the handshake, receives a broadcast drawing and decodes it — a sent
`wast.nap` arrives as 206 commands, distinct from the 173-command startup
sample — and `n` publishes the same 11215 bytes back unchanged, so the NAPLPS
survives the JSON round trip intact. MediaPipe loads its gesture model, and
`VideoSource` falls back to a synthetic feed when it finds no camera, which is
what makes the app testable headless under Xvfb.

The camera switch is verified only on its failure path: this machine has no
camera attached — `rpicam-hello` reports none and every `/dev/video*` is a codec
or ISP node — so pressing `c` twice under Xvfb exercised "requested camera
absent, keep what works" both times and the app carried on from the synthetic
feed. **The CSI↔webcam swap itself is untested against hardware.** Anyone with
both cameras plugged in should confirm two things the failure path cannot show:
that the pip picture actually changes, and that switching back to CSI succeeds
after `rpicam-vid` has once been closed and its sensor released.
