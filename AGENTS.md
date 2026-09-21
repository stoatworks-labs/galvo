# galvo — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that traces the
outlines in a clip and scans them the way an ILDA laser projector does. C++17 +
GLSL 4.10, CMake, universal macOS `.bundle` and a Windows `.dll`. MIT,
intended home `github.com/stoatworks-labs/galvo`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the tracer, the point stream, the scanner or the
beam renderer.

---

## The one idea

**A laser show is a path scanned by two mirrors with a finite point budget.**
The picture is where the beam went, at the brightness its dwell time gave it,
and nothing else is drawn.

That is the whole plugin, and every artefact anybody recognises falls out of it
rather than having been arranged:

- **Corners have bright dots** because the point stream repeats a sharp vertex
  several times over. The beam is on and it is not going anywhere, so the light
  piles up in one place.
- **Corners round off at speed** because a galvo is a mass on a spring and
  cannot change direction in zero time. Lower the Galvo Speed and the rounding
  gets worse, because the natural frequency comes down with it.
- **An under-damped scanner overshoots and rings** past a corner, which is what
  Damping under 1 does and why it is a control rather than a constant.
- **Hooks and tails** appear when Blanking Delay is mis-set: the beam comes on
  before the mirrors have arrived, so it draws the tail of the approach.
- **A busy frame flickers and crawls.** The scanner draws `pps / fps` points
  per host frame and continues where it left off, so a frame needing more than
  that is drawn across several output frames.

If you are ever tempted to draw one of those directly, stop: either it already
falls out of the chain, or the chain is wrong somewhere and that is the bug.

The rule is made arithmetic in two places and both are load-bearing.

**In the renderer, `1/v` is never computed.** A fixed quantum of energy is
deposited per *sample interval* and spread over whatever screen distance the
mirrors covered in that interval. Brightness proportional to dwell time is then
what "equal energy per unit time" *means*, not a term applied on top of
something else. The fragment shader evaluates the exact convolution of a
uniform segment with a Gaussian spot, which conserves energy for any length and
any spot size and stays finite when the beam stops — with no clamp and no
divide-by-zero guard anywhere. (This is vectrix's renderer, and the same
lineage of code.)

**In the scanner, `dt` is carried per sample.** That is what makes the light in
a frame independent of the point rate as an *identity* rather than a
calibration: the total is `BeamPower × beam-on time`, in which the number of
samples does not appear. `gvtest --energy` measures it across a 100:1 range of
galvo bandwidths and an 8:1 range of point rates.

### What falls out, and what does not

The honest limit of this release is that **the tracer is a tracer, not a
vectoriser**. It thins a thresholded edge mask and walks it, so what it finds
is the *skeleton of the edge band*, not the artwork's own outline:

- On logos, titles and line art it finds a small number of long clean contours
  and the result reads exactly like a laser drawing.
- On real footage it finds a mess of short strokes and the picture becomes a
  scribble. That is a property of the problem, not a bug to be tuned out, and
  the README says so.
- A junction is walked as whichever arm the neighbour order reached first, so a
  letter with a crossing (an X, a K) can come out as two contours that share a
  point rather than one path through it.

`Min Length` and `Simplify` are the controls that make the difference between
those two regimes, and `Background = Edge Mask` is how you actually set them.

---

## The traps

Ordered by how much time they will cost you.

**A closed contour's first vertex must be emitted ONCE.** It is the only vertex
the beam both departs from and arrives back at, and emitting it at both ends
gives it one lit point more than every other corner. The symptom is subtle and
the diagnosis is not: `gvtest --dwell` printed the four corners of a square as
`2.673 2.244 2.244 2.255` — one corner 19% brighter than the other three, from
a perfectly symmetrical input. It is emitted at the **arrival**, because that
is where the mirror is actually decelerating. `PointStream.cpp` says so at the
one `if` that does it.

**`ScopedFBOBinding` does not restore the viewport.** It restores the
framebuffer binding and only that (SDK `b1afaf9`). So every pass's
`ResizeViewPort()` leaks into the pass after it, and the composite — which
draws to the host's framebuffer and so has no buffer of its own to size itself
from — inherits whatever the last pass left. Here that would be the *trace-size*
stabilise buffer, so the whole effect would render into a 320-pixel corner.
`ProcessOpenGL` captures the host viewport up front and restores it before the
composite for that reason.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its new colour texture under one of
those, so *allocating a buffer silently unbinds your input texture from the
active unit*. The symptom is the dangerous part: correct on every frame except
the one that allocates. Every `Ensure()` in `ProcessOpenGL` therefore happens
before anything binds a texture.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second
time where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes
it first. It matters here rather than being pedantry: the trace buffers
reallocate whenever `Trace Size` changes, and an operator drags that.

