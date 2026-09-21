#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag -- and the fix
# for a bad tag is to re-point it, which strands the release.
#
# The two that have actually bitten this fleet:
#
#   * CFBundleExecutable carrying the PREVIOUS plugin's name, because the
#     plist template was copied from another repo. Nothing fails: the bundle
#     assembles, the binary is universal, nm finds plugMain. Then codesign
#     says "code object is not signed at all" and mentions nothing about a
#     plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was latched before it arrived. The build log calls that a success. Only
#     lipo knows.
#
# What each check answers that none of the others can:
#
#   shaders    does every shader compile, through a real GLSL compiler, before
#              a host has to find out -- including the two the plugin assembles
#              at run time, which no file on disk contains
#   trace      the contour tracer: a square is one closed contour of the right
#              perimeter, a C is one open one -- and the same end to end
#              through the plugin's own detect passes
#   step       the galvo IS a second-order system: overshoot against the
#              textbook figure, and the residual after eight points against the
#              exact closed form, on both axes
#   budget     a frame of N points at P pps takes ceil( N / (P/60) ) host
#              frames, and the engine's own counter agrees
#   energy     the light in a frame does not depend on the galvo or the point
#              rate. The test the renderer exists to pass.
#   dwell      a corner is brighter than the side by the ratio its dwell points
#              predict, and the side matches its own closed form
#   sweep      no control is silently dead. A GLSL uniform whose name does not
#              match the C++ is ignored without a word, so this is the only
#              thing standing between a typo and a shipped slider that does
#              nothing.
#   bench      the render cost, for the record. Not pass/fail -- there is no
#              threshold worth asserting on somebody else's GPU -- but a verify
#              run leaves a timing, which is what turns "it feels slower" into
#              a comparison.
#   binary     universal, exports plugMain, the plist names a binary that is
#              really there, and the ad-hoc codesign the release job runs
#              succeeds
#   oxbow      instantiation and real frames through a real FFGL host, which
#              nothing else here reaches
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
UNIVERSAL="${UNIVERSAL:-build-universal}"
failures=()

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures+=("$1"); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [ "source/Shaders.cpp" ]

# Shaders the plugin assembles at RUN TIME, which therefore exist in no file.
# Mirrors TraceVertexSource()/TraceFragmentSource() in Shaders.cpp -- and a
# name that has moved is a KeyError here, not a silent skip.
ASSEMBLED = {
	"TraceVertex":   [ "#version 410 core\n", "kBeamConstants", "kTraceVertexBody" ],
	"TraceFragment": [ "#version 410 core\n", "kBeamConstants", "kTraceFragmentBody" ],
}

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass: it means the extraction
		# has lost track of where this repo keeps its GLSL, and a check that
		# silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	[ "$bad" -eq 0 ] && printf '   %d shaders, all compile\n' "$n"
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then pass "every shader compiles"; else fail "a shader does not compile"; fi

#---------------------------------------------------------------------------
# A FRESH universal build, and the build directory is deleted first.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list lives. A developer who configured once
# with -DCMAKE_OSX_ARCHITECTURES=arm64 for a fast iteration loop -- which is
# the documented way to work in CLAUDE.md -- leaves a tree where this script
# happily rebuilds, finds a single-architecture binary, and reports it as a
# defect in the source.
#---------------------------------------------------------------------------
step "build (fresh, universal)"
rm -rf "$UNIVERSAL"
if cmake -B "$UNIVERSAL" -DCMAKE_BUILD_TYPE=Release >/tmp/galvo-configure.log 2>&1 \
   && cmake --build "$UNIVERSAL" --parallel >/tmp/galvo-build.log 2>&1; then
	pass "configured and built universal"
else
	fail "build failed -- see /tmp/galvo-build.log"
	tail -25 /tmp/galvo-build.log
	printf '\n\033[31mstopping: nothing below can run\033[0m\n'
	exit 1
fi

TEST="$UNIVERSAL/gvtest"

step "the checks"
for check in trace step budget energy dwell; do
	log="/tmp/galvo-$check.log"
	if "$TEST" "--$check" >"$log" 2>&1; then
		pass "gvtest --$check"
	else
		fail "gvtest --$check -- see $log"
		tail -12 "$log"
	fi
done

step "sweep: no control silently dead"
if python3 tools/sweep.py --binary "$TEST" >/tmp/galvo-sweep.log 2>&1; then
	pass "$(tail -1 /tmp/galvo-sweep.log)"
else
	fail "dead controls -- see /tmp/galvo-sweep.log"
	tail -5 /tmp/galvo-sweep.log
fi

step "bench: the render cost, for the record"
"$TEST" --bench --frames 60 2>&1 | sed -n '3,8p'

BUNDLE="$UNIVERSAL/Galvo.bundle"
BIN="$BUNDLE/Contents/MacOS/Galvo"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "binary"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o
	# pipefail`: grep exits at once, nm takes SIGPIPE, and the pipeline reports
	# failure. It is output-size dependent, so it fires on the bigger binary
	# first and looks intermittent. Capture and match with `case` -- not a
	# pipeline anywhere.
	symbols=$( nm -gU "$BIN" 2>/dev/null || true )
	case "$symbols" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle would load and contain no plugins" ;;
	esac

	archs=$( lipo -archs "$BIN" 2>/dev/null )
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present ($archs)" ;; *) fail "NOT universal (got: $archs)" ;; esac

	exe=$( /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign fails after the tag"
	fi

	ident=$( /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ "$ident" = "com.stoatworks.ffgl.galvo" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', not com.stoatworks.ffgl.galvo"
	fi

	step "codesign (the exact command the release job runs, on a copy)"
	tmp=$( mktemp -d )
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Galvo.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs"
	else
		fail "ad-hoc signing failed -- the failure that never mentions the plist"
	fi
	rm -rf "$tmp"

	step "oxbow: a real FFGL host loads it"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$( "$OXBOW" probe "$BUNDLE" 2>&1 )
		case "$probe" in
			*"SW Galvo"*) pass "name is SW Galvo" ;;
			*) fail "oxbow does not see the name: $( printf '%s' "$probe" | head -3 )" ;;
		esac
		case "$probe" in *"GV01"*) pass "id is GV01" ;; *) fail "id is not GV01" ;; esac
		case "$probe" in *"type:        effect"*) pass "type is effect" ;; *) fail "type is not effect" ;; esac

		# An effect needs an input, and oxbow's selftest feeds it one.
		self=$( "$OXBOW" selftest "$BUNDLE" 2>&1 )
		case "$self" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "instantiates and renders in a host" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if (( ${#failures[@]} == 0 )); then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi
printf '\033[31mFAILURES:\033[0m\n'
printf '  %s\n' "${failures[@]}"
exit 1
