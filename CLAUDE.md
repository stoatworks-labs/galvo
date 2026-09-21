# galvo

An ILDA laser projector as an FFGL **effect** (`GV01`) for Resolume
Arena/Avenue: the clip's outlines traced into contours, scanned by two
galvanometer mirrors at a fixed point rate, and lit by where the beam actually
went. C++17 + GLSL 4.10, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT.

Read `AGENTS.md` before changing the tracer, the point stream, the scanner or
the beam renderer.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/gvtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Galvo Speed=12" --set "Corner Dwell=6"`
- List parameters, with type and range: `./build/gvtest --list`
- The test card alone: `./build/gvtest --card /tmp/card.png`
- Film a clip through it: `... | ./build/gvtest --pipe --size 1920x1080 [--script cues.txt] | ...`
  — raw RGBA frames on stdin, raw RGBA frames on stdout, the fleet's format, so
  one filming script drives any of the plugins. A `--script` line is
  `frame  Parameter Name  value`, in that parameter's own units, held before
  the first key and interpolated between. **A reel has to be filmed from its
  first frame**: the stabilise pass, the accumulation buffer and the scanner's
  cursor all carry across frames, so there is no seeking.

An **integer** parameter is set in its own units — `Galvo Speed=12` is 12 kpps,
`Trace Size=480` is 480 pixels. Everything else is 0..1.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check + oxbow)
- The demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`
- The contour tracer: `./build/gvtest --trace`
- The galvo is a second-order system: `./build/gvtest --step`
- The point budget and the crawl: `./build/gvtest --budget`
- Light does not depend on the galvo: `./build/gvtest --energy`
- Corners are brighter by what dwell predicts: `./build/gvtest --dwell`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/gvtest --bench`

`--step`, `--budget` and the synthetic half of `--trace` need **no GPU** — the
whole CPU half of this plugin is plain C++ — which is why CI runs them.

## Notes
- **Nothing is drawn.** The dwell dots, the rounded corners, the hooks and
  tails, the flicker of a busy frame: all of it falls out of a second-order
  mirror following a point stream at a fixed rate. If you are tempted to draw
  one of them, the chain is wrong somewhere and that is the bug.
- **`1/v` is never computed.** Energy per sample interval is spread over the
  distance covered; the dwell law falls out. Do not "simplify" it.
- **`dt` lives in the sample**, not in a block-wide rate. That is what makes
  the light independent of the point rate as an identity rather than a tuning.
- The CPU half (`Tracer`, `PointStream`, `Scanner`) has **no GL in it at all**.
  Keep it that way: it is what makes the physics testable without a GPU.
- **One synchronous `glReadPixels` per frame**, of the small mask. It is a
  pipeline stall and it is the single biggest cost in the plugin — about 0.4 ms
  at 720p and 0.9 ms at 4K. `--bench` reports it separately for that reason.
- A **closed contour's first vertex is emitted once**, at the arrival. Emitting
  it at both ends made one corner of every square 19% brighter than the other
  three, and `--dwell` measured it.
- **Colour Mode = Clip reads the clip blurred**, at a mip level matched to the
  spot. A contour runs along a boundary, which is the one place a point sample
  is a coin flip between the two sides.
- `sample`, `half`, `layout`, `filter`, `input`, `output`, `common`, `active`,
  `patch`, `flat` are GLSL reserved words. A shader error surfaces only at
  runtime, in the diagnostics log, as "the effect does nothing".
- **`ScopedFBOBinding` restores the framebuffer and NOT the viewport**; every
  `ffglex::Scoped*` binding clears to 0 rather than restoring. Allocate every
  buffer before anything binds a texture.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it. `FF_TYPE_INTEGER` is exempt, which is why Galvo Speed is in kpps.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `galvo_core` is an **OBJECT** library, not STATIC — the plugin registers
  itself from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `GV01`. Display name `SW Galvo` (16 characters is the limit).

## Windows
- The x64 `.dll` is **cross-compiled in the Parallels guest** on this Mac (ARM64
  Windows 11, MSVC 2022 Build Tools, `cmake -A x64`, vcpkg triplet
  `x64-windows-static-md`) — the route the fleet's
  `~/Projects/resolume/winbuild` scripts take. No x64 Windows machine builds it.
- Check it with `dumpbin /EXPORTS` for `plugMain`. The 2026-09-21 build was
  404,992 bytes.
- Tested on **win-lab** (x64 Windows 11 Pro, no GPU, Mesa llvmpipe beside
  Arena). **Start Arena through the session-1 scheduled-task wrapper** — an ssh
  session lands on the service window station and has no desktop. Instantiate
  from Arena's own effects browser: the REST add-effect endpoint returns 200
  and adds nothing. See `AGENTS.md`.

## Not done yet
- **Never run on a GPU in Resolume.** Arena 7.27.1 on Windows registers, loads
  and instantiates it, and the shaders compile — on llvmpipe, a software
  rasteriser. Nothing was timed there.
- **Never instantiated in Arena on macOS.** Here it is `oxbow` only.
- No user guide, no OpenFX port, no video.
- **The browser demo's CPU half is a port and nothing checks it.**
  `demo/plugin.js` re-implements `Tracer`, `PointStream`, `Scanner` and
  `Controls` in JavaScript, because without them the page has nothing to show.
  `check_shaders.py` covers the GLSL only. Change any of those four files and
  the page has to be changed by hand to match.
- `ATTRIBUTIONS.md` is still a provisional hand copy — `sync-attributions.py`
  does not know this repo. `source/StoatworksAbout.h` is generated by
  `sync-about.py` now; do not hand-edit it.
- No Plotter mode (the spec's optional second look).

## Browser demo

`demo/` is the page at **galvo-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` in this repo with `cf-run npx wrangler deploy` — no build step;
what is committed is what is served. `demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh galvo` and
is not a place to edit.

Verify a deploy **by content, never by status code** — a stale page answers a
cheerful 200:

    curl -s 'https://galvo-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere.

    ~/Library/Logs/galvo/galvo.YYYY-MM-DD.log       macOS
    %LOCALAPPDATA%\galvo\galvo.YYYY-MM-DD.log       Windows

On Windows that log is the **proof of instantiation** — `plugin loaded build=…`,
then the `GL vendor=…` line and `initialised` — because Arena's REST API will
not show you an effect applied to the composition.
