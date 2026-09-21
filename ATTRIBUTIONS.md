# Attributions

Galvo is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Galvo is not
> registered there yet, so this copy is hand-written in the shape the script
> produces. Register it before the first release — and note that the script's
> `--only` flag truncates the file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at `external/ffgl/deps/glew-2.1.0`. Not
fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at
OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>
Licence: PNG Reference Library License (libpng)
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly
— listed because it is present in the checkout.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked from the system, by the offline harness only, to deflate the PNGs it
writes. Nothing in the shipped plugin uses it.

## Methods, not code

Nothing below was copied. Each is a published method implemented here from a
description of what it does.

- **Zhang-Suen thinning** — T. Y. Zhang and C. Y. Suen, "A fast parallel
  algorithm for thinning digital patterns", CACM 27(3), 1984. Reduces the
  thresholded edge band to a one-pixel skeleton so a contour walk does not
  wander across it.
- **Ramer–Douglas–Peucker simplification** — Urs Ramer (1972); David Douglas
  and Thomas Peucker (1973). Turns a traced staircase back into the few
  vertices a scanner should actually be sent to.
- **The second-order galvanometer model** is the standard damped-oscillator
  step response found in any controls textbook. The mapping from a scanner's
  kpps rating to a natural frequency is this repo's own, and it is stated in
  `source/Controls.h` so it can be argued with.

The energy-conserving segment renderer — a fixed quantum of light per sample
interval, spread along the distance covered by the exact convolution of a
uniform segment with a Gaussian — is carried over from this fleet's **vectrix**
and is the same code lineage, not a third-party component.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong,
or you would rather not be listed — open an issue and it will be fixed.