**Colour Mode = Clip must read the clip BLURRED.** A contour runs along a
boundary, which is precisely the one place in the picture where the colour is
ambiguous — a point sample lands on whichever side the rounding fell. The
symptom was a ring whose outer circle came out cyan and whose inner circle came
out grey, from the same clip, for no reason an operator could see. It is read
at a mip level matched to the spot, so what the beam takes is the colour of the
region it is passing through.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
can only be called afterwards. `FF_TYPE_INTEGER` is exempt and passes its
default through untouched — which is why Galvo Speed really is declared in
kpps, Trace Size in pixels and Blanking Delay in signed points, and why those
four are the only parameters `--set` takes in real units.

**`SetParamGroup` collapses RUNS of consecutive same-group ids.** The enum
order in `Galvo.h` is therefore load-bearing: insert a parameter mid-enum and a
group silently splits in two, *and* every saved composition renumbers. Append
only.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. The core is an **OBJECT** library for that
reason. Verify with `nm -gU … | grep _plugMain` *and* an actual host load —
`tools/verify.sh` does both, the second through `oxbow`.

**Binding texture 0 to a declared sampler makes the driver complain.** The
trace shader declares `CopyTexture` whether or not Colour Mode is Clip, and the
harness renders with no clip at all; binding 0 there logs `unit 0 ... is
unloadable and bound to sampler type (Float)` once per instance, from a plugin
that is working perfectly. The unused unit is pointed at the accumulation
buffer we are *not* drawing into.

**GLSL reserved words, and this plugin walks straight into several.** `sample`
is reserved and a plugin about beam samples wants it everywhere; `half` is
reserved and the segment maths wants a half-length. Hence `sampleA`/`sampleB`
and `halfLen`. Shader errors surface only at **runtime**, as "the effect does
nothing", with a line number in a file that does not exist because the trace
shaders are assembled from strings.

**`set -o pipefail` plus `grep -q` is a race, and the big binary loses.**
`nm -gU "$bin" | grep -q _plugMain` reports failure *because* the symbol was
found: `grep -q` exits at the first match, `nm` takes SIGPIPE, and `pipefail`
propagates it. It is output-size dependent, so it looks intermittent. Capture
into a variable and match with `case` — `verify.sh` and `release.yml` both do.

**The harness needs a synthetic clock.** Left to the wall clock it renders a
hundred frames in a few milliseconds, so the frame delta clamps to its minimum,
the scanner's budget is tiny and nothing is reproducible. `gvtest` drives
`SetTime` on a synthetic 60 fps clock and declares its unit outright rather
than letting the calibration infer one.

---

## Shape of the code

    source/Tracer.*        thin, walk, simplify. No GL.
    source/PointStream.*   order, sample, dwell, blank, decimate. No GL.
    source/Scanner.*       the second-order mirrors and the point budget. No GL.
    source/render/Beam.*   decay, then energy along the path. The GPU half.
    source/Shaders.cpp     all GLSL: copy, edge, stabilise, decay, trace,
                           composite.
    source/Controls.*      0..1 host parameters to physical units.
    source/PassBuffer.*    FFGLFBO with the leak fixed, three sampling modes.
    source/Galvo.*         the plugin: parameters, buffers, the frame.
    source/Diag.*          a log file, for the shader that will not compile.
    tools/gvtest/          the offline harness.
    tools/sweep.py         no control is silently dead.
    tools/verify.sh        all of it, plus what the release job would check.

The frame, in order:

1. **copy** — picture size, mipmapped. Resolves `MaxUV` once. The mip chain is
   what lets the edge pass detect at a scale rather than at a pixel.
2. **edge** — *trace* size. Sobel on the selected channel, reading the copy at
   the mip level that matches the trace resolution plus Detail. This is what
   makes the readback small: the mask is born at 320 pixels wide rather than
   being shrunk to it afterwards.
3. **stabilise** — trace size, ping-ponged. Asymmetric temporal filter, then
   the threshold. Red is the mask; green is the pre-threshold value that feeds
   back, so Threshold can move without the history re-converging.
4. **readback** — one `glReadPixels` of one byte per trace texel.
5. **trace, order, stream, scan** — CPU, no GL.
6. **decay + trace** — picture size, additive, into the ping-ponged
   accumulation.
7. **composite** — output size, to the host.

**Three things live on the CPU because GL 4.1 core cannot do them**, not
because it was convenient: contour following is sequential pointer-chasing, and
FFGL on macOS has no compute shaders (4.3), no SSBOs (4.3) and no image
load/store (4.2). That constraint is also the gift — the entire physics of this
plugin is plain C++ that a test can call with no context at all.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4):**

- **The galvo really is a second-order system.** Step overshoot matches
  `exp(-pi z / sqrt(1-z^2))` at five dampings on both axes — 0.2537 measured
  against 0.2538 predicted at z=0.4 — and the residual eight points after the
  step matches the *exact* closed form to three decimals at every damping,
  including the critically- and over-damped cases where the formula changes
  shape.
- **Energy is conserved.** The light in a frame varies by **0.026%** across
  twelve galvos spanning 8–60 kpps and 0.4–1.2 damping, and by **0.011%**
  across an 8:1 range of point rates. Tolerance is 0.5%. It sits 0.33% below
  the analytic prediction, and that deficit is the 16-bit accumulation buffer —
  measured, not guessed: RGBA32F brings it to 0.016%. See `render/Beam.h` for
  why 16F ships anyway.
