# Build Failure Analysis: nap-xtz-client on aarch64

**System:** Raspberry Pi, Debian 12 (bookworm), Linux 6.12.96 aarch64, GCC 12, openFrameworks 0.12.1 (`of_v0.12.1_linuxaarch64_release`)

## Status

**Resolved.** All five compile fixes below and the Poco link flags are applied, and the project now builds, links, and runs on this machine. See the build-status section of `ARCHITECTURE.md` for where each fix landed and what survives an openFrameworks reinstall.

## Summary

The client has never been built on this machine — there is no `obj/` tree and no binary. Running the build and probing past each failure in turn shows that the project's **own code is fine**: `src/ofApp.cpp` and `src/main.cpp` compile clean once the addons are fixed. Every blocker lives in the vendored addons plus one openFrameworks core header.

There are two independent problem classes:

- **A.** One genuinely aarch64-specific failure, at the **link** stage.
- **B.** Five **compile** failures caused by OF 0.12.1 / GCC 12 / C++20 drift, which would hit any modern Linux.

The compile blockers come first and are sequential — each hides the next.

---

## A. The aarch64-specific blocker (link stage)

`addons/ofxPoco/addon_config.mk` defines platform sections for `linux64`, `linuxarmv6l`, `linuxarmv7l`, `msys2`, and `vs` — but **not `linuxaarch64`**.

openFrameworks matches those section names against `ABI_LIB_SUBPATH` (see `libs/openFrameworksCompiled/project/makefileCommon/config.addons.mk:127`). On this machine `make -p` reports:

```
PLATFORM_LIB_SUBPATH = linuxaarch64
ABI_LIB_SUBPATH      = $(PLATFORM_LIB_SUBPATH)
PLATFORM_ALTERNATIVE := void
```

Nothing matches, so ofxPoco contributes **zero** `ADDON_LDFLAGS`. Inspecting the emitted link line confirms it contains no `-lPoco*`, no `-lssl`, and no `-lcrypto`. The link is guaranteed to fail with undefined Poco symbols once compilation succeeds.

This is why the project built on 32-bit Pi OS (where `linuxarmv7l` matched) and does not on 64-bit Pi OS.

Poco itself is not the problem. `libpoco-dev` 1.11.0-3+deb12u1 is installed, headers resolve from `/usr/include/Poco`, and all nine required libraries link successfully when tested directly:

```
PocoNetSSL PocoNet PocoCrypto PocoUtil PocoXML PocoJSON PocoFoundation crypto ssl   -> all OK
```

Note that `libs/poco/` does not exist under the OF root, so ofxPoco's `ADDON_INCLUDES = libs/poco/include` is a no-op — headers come from the system package instead. That part works fine; only the link flags are missing.

### Fix

Add a `linuxaarch64:` section to `ofxPoco/addon_config.mk` mirroring the `linuxarmv7l` block:

```make
linuxaarch64:
	ADDON_LDFLAGS = -lPocoNetSSL
	ADDON_LDFLAGS += -lPocoNet
	ADDON_LDFLAGS += -lPocoCrypto
	ADDON_LDFLAGS += -lPocoUtil
	ADDON_LDFLAGS += -lPocoJSON
	ADDON_LDFLAGS += -lPocoXML
	ADDON_LDFLAGS += -lPocoFoundation
	ADDON_LDFLAGS += -lcrypto
	ADDON_LDFLAGS += -lssl
```

**Preferred alternative:** ofxPoco ships with openFrameworks, so an OF reinstall would wipe that edit. Putting the flags in this project's own `config.make` instead makes the fix survive:

```make
PROJECT_LDFLAGS = -lPocoNetSSL -lPocoNet -lPocoCrypto -lPocoUtil -lPocoJSON -lPocoXML -lPocoFoundation -lcrypto -lssl
```

---

## B. Compile blockers (OF 0.12.1 / GCC 12 / C++20 drift)

Not architecture-specific. `ofxIO` is pinned to a 2019 commit (`9b55d4b`) written against OF 0.10; `ofxHTTP` is at `5191321` (2023).

### 1. `ofxIO` — stale `override` on removed virtuals

**File:** `addons/ofxIO/libs/ofxIO/include/ofx/IO/ThreadsafeLoggerChannel.h:67,71`

```
error: 'virtual void ofx::IO::BaseThreadsafeLoggerChannel::log(ofLogLevel, const std::string&, const char*, ...)'
       marked 'override', but does not override
```

