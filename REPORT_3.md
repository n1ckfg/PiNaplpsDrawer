# Build and Verification: nap-xtz-client on aarch64

**System:** Raspberry Pi, Debian 12 (bookworm), Linux 6.12.96 aarch64, GCC 12,
openFrameworks 0.12.1 (`of_v0.12.1_linuxaarch64_release`)

Follow-up to `REPORT_2.md`, which enumerated the build blockers but left them
unapplied. This report covers applying them, the first successful build on this
machine, and an end-to-end check of both network directions.

## Summary

The port was already code-complete in `src/`. What stood between it and a
working app were the five compile fixes from `REPORT_2.md`, all of which live in
shared trees rather than in this project. With those applied the app builds,
links, runs, and exchanges drawings with a real `ws` server in both directions.

Section A of `REPORT_2.md` — the missing `linuxaarch64` section in
`ofxPoco/addon_config.mk` — needed no work. The recommended alternative was
already in place: this project's `config.make` carries the Poco link flags plus
the `$ORIGIN` rpath, which is also the form that survives an openFrameworks
reinstall.

## Fixes applied

| Where | What |
| --- | --- |
| `ofxIO` `libs/ofxIO/include/ofx/IO/ThreadsafeLoggerChannel.h` + `src/ThreadsafeLoggerChannel.cpp` | removed the two `log()` overrides (varargs and `va_list`) that OF no longer declares, declarations and definitions both |
| `ofxHTTP` `libs/ofxHTTP/include/ofx/HTTP/BaseRoute.h` | added `#include <queue>` and `#include <mutex>` |
| `ofxHTTP` `libs/ofxHTTP/src/PostRoute.cpp` | `std::filesystem::extension(p)` becomes `std::filesystem::path(p).extension().string()` |
| `ofxIO` `libs/ofxIO/src/JSONUtils.cpp` (2 sites) | same change, as `path(filename).extension()` |
| `ofxCrypto` `src/ofxCrypto.cpp` (4 sites) + `src/ofxCrypto.h` | qualified the bare `ostringstream`/`istringstream`/`stringstream`; added `<sstream>` to make the dependency explicit |
| openFrameworks `libs/openFrameworks/types/ofTypes.h` | added `#include <memory>` |

The `ofxIO`, `ofxHTTP`, and `ofxCrypto` trees are `n1ckfg` forks, so those four
fixes can land upstream. The `ofTypes.h` fix is in openFrameworks' own code, and
the OF root is not a git repository — that one is untracked and will need
reapplying after any OF reinstall.

## Build result

`make -j4 Release` completes clean: 148 translation units compiled, no errors,
and the link succeeds.

```
bin/nap-xtz-client: ELF 64-bit LSB executable, ARM aarch64, dynamically linked
6117384 bytes
```

`ldd` resolves every library, `libmediapipe_tasks_vision.so` included — it is
found through the `$ORIGIN` rpath, so the binary runs from `bin/` without
`LD_LIBRARY_PATH`.

## Runtime verification

The app has no display on this machine, so it was driven headless under Xvfb.
Startup is clean end to end:

- `ofxNaplps` decodes the startup sample (Telidon 709, 173 commands)
- the inbound `ofxHTTP::SimpleWebSocketServer` starts on port 7112
- `VideoSource` finds no camera and falls back to its synthetic feed, which is
  what makes headless testing possible at all
- `ofxMediaPipe::GestureRecognizer` loads `gesture_recognizer.task` and reports
  ready for 2 hands

### Both network directions, against a real `ws` server

Checked against Node's `ws` — the same implementation `nap-xtz-server` runs —
installed into a scratch directory, not into the project.

**Receive.** The server pushes a drawing on connect, as `app.js` does when it
holds a `latestToken`. `NapClient` completes the handshake, and the frame is
parsed and decoded. Sending `wast.nap` rather than the startup sample makes the
result unambiguous: it arrives as **206 commands**, distinct from the
173-command file already on screen, so the drawing on the canvas is genuinely
the one that came off the wire.

**Send.** With that drawing loaded, `n` publishes it back as
`{type: "naplps", source: "client"}`. The server logs **11215 bytes received**
against **11215 bytes sent** — byte-identical, so the NAPLPS stream survives the
JSON round trip intact, which is the property the `\u00xx` escaping in
`makeFrame()` exists to guarantee.

## A correction worth recording

Partway through this work a hand-rolled Python websocket server was used as a
stand-in for the backend. Poco refused its handshake, and the evidence initially
looked like a genuine interop bug: Poco appeared to validate
`Sec-WebSocket-Accept` against a nonstandard GUID, which would have meant
`NapClient` could never talk to a standards-compliant server and would have
justified rewriting the client's framing by hand.

That conclusion was wrong. `258EAFA5-E914-47DA-95CA-C5AB0DC85B11` **is** the
RFC 6455 GUID; the test harness had it wrong. Testing against real Node `ws`
settled it — `ws` computes the same value Poco does, and the handshake succeeds.
Poco, `ofxHTTP`, and `NapClient` were all correct throughout, and no code
changed as a result.

The lesson for anyone debugging this layer again: check against a real
implementation before concluding the library is at fault. `ofxHTTP`'s own server
on port 7112 is a Poco peer and makes a convenient first control; Node `ws` is
the one that matters, since it is what the backend actually runs.

## Notes and caveats

- **Nothing is committed.** Changes sit across four working trees: this repo
  (documentation only), plus `ofxIO`, `ofxHTTP`, and `ofxCrypto`. The
  `ofTypes.h` change is in the unversioned OF root.
- **`xdotool` was installed** (`sudo apt-get install xdotool`) to inject the `n`
  keypress into the Xvfb display. It is a test dependency only, and nothing in
  the project references it; `sudo apt remove xdotool` reverses it.
- **Not covered here:** the chain paths. `c` (`GET /api/tezos/latest`) and `m`
  (`POST /api/tezos/mint`) both need the Node backend's HTTP API, whose
  dependencies are not installed in `nap-xtz-server/`. Minting additionally
  needs `TEZOS_SECRET_KEY` configured server-side.
- **Still deliberately unported:** `tezos.js`. There is no Beacon SDK for C++,
  so this app holds no wallet and no keys, and `m` asks the server to sign
  instead. That is the design, not an omission.
- The runtime performance items in `REPORT.md` were written against the earlier
  `PiNaplpsPlayer`. The FBO caching suggestion is already implemented; the rest
  are worth revisiting now that the app runs, but were not evaluated here, since
  Xvfb's software renderer says nothing useful about frame cost on real Pi GPU
  hardware.

## Method

Each fix was applied in turn and the build rerun, rather than patched
speculatively — the compile blockers are sequential and each hides the next. The
binary was then run under Xvfb against a Node `ws` server, with a TCP tee proxy
used to capture the handshake bytes on the wire while the harness problem above
was being resolved.
