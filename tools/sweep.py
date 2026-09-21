#!/usr/bin/env python3
"""No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is
indistinguishable from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

------------------------------------------------------------------ the traps

**Most controls are conditional, and each one carries the context it needs.**
Dim is invisible unless Background is Dimmed Clip; Hue Spread does nothing
unless Colour Mode is Palette; Scan Rate Floor does nothing until the stream is
too long for it to matter. A naive sweep reports those as dead and buries the
one real failure in nine false ones. Those contexts are the interesting content
of this file -- add a parameter without one and, if it turns out to be
conditional, this will say so loudly.

**Stability provably does nothing on a still picture**, because the history and
the current frame hold the same number and every blend between them returns it.
It is swept against per-frame noise, which is the condition it exists for.

**Blanking Delay is swept as an integer**, because it is one: the positions
below are in the parameter's own units for an integer control, not 0..1.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press.

    python3 tools/sweep.py [--binary build/gvtest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.
"""

import argparse
import concurrent.futures
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all, and any
# render settings it needs. "Frames" and "Noise" are not plugin parameters but
# both decide whether certain controls can mean anything.
CONTEXT = {
    # A threshold needs something marginal to be either side of it.
    "Threshold": ["Detect On=0"],
    # Stability filters over TIME. On a still picture every blend returns the
    # same number -- see the docstring.
    "Stability": ["Noise=0.12", "Frames=24"],
    # Dwell points only exist where there are corners, and the card's square
    # and bar have them; at density 1.0 the sides swamp them, so hold Density
    # somewhere the dots read.
    "Corner Dwell": ["Density=0.4"],
    # The beam has to be somewhere for a delay to move it, and a delay of a
    # couple of points is only visible when points are far apart.
    "Blanking Delay": ["Density=0.25"],
    # Nothing to decimate unless the stream is long and the scanner is slow.
    "Scan Rate Floor": ["Density=0.95", "Galvo Speed=8", "Min Length=0.0"],
    # Free-running versus restart-each-frame only differ once the stream is
    # longer than one frame's budget.
    "Frame Sync": ["Density=0.95", "Galvo Speed=8", "Frames=6"],
    # Persistence is a multi-frame accumulation, so it needs frames AND
    # something that moves between them.
    "Persistence": ["Noise=0.06", "Frames=20"],
    # The colour pickers and the hue sweep only act in Palette mode.
    "Colour": ["Colour Mode=1"],
    "Colour_Green": ["Colour Mode=1"],
    "Colour_Blue": ["Colour Mode=1"],
    "Hue Spread": ["Colour Mode=1"],
    # Dimming is a background mode.
    "Mix": ["Background=1"],
}

# Positions to try, as a fraction of the parameter's declared range. Three
# rather than two: a control that is a no-op at both ends but not in the
# middle is rare, but it costs one render to stop worrying about it.
FRACTIONS = [0.0, 0.5, 1.0]

# Parameters with no scalar value worth sweeping.
SKIP = {
    "About": "a display-only text line",
    "User guide": "a button that opens a web browser",
    "Project page": "a button that opens a web browser",
    "Source on GitHub": "a button that opens a web browser",
    "Support the work": "a button that opens a web browser",
}


def parse_list(binary):
    """Every parameter as (name, type, default, min, max)."""
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError("could not list parameters: " + listing.stderr.strip())

    rows = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 5:
            continue
        # The name may contain spaces, so take the fixed columns off the end.
        low, high = float(parts[-2]), float(parts[-1])
        default, kind = float(parts[-3]), parts[-4]
        name = " ".join(parts[1:-4])
        rows.append((name, kind, default, low, high))
    return rows


def render(binary, out, settings, size, frames, noise):
    command = [binary, "--out", str(out), "--size", size,
               "--frames", str(frames), "--noise", str(noise)]
    for setting in settings:
        command += ["--set", setting]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("render failed: " + result.stderr.strip())
    return hashlib.sha256(out.read_bytes()).hexdigest()


def sweep_one(binary, size, row, index):
    name, kind, _default, low, high = row
    context = list(CONTEXT.get(name, []))

    frames, noise = 10, 0.0
    for entry in list(context):
        if entry.startswith("Frames="):
            frames = int(entry.split("=", 1)[1])
            context.remove(entry)
        elif entry.startswith("Noise="):
            noise = float(entry.split("=", 1)[1])
            context.remove(entry)

    # An integer or option parameter is set in its own units; a standard one
    # is 0..1 and its range says so anyway.
    values = [low + (high - low) * f for f in FRACTIONS]
    if kind in ("integer", "option"):
        values = sorted({round(v) for v in values})

    digests = set()
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / f"sweep{index}.png"
        for value in values:
            digests.add(render(binary, out, context + [f"{name}={value}"],
                               size, frames, noise))
    return name, len(digests) > 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/gvtest")
    parser.add_argument("--size", default="480x270")
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DGALVO_BUILD_TOOLS=ON first")
        return 2

    rows = [r for r in parse_list(str(binary)) if r[0] not in SKIP and r[1] != "text"]
    if not rows:
        print("no parameters found")
        return 2

    dead, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = {pool.submit(sweep_one, str(binary), arguments.size, row, i): row[0]
                   for i, row in enumerate(rows)}
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                name, alive = future.result()
            except RuntimeError as error:
                print(f"  {'ERR':4}  {name}: {error}")
                failed.append(name)
                continue
            print(f"  {'ok' if alive else 'DEAD':4}  {name}")
            if not alive:
                dead.append(name)

    print()
    if failed:
        print(f"{len(failed)} parameter(s) could not be rendered: {', '.join(failed)}")
        return 1
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(sorted(dead))}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {len(rows)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