- **Dwell predicts brightness.** A corner with four dwell points is **4.27×**
  the mid-side brightness against a predicted 4.19× (+1.9%), and the side's own
  peak is within 1.0% of its closed form. All four corners of a square now
  agree to 0.5%.
- **The tracer does what it claims.** A one-pixel square outline is one closed
  contour of perimeter 319 against 320 expected, in four vertices; the same
  square as a three-pixel band is still *one* contour after thinning; a 270°
  arc is one **open** contour of the right length; and a filled square pushed
  through the plugin's own detect passes comes back as one closed contour
  within 2.3% of its predicted perimeter.
- **The point budget behaves.** 4000 points at 8 kpps take exactly 30 host
  frames to scan, and the engine's own counter agrees frame by frame across
  four point rates. Restart-each-frame pins the cursor at 500 and completes no
  scans, which is the "too busy, cut it off" behaviour rather than the crawl.
- **No dead controls.** All **24** swept parameters measurably change the
  picture, each with the context that makes it mean anything (`tools/sweep.py`).
  `Min Length` was genuinely dead until the test card grew short dashes for it
  to drop — the control was fine; the card had nothing in the range it governs.
- **It loads in a real FFGL host.** `oxbow selftest` instantiates it and renders
  120 frames with no GL error, reporting `SW Galvo` / `GV01` / `effect` and 28
  parameters in four groups with the About line populated.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, the plist names a binary that is
  really there, and the ad-hoc codesign the release job runs succeeds.
- **The render cost**, by `gvtest --bench` (60 frames each, 20-frame warm-up,
  `glFinish` both sides, default controls):

  | | ms/frame | of which CPU | readback alone |
  | --- | --- | --- | --- |
  | 1280×720 | 0.75 | 0.62 | 0.39 |
  | 1920×1080 | 0.87 | 0.74 | 0.44 |
  | 3840×2160 | 1.42 | 1.27 | 0.95 |

  Note what that says: **the GPU is not the cost, the readback is.** The beam
  renderer is about 0.15 ms at every resolution because it draws one quad per
  interval and the interval count depends on the point rate, not the raster.
  Scaling with resolution is almost entirely `glReadPixels` stalling the
  pipeline. Lowering Trace Size is therefore the performance control.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** `oxbow` is an FFGL host and a real one, but
  it is not Resolume: how the four groups present, whether 28 controls is too
  many in practice, and whether an operator can find Blanking Delay are all
  untested. Nothing has driven the host's own clock either — the millisecond
  detection is tinsel's, carried over, and has not been seen against Resolume
  here.
- **The kpps → bandwidth mapping is ours and is a rule of thumb.** A scanner
  rated at P pps is taken to settle a step in eight point periods at 0.7
  damping. That is stated in `Controls.h` so it can be argued with; it has not
  been checked against a real scanner's ILDA test-pattern figures.
- **No real projector has been driven.** This models an ILDA scanner; it does
  not output ILDA, and there is no DAC path. The point stream is ILDA-*style*,
  not ILDA-format.
- **Windows is untried.** It has never been compiled for Windows, let alone
  run: there is no Windows CI run behind this, only a workflow that would.
- **The one-frame-old question does not arise, and that is a cost.** The
  readback is synchronous rather than a double-buffered PBO like vectrix's, so
  the trace is of *this* frame — at the price of a pipeline stall. A PBO would
  halve the frame cost and make the contours one frame late. That is the single
  most valuable thing left to try.
- **No Plotter mode**, which the spec offered as optional. The scanner is
  finished and verified; the plotter is not started.
- **No OpenFX port and no browser demo**, neither required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies**,
  written in the shape the fleet's sync scripts generate, with `guide=""`
  because no user guide exists. Register the project in the website's
  `projects.json` and re-run the syncs before any release — the About facts
  were chosen so the button count, and therefore the parameter count, does not
  change when they are regenerated.

---

## Decisions taken without asking

The brief said to decide and move on, so: **Detect On** carries tinsel's four
modes verbatim rather than a reduced set, because the trap in the `Luma or
Alpha` default is worth inheriting the fix for. **Trace Size is an integer in
pixels** rather than a 0..1 scale, because an operator who wants 320 wants 320.
**The scan-rate floor decimates by regenerating the stream at a lower density**
rather than by dropping every Nth point, because dropping points from a
corner's dwell burst is exactly the wrong place to lose them. **Persistence is
an exposure, not a phosphor** — one decay, no colour cascade — because the
reference is a camera watching a laser, which is how anybody has ever seen one
photographed. **There are no factory presets**, because the fleet's preset
mechanism carries a host-echo trap that deserves its own pass rather than being
copied in at the end of a build.

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The renderer's
lineage is **vectrix**; the detect chain's is **tinsel**; the readback-and-walk
shape is **vectrix**'s Trace source, done at higher resolution and with the
ordering moved into its own file.