OF removed the varargs and `va_list` virtuals from `ofBaseLoggerChannel`. As of `libs/openFrameworks/utils/ofLog.h:685` the base class has exactly one pure virtual `log(level, module, message)` plus a non-virtual variadic template that forwards to it.

**Fix:** delete both stale declarations *and* their definitions at `ThreadsafeLoggerChannel.cpp:94` and `:105`. The base template already handles printf-style calls. (Merely dropping the `override` keyword compiles, but leaves two redundant virtuals that hide the base template.)

This header is pulled in transitively and is not otherwise used by this project:
`ofApp.h` -> `Pinopticon_Http.hpp` -> `ofxHTTP.h` -> `ofxIO.h:44` -> `ThreadsafeLoggerChannel.h`

### 2. `ofxHTTP` — missing standard includes

**File:** `addons/ofxHTTP/libs/ofxHTTP/include/ofx/HTTP/BaseRoute.h:575,578`

```
error: 'queue' in namespace 'std' does not name a template type
error: '_frameQueue' was not declared in this scope
```

The header uses `std::queue` and `std::mutex` but includes only `<set>` and `<string>`. It relied on transitive includes that older libstdc++ happened to provide.

**Fix:** add `#include <queue>` and `#include <mutex>`.

### 3. openFrameworks core — missing `<memory>`

**File:** `libs/openFrameworks/types/ofTypes.h:87`

```
error: 'shared_ptr' in namespace 'std' does not name a template type
```

`ofTypes.h` defines `template <typename T> using ofPtr = std::shared_ptr<T>;` but includes only `<mutex>`. Most translation units reach it after `ofMain.h` has already pulled in `<memory>`; ofxHTTP's `OAuth10RequestFilter.cpp` include order does not.

**Fix:** add `#include <memory>` to `ofTypes.h`. This is an OF 0.12.1 core bug.

### 4. `std::filesystem::extension()` does not exist

Two addons call a Boost.Filesystem free function that has no `std::filesystem` equivalent — the standard uses the member `path::extension()`.

- `addons/ofxHTTP/libs/ofxHTTP/src/PostRoute.cpp:311`
  `p += std::filesystem::extension(originalFilename);`
  -> `p += originalFilename.extension().string();`

- `addons/ofxIO/libs/ofxIO/src/JSONUtils.cpp:26` and `:66`
  `if (std::filesystem::extension(filename) == ".gz")`
  -> `if (filename.extension() == ".gz")`

### 5. `ofxCrypto` — unqualified stream types

**File:** `addons/ofxCrypto/src/ofxCrypto.cpp:58,67,68,87`

```
error: 'ostringstream' was not declared in this scope; did you mean 'std::ostringstream'?
```

`ofxCrypto.h` has no `using namespace std;` (only seven `using Poco::...` declarations). Four sites use bare `ostringstream`, `istringstream`, and `stringstream`.

**Fix:** qualify them with `std::`. GCC's "did you mean" confirms `std::ostringstream` is already visible, so no extra include is strictly required, though adding `#include <sstream>` makes the dependency explicit.

---

## Additional notes

- **Addon ownership:** `setup.sh` clones ofxNaplps, ofxHTTP, ofxIO, ofxMediaType, ofxNetworkUtils, ofxSSLManager, ofxJSON, and ofxCrypto from `github.com/n1ckfg/*` forks, so fixes 1, 2, 4, and 5 can land upstream rather than as local patches. Fix 3 (OF core) and the ofxPoco link flags are in OF-bundled code — use the `config.make` approach or document a post-install patch step.
- **ofxNaplps** also lacks a `linuxaarch64` section, but all its platform sections are empty, so this is harmless.
- **All nine required addons are present** in `addons/`, and the OF core library (`libopenFrameworks.a`, 10 MB) is already built for `linuxaarch64`. The OF install itself is healthy.

## Suggested order of work

1. Apply compile fixes 1-5 to reach a clean compile.
2. Add the Poco link flags (section A) to reach a successful link.
3. Build and run; only then evaluate the runtime performance items in `REPORT.md`.

## Method

Findings were established by running the actual build, then iteratively applying temporary patches to reveal each subsequent failure, and finally sweeping every addon source file with `g++ -fsyntax-only` using the project's real compile flags to enumerate the remainder in one pass. All temporary patches were reverted; `git status` was verified clean in `ofxIO`, `ofxHTTP`, and `nap-xtz-server`.
