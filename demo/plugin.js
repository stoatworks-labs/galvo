/**
 * Galvo — browser demo.
 *
 * An ILDA laser projector in a page. The one idea, from `AGENTS.md`: **nothing
 * is drawn.** The picture is where two galvanometer mirrors went and how long
 * they lingered there, so the bright dot on every corner, the rounding at
 * speed, the hooks from a mis-set blanking delay and the flicker of a frame too
 * busy for the scanner are not effects — they fall out of a second-order mirror
 * following a point stream at a fixed rate, and a renderer that deposits equal
 * energy per unit of beam-on time.
 *
 * That means this plugin is **not a shader**. It is a GPU edge detector, then a
 * synchronous readback, then a CPU tracer / ordering / point stream / galvo
 * model, then a GPU beam renderer. The CPU middle has to exist here or the page
 * has nothing to show at all, so the whole of it is ported — and the two halves
 * are not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX`, `COPY`, `EDGE`, `STABILISE`,
 *   `DECAY`, `BEAM_CONSTANTS`, `TRACE_VERTEX_BODY`, `TRACE_FRAGMENT_BODY` and
 *   `COMPOSITE` below are `kVertexShader` … `kCompositeShader` from
 *   `source/Shaders.cpp`, copied across unedited and assembled the way
 *   `TraceVertexSource()`/`TraceFragmentSource()` assemble them.
 *   `demo/tools/check_shaders.py` compares them character for character and
 *   `tools/verify.sh` runs it, because two copies of a shader is exactly the
 *   arrangement that drifts.
 *
 *   The CPU half is a port — of `Controls.cpp`, `Tracer.cpp`,
 *   `PointStream.cpp` and `Scanner.cpp`, in that order, function for function.
 *   Nothing checks a port but a reader. `gvtest --trace`, `--step`, `--budget`,
 *   `--energy` and `--dwell` in the repository check the C++ originals and have
 *   no idea this page exists.
 *
 * ------------------------------------------------------ what this page traces
 *
 * At the plugin's own working resolution, and that is load-bearing rather than
 * a saving. Reading a mask back from the GPU in a browser is `readPixels`,
 * which is synchronous and stalls the pipeline exactly as the plugin's
 * `glReadPixels` does — so the page traces at **Trace Size**, 320 × 180 by
 * default and up to 640 × 360, one byte per texel, the same mask the plugin
 * walks. The Zhang-Suen thinning, the contour walk and the Douglas-Peucker
 * simplification then run over that, in JavaScript, every frame.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** There is nothing to miss: Galvo has no audio path, no FFT
 * parameter and no beat input, so unlike the rest of this suite there is no
 * caveat to make here.
 *
 * **The stabilise buffer is eight bits deep, not sixteen.** The plugin keeps
 * its ping-ponged edge buffers in RGBA16F and reads the mask back as
 * `GL_RED`/`GL_UNSIGNED_BYTE`. WebGL2 will not read a float framebuffer as
 * bytes, so those two buffers are RGBA8 here. The mask the tracer walks is a
 * byte either way and is unaffected; what is quantised is the *green* channel,
 * the pre-threshold value the temporal filter feeds back to itself, so
 * Stability has slightly coarser memory here than in the plugin.
 *
 * **The integer controls are dropdowns.** Galvo Speed, Trace Size, Corner Dwell
 * and Blanking Delay are `FF_TYPE_INTEGER` and carry real units — kpps, pixels,
 * points — and the demo kit has no integer control, so they are dropdowns of
 * their own values. Trace Size is the one that had to be thinned: the plugin's
 * range is every integer from 160 to 640 and this offers every twentieth one.
 *
 * **The About block is absent**, as on every page in this suite: four buttons
 * that open a browser.
 *
 * ------------------------------------------------------- decided, not asked
 *
 * **The whole CPU chain is ported rather than a subset**, because every part of
 * it is load-bearing for the picture: drop the tracer and there is nothing to
 * scan, drop the scanner and there is no crawl, drop the corner dwell and the
 * thing the plugin is for is gone.
 *
 * **There is a line of statistics under the canvas** — contours, points in the
 * stream, points scanned this frame, milliseconds on the CPU half — which no
 * other page in this suite has. Galvo's most distinctive behaviour is what
 * happens when a frame is longer than the scanner's point budget, and without
 * those numbers a crawling picture reads as a broken page rather than as a
 * projector running out of points. It reports; it measures nothing.
 *
 * **The clip list starts on `Shape on transparency`, not the Geometry card.**
 * The geometry card is a fine-line test card, and at a 320-pixel trace
 * resolution taken from a 960-pixel picture its lines sit below the mip level
 * the Sobel reads at, so it traces almost nothing. That is correct and it looks
 * like a fault. A logo-shaped outline is what this plugin is for.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core — so a pixel here
 * is not evidence about a pixel there.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The backticks inside the comments are escaped, because a template literal has
// nowhere else to go; check_shaders.py decodes that one escape before comparing
// and rejects any other backslash, so the escape cannot hide a difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. MaxUV is folded in once, in the
	//copy pass; every pass after it works on a texture we allocated, where the
	//picture really does fill the texture.
	uv = vUV;
}
`;

const COPY = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and on a logo that
	//shows up as a false edge down the side of the frame -- which this plugin
	//would then dutifully scan.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. The mip chain built on this texture
	//is a box filter, and averaging premultiplied samples is the correct
	//filter; averaging straight colour smears the colour of transparent pixels
	//into the picture.
	fragColor = texture( InputTexture, picture * MaxUV );
}
`;

const EDGE = `#version 410 core

uniform sampler2D CopyTexture;
uniform vec2 Step;        //one Sobel tap, in picture space: a trace texel times 2^Detail
uniform float Lod;        //mip level to read: log2( picture / trace ) + Detail
uniform float SourceMode; //0 luma, 1 alpha, 2 chroma, 3 luma or alpha

in vec2 uv;
out vec4 fragColor;

//What "different" means between two pixels. A logo delivered with alpha has a
//perfect edge already in its alpha channel, and running a luma Sobel over it
//instead throws away the only clean signal in the frame.
float channel( vec2 at )
{
	vec4 c = textureLod( CopyTexture, at, Lod );
	int mode = int( SourceMode + 0.5 );

	if( mode == 1 )
		return c.a;

	if( mode == 2 )
	{
		//Chroma distance from the pixel's own grey: the boundary between two
		//colours of equal brightness, which a luma Sobel is blind to.
		float y = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		return length( c.rgb - vec3( y ) ) + y * 0.25;
	}

	//Un-premultiply before taking luma, or a soft alpha edge reads as a
	//brightness ramp and the Sobel finds a wide smear where there is a hard
	//boundary.
	vec3 straight = c.a > 0.0031 ? c.rgb / c.a : c.rgb;
	float luma = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );

	if( mode == 3 )
	{
		//Both at once. The alpha sets a floor so a dark logo on transparency
		//still has a boundary; the luma adds the detail inside the shape.
		//NOT max( luma * c.a, c.a ), which is identically 1.0 for every opaque
		//pixel and found no edges on any clip without alpha (tinsel's trap).
		return c.a * ( 0.35 + 0.65 * luma );
	}

	return luma;
}

void main()
{
	//Sobel. Two 3x3 convolutions; the magnitude of the pair is the gradient.
	float tl = channel( uv + vec2( -Step.x,  Step.y ) );
	float tc = channel( uv + vec2(     0.0,  Step.y ) );
	float tr = channel( uv + vec2(  Step.x,  Step.y ) );
	float ml = channel( uv + vec2( -Step.x,     0.0 ) );
	float mr = channel( uv + vec2(  Step.x,     0.0 ) );
	float bl = channel( uv + vec2( -Step.x, -Step.y ) );
	float bc = channel( uv + vec2(     0.0, -Step.y ) );
	float br = channel( uv + vec2(  Step.x, -Step.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );

	//Divide by four, the sum of one side of the kernel, so a clean black-to-
	//white step gives exactly 1.0 and Threshold means the same on any footage.
	fragColor = vec4( length( vec2( gx, gy ) ) * 0.25, 0.0, 0.0, 1.0 );
}
`;

const STABILISE = `#version 410 core

uniform sampler2D EdgeTexture;
uniform sampler2D HistoryTexture; //the previous frame's output of this pass
uniform float Attack;             //0..1 blend towards a *stronger* edge
uniform float Release;            //0..1 blend towards a *weaker* edge
uniform float Threshold;          //gradient magnitude at which a pixel is an edge
uniform float Softness;           //width of the threshold, as a fraction of it
uniform float Reset;              //1 to ignore history entirely

in vec2 uv;
out vec4 fragColor;

void main()
{
	float current = texture( EdgeTexture, uv ).r;
	float history = texture( HistoryTexture, uv ).g;

	//Asymmetric on purpose. A symmetric IIR is a low-pass, and a low-pass on
	//an edge signal trades flicker for lag: the outline of anything moving
	//arrives late. What actually goes wrong on footage is that edges *drop
	//out* for a frame, and the contour the tracer found last frame vanishes
	//and comes back. So an edge that appears is believed at once and an edge
	//that vanishes is given a few frames to return.
	float blend = current > history ? Attack : Release;
	float stable = mix( history, current, blend ) * ( 1.0 - Reset ) + current * Reset;

	float lower = Threshold * ( 1.0 - Softness );
	float upper = Threshold * ( 1.0 + Softness );
	float mask = smoothstep( lower, max( upper, lower + 1e-5 ), stable );

	//r: the mask, which is what the CPU reads back as one byte per texel.
	//g: what feeds back, before the threshold, so Threshold can move without
	//   the history having to re-converge.
	fragColor = vec4( mask, stable, 0.0, 1.0 );
}
`;

const DECAY = `#version 410 core

uniform sampler2D HistoryTexture;
uniform float Decay;   //fraction kept this frame; 0 clears
uniform float Ceiling; //a runaway may not climb past this

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 h = texture( HistoryTexture, uv );

	//A ping-ponged accumulator is the one place a single bad value is
	//permanent: NaN * Decay is NaN for the rest of the session. Comparisons
	//rather than isnan(), which a fast-math compiler is entitled to fold away.
	if( !( h.r > -1e30 && h.r < 1e30 ) ) h.r = 0.0;
	if( !( h.g > -1e30 && h.g < 1e30 ) ) h.g = 0.0;
	if( !( h.b > -1e30 && h.b < 1e30 ) ) h.b = 0.0;
	if( !( h.a > -1e30 && h.a < 1e30 ) ) h.a = 0.0;

	fragColor = min( h * Decay, vec4( Ceiling ) );
}
`;

const BEAM_CONSTANTS = `
const float Extent     = 4.5;
const float Sqrt2Pi    = 2.50662827463100050;
const float InvSqrt2Pi = 0.39894228040143268;
`;

const TRACE_VERTEX_BODY = `
layout( location = 0 ) in vec4 sampleA;  //x, y picture units; dt seconds; beam on
layout( location = 1 ) in vec4 colourA;  //r, g, b
layout( location = 2 ) in vec4 sampleB;
layout( location = 3 ) in vec4 colourB;

uniform sampler2D CopyTexture; //the picture, for Colour Mode = Clip
uniform float BeamPower;       //light per second of beam-on time
uniform float SpotSigma;       //picture units: 1 = the picture height
uniform float Aspect;          //picture width / height; x runs 0..Aspect
uniform float ColourMode;      //0 clip, 1 palette, 2 white
uniform float ColourLod;       //mip level the clip is read at: the spot's own size
uniform float Saturate;        //how far a clip colour is pushed toward a laser primary

flat out float segLength;
flat out float segSigma;
flat out float segEnergy;
flat out vec3 segColour;
out vec2 segUV;                //x along the segment from its centre, y across

//Not isnan()/isinf(): those are the first thing a fast-math compiler folds to
//constant false. A comparison chain has no intrinsic to fold, and one NaN in
//the accumulation buffer is there for the life of the plugin instance.
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

void main()
{
	vec2 a = sampleA.xy;
	vec2 b = sampleB.xy;

	//The whole brightness model, in one line: energy per interval, not
	//intensity per pixel. The fragment stage spreads it over however far the
	//mirrors moved, so nothing anywhere divides by a speed, and a mirror that
	//lingers puts more light in one place because it spent more intervals
	//there. \`on\` is the interval's blanking state; \`dt\` is the interval's
	//length, carried per sample so this is independent of the point rate.
	float energy = BeamPower * max( sampleA.z, 0.0 ) * sampleA.w;
	float sigma  = SpotSigma;

	vec3 colour = colourA.rgb;
	if( int( ColourMode + 0.5 ) == 0 )
	{
		//The clip under the middle of the interval, read BLURRED -- at a mip
		//level matched to the spot -- and that is the whole trick.
		//
		//A contour runs along a boundary, which is precisely the one place in
		//the picture where the colour is ambiguous: a point sample there lands
		//on whichever side the rounding fell, so a ring came out cyan on its
		//outer edge and grey on its inner one, from the same clip, for no
		//reason an operator could see. Reading the local average instead gives
		//the colour of the region the beam is passing through, which is what
		//"the clip's colour" means to the person who chose the mode.
		vec2 mid = 0.5 * ( a + b );
		vec4 c = textureLod( CopyTexture, vec2( mid.x / Aspect, mid.y ), ColourLod );

		//Un-premultiplied, and a transparent pixel is taken as white rather
		//than black: the edge of an alpha-keyed logo sits exactly on the
		//boundary, where a half-alpha sample would otherwise dim every outline
		//found by Alpha.
		vec3 straight = c.a > 0.05 ? c.rgb / c.a : vec3( 1.0 );

		//Toward a laser colour. A laser's channels are lit or not; the picture
		//underneath is any mixture. Raising the ratios to a power keeps the
		//brightest channel where it is and pushes the others down, so a pink
		//becomes a red beam at the same brightness and a grey stays white.
		float m = max( straight.r, max( straight.g, straight.b ) );
		vec3 pure = m > 1e-4 ? m * pow( straight / m, vec3( 1.0 + 3.0 * Saturate ) ) : vec3( 0.0 );
		colour = pure;
	}
	else if( int( ColourMode + 0.5 ) == 2 )
	{
		colour = vec3( 1.0 );
	}

	vec2 delta = b - a;
	float span = length( delta );
	vec2 dir   = span > 1e-9 ? delta / span : vec2( 1.0, 0.0 );

	//A floor of a twentieth of a spot, so the box is never zero-area. The
	//fragment stage does not divide by it below a quarter of a sigma anyway.
	float len = max( span, 0.05 * sigma );

	bool ok = usable( a.x ) && usable( a.y ) && usable( b.x ) && usable( b.y )
	       && usable( energy ) && energy > 0.0 && sigma > 0.0;

	if( !ok )
	{
		//A degenerate quad off the frustum: no area, rasterises nothing.
		//Returning without writing gl_Position would be undefined.
		segLength   = 0.0;
		segSigma    = 1.0;
		segEnergy   = 0.0;
		segColour   = vec3( 0.0 );
		segUV       = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	//An oriented box around the capsule, padded by Extent sigma on all sides.
	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;

	//Corners from the vertex index, as a triangle strip.
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = a + dir * ( 0.5 * len );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength = len;
	segSigma  = sigma;
	segEnergy = energy;
	segColour = colour;
	segUV     = vec2( sx * halfAlong, sy * halfAcross );

	//Picture units are isotropic -- one unit is the picture height on both
	//axes -- so the spot is round. The divide is the only place the frame's
	//shape enters the trace at all.
	gl_Position = vec4( pos.x / Aspect * 2.0 - 1.0, pos.y * 2.0 - 1.0, 0.0, 1.0 );
}
`;

const TRACE_FRAGMENT_BODY = `
flat in float segLength;
flat in float segSigma;
flat in float segEnergy;
flat in vec3 segColour;
in vec2 segUV;

out vec4 fragColor;

//The standard normal CDF. GLSL has no erf, so the usual tanh approximation,
//good to about 3e-4. The clamp is not tidiness: a driver computing tanh as
//(e^2x - 1)/(e^2x + 1) overflows around x = 44 and yields inf/inf = NaN.
float ncdf( float x )
{
	float t = clamp( 0.7978845608 * ( x + 0.044715 * x * x * x ), -8.0, 8.0 );
	return 0.5 * ( 1.0 + tanh( t ) );
}

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across: a normalised Gaussian, minus its value at the quad's own edge, so
	//the cut at Extent sigma is a smooth zero and not a step. One segment's
	//step is invisible; a thousand overlapping at a dwell dot is a visible
	//polygon edge in the brightest part of the picture.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across = max( across - pedestal, 0.0 );

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, taken explicitly. A difference of two nearly equal
		//CDFs each carrying 3e-4 of error is several percent wrong by the
		//time L is a quarter of a sigma -- and a short segment is exactly the
		//dwell dot this plugin is about. The limit costs one exp and is exact.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv );
	}
	else
	{
		float halfLen = 0.5 * segLength;//\`half\` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	float deposit = segEnergy * across * along;
	fragColor = vec4( segColour * deposit, deposit );
}
`;

const COMPOSITE = `#version 410 core

uniform sampler2D CopyTexture;
uniform sampler2D BeamTexture;   //accumulated light, rgb and a
uniform sampler2D StableTexture; //the mask, for Background = Edge Mask

uniform float Background; //0 black, 1 clip, 2 dimmed clip, 3 alpha, 4 edge mask
uniform float Dim;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 source = texture( CopyTexture, uv );
	vec4 beam = texture( BeamTexture, uv );

	int mode = int( Background + 0.5 );

	vec4 result;
	if( mode == 4 )
	{
		//The edge mask on its own, in white. Not a look: it is how Threshold,
		//Detail and Trace Size are actually set, because judging a threshold
		//through a layer of beam is guesswork. Nearest-sampled at the trace
		//resolution, so the operator sees the texels the tracer walks.
		float mask = texture( StableTexture, uv ).r;
		result = vec4( vec3( mask ), 1.0 );
	}
	else if( mode == 3 )
	{
		//The light over nothing, premultiplied, for the layer below.
		result = vec4( beam.rgb, clamp( beam.a, 0.0, 1.0 ) );
	}
	else
	{
		vec4 back = source;
		if( mode == 0 )
			back = vec4( 0.0, 0.0, 0.0, 1.0 );
		else if( mode == 2 )
			back = vec4( source.rgb * Dim, source.a );

		//Added, not blended. A beam is light arriving on top of whatever is
		//there; alpha-blending it would have the beam hide the picture it is
		//drawn over.
		result = vec4( back.rgb + beam.rgb, clamp( back.a + beam.a, 0.0, 1.0 ) );
	}

	fragColor = mix( source, result, MixAmount );
}
`;

// The two trace stages are assembled around one shared constants string,
// because the vertex stage sizing the quad and the fragment stage subtracting
// the Gaussian's pedestal have to agree about `Extent`. Same assembly as
// TraceVertexSource() / TraceFragmentSource(), and tools/verify.sh mirrors it
// again when it compiles them with glslc.
const TRACE_VERTEX = `#version 410 core\n${BEAM_CONSTANTS}${TRACE_VERTEX_BODY}`;
const TRACE_FRAGMENT = `#version 410 core\n${BEAM_CONSTANTS}${TRACE_FRAGMENT_BODY}`;

//===========================================================================
// A port of source/Controls.cpp.
//
// Every FF_TYPE_STANDARD parameter is a plain 0..1 float even where it stands
// for trace pixels or a damping ratio, because SetParamInfo clamps a STANDARD
// default into 0..1 before SetParamRange can widen it. These are the functions
// that say what a slider position means, and they exist in the plugin so the
// harness and the effect cannot disagree — this is a third copy, and only a
// reader is checking it.
//===========================================================================

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

const lerp = (from, to, t) => from + (to - from) * clamp01(t);

/// Geometric interpolation. Equal slider movements are equal *ratios*, which is
/// the right behaviour for any quantity where the question is "how many times
/// more" rather than "how much more".
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

const thresholdFromParam = (v) => geometric(0.02, 1.0, v);
const detailFromParam = (v) => lerp(0.0, 3.0, v);
const attackFromParam = (v) => lerp(1.0, 0.75, v);
const releaseFromParam = (v) => geometric(1.0, 0.02, v);
const minLengthFromParam = (v) => lerp(0.0, 60.0, v);
const simplifyFromParam = (v) => lerp(0.2, 4.0, v);
const dampingFromParam = (v) => lerp(0.4, 1.2, v);

/// A scanner rated at P pps is taken to settle a step to within 2% in eight
/// point periods at a damping of 0.7. 4 / (zeta * omega) is that settling time,
/// so omega = 4P / (8 * 0.7) = 0.714 P — 3.4 kHz for a 30 kpps scanner, which
/// is the right order for a real one. The rule of thumb is what sets omega; it
/// is not a claim about what the mirror then does.
const omegaFromPointRate = (pps) => (4.0 * pps) / (8.0 * 0.7);

const densityFromParam = (v) => geometric(20.0, 400.0, v);
const scanRateFloorFromParam = (v) => {
  const hz = lerp(0.0, 30.0, v);
  return hz < 1.0 ? 0.0 : hz;
};
const spotSigmaFromParam = (v) => geometric(0.0015, 0.02, v);
const beamPowerFromParam = (v) => geometric(0.3, 30.0, v);
const persistenceTauFromParam = (v) => (v <= 0.0 ? 0.0 : geometric(0.004, 1.0, v));
const hueSpreadFromParam = (v) => lerp(0.0, 2.0, v);

/// Constants the controls do not reach — Controls.h.
const kCornerAngleDegrees = 30.0;
const kBlankSettle = 2;
const kThresholdSoftness = 0.3;
const kDimLevel = 0.25;
const kLaserSaturation = 0.85;

/// Galvo.cpp: a host frame is between these whatever the clock says. Shorter is
/// a scrub or a stall, longer is the machine having been asleep, and neither
/// should scan a minute of points in one go. A backgrounded tab is the
/// browser's version of the same accident.
const kMinFrame = 1.0 / 240.0;
const kMaxFrame = 1.0 / 24.0;

//===========================================================================
// A port of source/Tracer.cpp — from a binary edge mask to ordered polylines.
//
// This is the half of the pipeline the GPU cannot do: contour following is
// pointer-chasing, which pixel comes *next*, and FFGL on macOS is GL 4.1 core
// with no compute shaders. So the plugin reads the mask back small and walks it
// on the CPU, and so does this.
//===========================================================================

/// The 8-neighbourhood. The four direct neighbours come first so that the walk
/// prefers a straight step over a diagonal one: on a thinned skeleton a
/// diagonal taken while a direct neighbour was available leaves that neighbour
/// orphaned as a one-pixel contour of its own.
const NEIGHBOUR_X = [1, 0, -1, 0, 1, -1, -1, 1];
const NEIGHBOUR_Y = [0, 1, 0, -1, 1, 1, -1, -1];

/// Zhang-Suen's neighbourhood, in its own order: P2 north, then clockwise.
const ZS_X = [0, 1, 1, 1, 0, -1, -1, -1];
const ZS_Y = [1, 1, 0, -1, -1, -1, 0, 1];

/// Scratch for the eight neighbours inside the thinning loop. See thin().
const neighbourhood = new Int32Array(8);

function distanceToSegment(p, a, b) {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len < 1e-6) return Math.sqrt((p.x - a.x) * (p.x - a.x) + (p.y - a.y) * (p.y - a.y));
  return Math.abs(dy * p.x - dx * p.y + b.x * a.y - b.y * a.x) / len;
}

/// Iterative Douglas-Peucker over an open run [first, last], marking survivors
/// in `keep`.
function simplifyRun(points, first, last, epsilon, keep) {
  keep[first] = 1;
  keep[last] = 1;
  const stack = [first, last];

  while (stack.length > 0) {
    const b = stack.pop();
    const a = stack.pop();
    if (b <= a + 1) continue;

    let worst = 0.0;
    let pick = a;
    for (let i = a + 1; i < b; i += 1) {
      const d = distanceToSegment(points[i], points[a], points[b]);
      if (d > worst) {
        worst = d;
        pick = i;
      }
    }

    if (worst > epsilon) {
      keep[pick] = 1;
      stack.push(a, pick);
      stack.push(pick, b);
    }
  }
}

function polylineLength(points, closed) {
  let length = 0.0;
  for (let i = 1; i < points.length; i += 1) {
    const dx = points[i].x - points[i - 1].x;
    const dy = points[i].y - points[i - 1].y;
    length += Math.sqrt(dx * dx + dy * dy);
  }
  if (closed && points.length > 2) {
    const dx = points[0].x - points[points.length - 1].x;
    const dy = points[0].y - points[points.length - 1].y;
    length += Math.sqrt(dx * dx + dy * dy);
  }
  return length;
}

function simplifyPolyline(points, epsilon, closed) {
  if (points.length < 3) return points.slice();

  const keep = new Uint8Array(points.length);

  if (!closed) {
    simplifyRun(points, 0, points.length - 1, epsilon, keep);
  } else {
    // A closed loop has no endpoints to anchor on, so anchor on the first point
    // and the one farthest from it. Each half is then an ordinary open run, and
    // the two anchors survive whatever the tolerance — which is what keeps a
    // circle from simplifying to nothing.
    let far = 0;
    let farthest = -1.0;
    for (let i = 1; i < points.length; i += 1) {
      const dx = points[i].x - points[0].x;
      const dy = points[i].y - points[0].y;
      const d = dx * dx + dy * dy;
      if (d > farthest) {
        farthest = d;
        far = i;
      }
    }

    simplifyRun(points, 0, far, epsilon, keep);

    // The second half runs from `far` round to the first point again, which is
    // not in the array twice — so it is simplified against a copy with the
    // start appended.
    const tail = points.slice(far);
    tail.push(points[0]);
    const keepTail = new Uint8Array(tail.length);
    simplifyRun(tail, 0, tail.length - 1, epsilon, keepTail);
    for (let i = 0; i + 1 < tail.length; i += 1) if (keepTail[i]) keep[far + i] = 1;
  }

  const out = [];
  for (let i = 0; i < points.length; i += 1) if (keep[i]) out.push(points[i]);
  return out;
}

class Tracer {
  constructor() {
    this.skeleton = new Uint8Array(0);
    this.visited = new Uint8Array(0);
  }

  /// `mask` is width x height bytes, row 0 first. The contours come back in the
  /// same coordinates, x and y being pixel indices.
  trace(mask, width, height, params) {
    const out = [];
    if (width < 3 || height < 3) return out;

    const count = width * height;
    if (this.skeleton.length !== count) {
      this.skeleton = new Uint8Array(count);
      this.visited = new Uint8Array(count);
    }
    const skeleton = this.skeleton;
    this.visited.fill(0);

    for (let i = 0; i < count; i += 1) skeleton[i] = mask[i] >= params.threshold ? 1 : 0;

    // The border is cleared so every neighbourhood lookup below can skip its
    // bounds check. A mask that reaches the edge of the frame loses one pixel
    // of it, which is the edge of the frame and not an edge in the picture.
    for (let x = 0; x < width; x += 1) {
      skeleton[x] = 0;
      skeleton[(height - 1) * width + x] = 0;
    }
    for (let y = 0; y < height; y += 1) {
      skeleton[y * width] = 0;
      skeleton[y * width + width - 1] = 0;
    }

    this.thin(width, height);
    this.walk(width, height, params, out);
    return out;
  }

  /// Zhang-Suen. Two sub-iterations per pass, deleting in batches so a pass
  /// sees the image as it was and not as it is becoming. Capped rather than run
  /// to convergence: a band three pixels wide is thin after two passes, and a
  /// pathological mask must not be allowed to hold the frame.
  thin(width, height) {
    const skeleton = this.skeleton;
    const doomed = [];
    // Hoisted out of the innermost loop on purpose. Zhang-Suen visits every lit
    // texel of the mask up to thirty-two times, and allocating an
    // eight-element array inside that loop cost more than the whole rest of the
    // CPU chain put together — seven million allocations a frame at a Trace
    // Size of 640. The C++ has this as a stack array and pays nothing for it.
    const p = neighbourhood;
    let changed = true;

    for (let pass = 0; pass < 16 && changed; pass += 1) {
      changed = false;
      for (let sub = 0; sub < 2; sub += 1) {
        doomed.length = 0;
        for (let y = 1; y < height - 1; y += 1) {
          for (let x = 1; x < width - 1; x += 1) {
            if (skeleton[y * width + x] === 0) continue;

            let neighbours = 0;
            let transitions = 0;
            let previous = skeleton[(y + ZS_Y[7]) * width + x + ZS_X[7]];
            for (let k = 0; k < 8; k += 1) {
              p[k] = skeleton[(y + ZS_Y[k]) * width + x + ZS_X[k]];
              neighbours += p[k];
              if (previous === 0 && p[k] === 1) transitions += 1;
              previous = p[k];
            }

            if (neighbours < 2 || neighbours > 6 || transitions !== 1) continue;

            // P2 north, P4 east, P6 south, P8 west in Zhang-Suen's naming.
            const n = p[0];
            const e = p[2];
            const s = p[4];
            const w = p[6];
            const first = sub === 0 ? n * e * s === 0 : n * e * w === 0;
            const second = sub === 0 ? e * s * w === 0 : n * s * w === 0;
            if (first && second) doomed.push(y * width + x);
          }
        }
        for (const index of doomed) skeleton[index] = 0;
        if (doomed.length > 0) changed = true;
      }
    }
  }

  walk(width, height, params, out) {
    const skeleton = this.skeleton;
    const visited = this.visited;

    const degree = (x, y) => {
      let count = 0;
      for (let k = 0; k < 8; k += 1) count += skeleton[(y + NEIGHBOUR_Y[k]) * width + x + NEIGHBOUR_X[k]];
      return count;
    };

    const walkFrom = (startX, startY) => {
      const raw = [];
      let x = startX;
      let y = startY;

      for (;;) {
        visited[y * width + x] = 1;
        raw.push({ x, y });

        let nextX = -1;
        let nextY = -1;
        for (let k = 0; k < 8; k += 1) {
          const nx = x + NEIGHBOUR_X[k];
          const ny = y + NEIGHBOUR_Y[k];
          if (nx < 1 || ny < 1 || nx >= width - 1 || ny >= height - 1) continue;
          if (skeleton[ny * width + nx] === 0 || visited[ny * width + nx]) continue;
          nextX = nx;
          nextY = ny;
          break;
        }
        if (nextX < 0) break;
        x = nextX;
        y = nextY;
      }

      // Two-pixel specks are noise, not drawing, and nothing under four pixels
      // can be told closed from open.
      if (raw.length < 4) return;

      // Closed if the walk came back next to where it began. Eight-adjacency,
      // because the skeleton is eight-connected.
      const last = raw[raw.length - 1];
      const closed =
        raw.length >= 8 && Math.abs(last.x - raw[0].x) <= 1.0 && Math.abs(last.y - raw[0].y) <= 1.0;

      const reduced = simplifyPolyline(raw, params.simplify, closed);
      if (reduced.length < 2) return;
      if (polylineLength(reduced, closed) < params.minLength) return;

      out.push({ points: reduced, closed });
    };

    // Endpoints first, so an open line is one stroke and not two halves. Then
    // junctions, so the arms of a T are walked from the crossing outward. Then
    // anything unvisited, which is the closed loops.
    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x] && degree(x, y) === 1) walkFrom(x, y);

    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x] && degree(x, y) >= 3) walkFrom(x, y);

    for (let y = 1; y < height - 1; y += 1)
      for (let x = 1; x < width - 1; x += 1)
        if (skeleton[y * width + x] && !visited[y * width + x]) walkFrom(x, y);
  }
}

//===========================================================================
// A port of source/PointStream.cpp — ordered contours to an ILDA-style point
// stream.
//
// A laser frame is a list of points: where the mirrors are told to go, and
// whether the beam is on when they get there. Order, sample, dwell at corners,
// blank the jumps, decimate to a scan-rate floor — in the order a real show
// controller does them.
//===========================================================================

const COLOUR_CLIP = 0;
const COLOUR_PALETTE = 1;
const COLOUR_WHITE = 2;

const distanceSquared = (a, b) => (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);

/// HSV to RGB, h in turns.
function hsvToRgb(h, s, v) {
  h -= Math.floor(h);
  const i = Math.floor(h * 6.0);
  const f = h * 6.0 - i;
  const p = v * (1.0 - s);
  const q = v * (1.0 - s * f);
  const t = v * (1.0 - s * (1.0 - f));
  switch (i % 6) {
    case 0: return [v, t, p];
    case 1: return [q, v, p];
    case 2: return [p, v, t];
    case 3: return [p, q, v];
    case 4: return [t, p, v];
    default: return [v, p, q];
  }
}

function rgbToHsv(r, g, b) {
  const mx = Math.max(r, Math.max(g, b));
  const mn = Math.min(r, Math.min(g, b));
  const v = mx;
  const d = mx - mn;
  const s = mx > 1e-6 ? d / mx : 0.0;
  if (d < 1e-6) return [0.0, s, v];
  let h;
  if (mx === r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
  else if (mx === g) h = (b - r) / d + 2.0;
  else h = (r - g) / d + 4.0;
  return [h / 6.0, s, v];
}

/// One pass of the generator at a given density and dwell. Separated from
/// buildStream so the scan-rate floor can run it again, thinner.
function generate(contours, traceWidth, traceHeight, aspect, params, density, cornerDwell) {
  const out = [];

  const sx = aspect / Math.max(traceWidth, 1);
  const sy = 1.0 / Math.max(traceHeight, 1);
  const toUnits = (p) => ({ x: (p.x + 0.5) * sx, y: (p.y + 0.5) * sy });

  const cosCorner = Math.cos((kCornerAngleDegrees * Math.PI) / 180.0);

  const emit = (p, on) => {
    out.push({ x: p.x, y: p.y, r: 1.0, g: 1.0, b: 1.0, on });
  };

  // Is vertex `v` a corner? Compare the direction in with the direction out.
  // The first vertex of an OPEN contour has no direction in and is not one.
  const isCorner = (contour, v) => {
    const n = contour.points.length;
    if (!contour.closed && (v === 0 || v + 1 >= n)) return false;
    const a = toUnits(contour.points[v]);
    const prev = toUnits(contour.points[(v + n - 1) % n]);
    const next = toUnits(contour.points[(v + 1) % n]);
    const ix = a.x - prev.x;
    const iy = a.y - prev.y;
    const ox = next.x - a.x;
    const oy = next.y - a.y;
    const il = Math.sqrt(ix * ix + iy * iy);
    const ol = Math.sqrt(ox * ox + oy * oy);
    if (il <= 1e-6 || ol <= 1e-6) return false;
    return (ix * ox + iy * oy) / (il * ol) < cosCorner;
  };

  for (const contour of contours) {
    const n = contour.points.length;
    if (n < 2) continue;

    // Pre-blanking: the mirrors are sent to the start and given time to get
    // there before there is anything to see.
    const start = toUnits(contour.points[0]);
    for (let k = 0; k < params.blankSettle + cornerDwell; k += 1) emit(start, false);

    const segments = contour.closed ? n : n - 1;
    for (let s = 0; s < segments; s += 1) {
      const a = toUnits(contour.points[s]);
      const b = toUnits(contour.points[(s + 1) % n]);

      // A closed contour's first vertex is emitted at the ARRIVAL below, not
      // here. The beam passes through it once per scan, so it must be emitted
      // once per scan: emitting it at both the departure and the arrival gave
      // it one lit point more than every other corner, and `gvtest --dwell`
      // duly measured that one corner of a square as 19% brighter than the
      // other three. The arrival is the right end to keep, because that is
      // where the mirror is actually decelerating.
      if (!(contour.closed && s === 0)) {
        emit(a, true);
        if (isCorner(contour, s)) for (let k = 0; k < cornerDwell; k += 1) emit(a, true);
      }

      // Interior points along the segment, at the density. The endpoint is the
      // next segment's `a`, or the closing vertex below.
      const length = Math.sqrt(distanceSquared(a, b));
      const steps = Math.max(1, Math.ceil(length * density));
      for (let i = 1; i < steps; i += 1) {
        const t = i / steps;
        emit({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t }, true);
      }
    }

    // The arrival. A closed contour lands back on its first vertex and an open
    // one stops at its last, and both need the mirror to come to rest before
    // the beam goes out — so both get the dwell a corner gets. That is what a
    // real controller's end-of-line points are for, and it is why the end of a
    // stroke is as bright as a corner rather than fading.
    const endVertex = contour.closed ? 0 : n - 1;
    const end = toUnits(contour.points[endVertex]);
    emit(end, true);
    if (contour.closed ? isCorner(contour, 0) : true)
      for (let k = 0; k < cornerDwell; k += 1) emit(end, true);

    // Post-blanking: the beam is off before the mirrors leave.
    emit(end, false);
  }

  // Colour. Assigned after the geometry so a hue sweep can be spread over the
  // whole stream rather than per contour.
  const total = out.length;
  if (params.colourMode === COLOUR_PALETTE) {
    const [h, saturation, v] = rgbToHsv(params.colour[0], params.colour[1], params.colour[2]);
    // A hue sweep on a grey would be no sweep at all, so the moment there is a
    // spread the colour is taken at full saturation — which is what a laser's
    // colour is anyway.
    const s = params.hueSpread > 0.0 ? 1.0 : saturation;
    for (let i = 0; i < total; i += 1) {
      const along = total > 1 ? i / (total - 1) : 0.0;
      const rgb = hsvToRgb(h + params.hueSpread * along, s, v);
      out[i].r = rgb[0];
      out[i].g = rgb[1];
      out[i].b = rgb[2];
    }
  }
  // Clip and White both carry unit colour here. Clip is applied on the GPU,
  // where the clip actually is, by the trace shader.

  return out;
}

/// Reorder `contours` in place into a nearest-neighbour tour starting from
/// `cursor`, reversing open contours and rotating closed ones as needed.
function orderContours(contours, cursor) {
  const ordered = [];
  const taken = new Array(contours.length).fill(false);

  for (let placed = 0; placed < contours.length; placed += 1) {
    let best = contours.length;
    let bestAt = 0;
    let bestCost = 1e30;
    let bestReverse = false;

    for (let i = 0; i < contours.length; i += 1) {
      if (taken[i]) continue;
      const c = contours[i];
      if (c.closed) {
        // Any vertex can start a loop, so the nearest one does.
        for (let k = 0; k < c.points.length; k += 1) {
          const d = distanceSquared(c.points[k], cursor);
          if (d < bestCost) {
            bestCost = d;
            best = i;
            bestAt = k;
            bestReverse = false;
          }
        }
      } else {
        const df = distanceSquared(c.points[0], cursor);
        const db = distanceSquared(c.points[c.points.length - 1], cursor);
        if (df < bestCost) {
          bestCost = df;
          best = i;
          bestAt = 0;
          bestReverse = false;
        }
        if (db < bestCost) {
          bestCost = db;
          best = i;
          bestAt = 0;
          bestReverse = true;
        }
      }
    }

    if (best >= contours.length) break;

    taken[best] = true;
    const source = contours[best];
    let points = source.points.slice();
    if (source.closed && bestAt !== 0) points = points.slice(bestAt).concat(points.slice(0, bestAt));
    if (bestReverse) points.reverse();

    const c = { points, closed: source.closed };
    cursor = c.closed ? c.points[0] : c.points[c.points.length - 1];
    ordered.push(c);
  }

  return ordered;
}

/// Build the stream. Returns `{ stream, wanted }` — `wanted` being the number
/// of points the stream would have had before any decimation, so a caller can
/// report how far over budget the frame was.
function buildStream(contours, traceWidth, traceHeight, aspect, params) {
  let out = generate(contours, traceWidth, traceHeight, aspect, params, params.density, params.cornerDwell);
  const wanted = out.length;

  if (params.scanRateFloorHz <= 0.0 || params.pointsPerSecond <= 0.0) return { stream: out, wanted };

  const budget = Math.max(1, Math.floor(Math.max(1.0, params.pointsPerSecond / params.scanRateFloorHz)));
  if (wanted <= budget) return { stream: out, wanted };

  // Over budget: thin the density and the dwell in proportion and try again.
  // Twice, because the per-segment ceil() and the fixed blanking points do not
  // scale, so the first attempt lands a little over. Whatever is still over
  // after that is cut from the end, which is what a real controller does when a
  // frame will not fit: the tail of the drawing is simply not drawn.
  let scale = budget / wanted;
  for (let attempt = 0; attempt < 2 && out.length > budget; attempt += 1) {
    const density = Math.max(1.0, params.density * scale * 0.95);
    const dwell = Math.floor(params.cornerDwell * scale);
    out = generate(contours, traceWidth, traceHeight, aspect, params, density, dwell);
    scale *= budget / Math.max(out.length, 1);
  }
  if (out.length > budget) out.length = budget;

  return { stream: out, wanted };
}

//===========================================================================
// A port of source/Scanner.cpp — two mirrors, a point clock, a frame budget.
//
// Each mirror is a second-order system: a mass on a torsion spring driven by a
// servo, integrated with RK4 at four substeps per point. A corner is rounded
// because the mirror cannot change direction in zero time; an under-damped
// mirror overshoots and rings back through it; a jump takes a few points to
// complete, so a beam that comes on early draws the hook at the start of a
// stroke.
//
// The point clock is the host's clock. A stream longer than one frame's budget
// is scanned across several frames, continuing where it left off, and the
// picture visibly crawls — which is what a real projector does with a frame
// that is too busy.
//===========================================================================

const SUBSTEPS = 4;

/// Samples are written straight into the layout the trace pass reads: eight
/// floats, two attributes per instance, the same `struct Sample` the plugin
/// uploads. Growable so the buffer is allocated once rather than per frame.
class SampleBuffer {
  constructor() {
    this.data = new Float32Array(8 * 8192);
    this.count = 0;
  }

  clear() {
    this.count = 0;
  }

  push(x, y, dt, on, r, g, b) {
    if ((this.count + 1) * 8 > this.data.length) {
      const grown = new Float32Array(this.data.length * 2);
      grown.set(this.data);
      this.data = grown;
    }
    const at = this.count * 8;
    const d = this.data;
    d[at] = x;
    d[at + 1] = y;
    d[at + 2] = dt;
    d[at + 3] = on;
    d[at + 4] = r;
    d[at + 5] = g;
    d[at + 6] = b;
    d[at + 7] = 0;
    this.count += 1;
  }
}

class Scanner {
  constructor() {
    this.pointsPerSecond = 30000.0;
    this.omega = 21428.0;
    this.zeta = 0.7;
    this.blankingDelay = 0;
    this.restart = false;
    this.reset();
  }

  reset() {
    this.position = [0.0, 0.0];
    this.velocity = [0.0, 0.0];
    this.current = [];
    this.pending = [];
    this.hasPending = false;
    this.cursor = 0;
    this.carry = 0.0;
    this.scans = 0;
    this.pointsScanned = 0;
    this.last = null;
  }

  setPointRate(pps) {
    this.pointsPerSecond = Math.max(pps, 1.0);
  }

  setResponse(naturalFrequency, damping) {
    this.omega = Math.max(naturalFrequency, 1.0);
    this.zeta = Math.max(damping, 0.05);
  }

  setStream(stream) {
    this.pending = stream;
    this.hasPending = true;

    // Nothing is being scanned, so there is nothing to wait for the end of.
    if (this.current.length === 0) {
      this.current = this.pending;
      this.pending = [];
      this.hasPending = false;
      this.cursor = 0;
    }
  }

  mirrorPosition() {
    return { x: this.position[0], y: this.position[1] };
  }

  /// One RK4 step of x'' = omega^2 (u - x) - 2 zeta omega x', with the command
  /// u held constant across the step — which it is: the DAC holds each point
  /// for a whole point period and the substeps subdivide that.
  step(target, h) {
    const w2 = this.omega * this.omega;
    const c = 2.0 * this.zeta * this.omega;

    for (let axis = 0; axis < 2; axis += 1) {
      const u = target[axis];
      const x = this.position[axis];
      const v = this.velocity[axis];

      const accel = (px, pv) => w2 * (u - px) - c * pv;

      const k1x = v;
      const k1v = accel(x, v);
      const k2x = v + 0.5 * h * k1v;
      const k2v = accel(x + 0.5 * h * k1x, v + 0.5 * h * k1v);
      const k3x = v + 0.5 * h * k2v;
      const k3v = accel(x + 0.5 * h * k2x, v + 0.5 * h * k2v);
      const k4x = v + h * k3v;
      const k4v = accel(x + h * k3x, v + h * k3v);

      this.position[axis] = x + (h / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
      this.velocity[axis] = v + (h / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
    }
  }

  /// Scan one host frame's worth of points and append the samples produced. The
  /// first sample appended is the previous frame's last one, so the intervals
  /// chain across frames without a gap.
  advance(frameSeconds, out) {
    if (this.restart) {
      // Every host frame starts the newest stream from its first point.
      if (this.hasPending) {
        this.current = this.pending;
        this.pending = [];
        this.hasPending = false;
      }
      this.cursor = 0;
    }

    // The budget: pps * dt points, with the fraction carried so the average
    // rate is exact over many frames rather than rounded down every frame.
    const exact = this.pointsPerSecond * Math.max(frameSeconds, 0.0) + this.carry;
    const budget = Math.floor(exact);
    this.carry = exact - budget;

    if (this.last !== null) {
      const l = this.last;
      out.push(l[0], l[1], l[2], l[3], l[4], l[5], l[6]);
    }

    if (this.current.length === 0) {
      // Nothing to draw. The mirrors sit; the clock still runs.
      this.pointsScanned += budget;
      return budget;
    }

    const period = 1.0 / this.pointsPerSecond;
    const h = period / SUBSTEPS;

    for (let i = 0; i < budget; i += 1) {
      // Re-read every point: a scan boundary below may have swapped in a
      // stream of a different length.
      const size = this.current.length;
      const p = this.current[this.cursor];

      // The blanking signal, shifted against the position signal. Modular,
      // because the stream loops: the point before the first is the last.
      let blankIndex = (this.cursor - this.blankingDelay) % size;
      if (blankIndex < 0) blankIndex += size;
      const on = this.current[blankIndex].on ? 1.0 : 0.0;

      const target = [p.x, p.y];
      for (let s = 0; s < SUBSTEPS; s += 1) {
        this.step(target, h);
        out.push(this.position[0], this.position[1], h, on, p.r, p.g, p.b);
      }

      this.pointsScanned += 1;
      this.cursor += 1;
      if (this.cursor >= this.current.length) {
        this.cursor = 0;
        this.scans += 1;
        // A scan boundary is where a new frame is taken up, so a frame that
        // takes three host frames to draw is drawn whole and then replaced,
        // rather than torn between two traces.
        if (this.hasPending) {
          this.current = this.pending;
          this.pending = [];
          this.hasPending = false;
          if (this.current.length === 0) break;
        }
      }
    }

    if (out.count > 0) {
      const at = (out.count - 1) * 8;
      const d = out.data;
      this.last = [d[at], d[at + 1], d[at + 2], d[at + 3], d[at + 4], d[at + 5], d[at + 6]];
    }

    return budget;
  }
}

//===========================================================================
// The renderer: the plugin's seven passes, in ProcessOpenGL's order.
//
//   1. copy       picture size, mipmapped. MaxUV folded in once.
//   2. edge       TRACE size. Sobel at the mip level matching it, plus Detail.
//   3. stabilise  trace size, ping-ponged. Asymmetric filter, then threshold.
//   4. readback   one readPixels of the small mask, then the CPU chain above.
//   5. decay      picture size. The accumulation buffer times the persistence.
//   6. trace      one instanced quad per scanner interval, additive, on top.
//   7. composite  background mode and mix, to the canvas.
//===========================================================================

/// A runaway cannot be allowed to climb to the top of a 16-bit float and become
/// an inf. Far above anything a picture holds, so it never shapes one. Beam.cpp.
const BEAM_CEILING = 6.0e4;

/// What the stats line under the canvas reports. Filled by the renderer, read
/// by a timer — see the note where it is mounted.
const telemetry = {
  contours: 0,
  stream: 0,
  wanted: 0,
  budget: 0,
  trace: '',
  cpuMillis: 0,
};

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const edgeShader = new Program(gl, VERTEX, EDGE, 'edge');
  const stabiliseShader = new Program(gl, VERTEX, STABILISE, 'stabilise');
  const decayShader = new Program(gl, VERTEX, DECAY, 'decay');
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');
  // The trace pass sources its own geometry: four vec4 instance attributes, two
  // per sample, not the screen quad's position and uv.
  const traceShader = new Program(gl, TRACE_VERTEX, TRACE_FRAGMENT, 'trace', {
    attribs: { sampleA: 0, colourA: 1, sampleB: 2, colourB: 3 },
  });

  const copyBuffer = new PassBuffer(gl, { filter: 'linear', mip: true });
  const edgeBuffer = new PassBuffer(gl, { filter: 'nearest' });
  const stableBuffer = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
  const accumulation = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];

  //-----------------------------------------------------------------------
  // One buffer of samples, read twice.
  //
  // Attributes 0/1 start at the beginning and 2/3 one Sample in, so instance i
  // sees sample i and sample i+1 with nothing duplicated. That is why the draw
  // asks for n-1 instances: n would read one Sample past the end.
  //
  // vertexAttribDivisor is VAO state, not global state, so it has to be set
  // with this VAO bound. Set it with the wrong one bound and the attributes
  // silently become per-vertex, which looks like corrupt geometry.
  //-----------------------------------------------------------------------
  const STRIDE = 32;
  const traceVAO = gl.createVertexArray();
  const traceVBO = gl.createBuffer();
  gl.bindVertexArray(traceVAO);
  gl.bindBuffer(gl.ARRAY_BUFFER, traceVBO);
  for (let i = 0; i < 4; i += 1) {
    gl.enableVertexAttribArray(i);
    gl.vertexAttribPointer(i, 4, gl.FLOAT, false, STRIDE, i * 16);
    gl.vertexAttribDivisor(i, 1);
  }
  gl.bindVertexArray(null);
  gl.bindBuffer(gl.ARRAY_BUFFER, null);

  const tracer = new Tracer();
  const scanner = new Scanner();
  const samples = new SampleBuffer();

  let mask = new Uint8Array(0);
  let readback = new Uint8Array(0);
  let contours = [];

  let stableCurrent = 0;
  let accumulationCurrent = 0;
  let historyValid = false;
  let lastTime = null;
  let lastDetail = null;
  let lastSource = null;

  return {
    render({ input, params, width, height, time }) {
      //------------------------------------------------------------------
      // Time. Galvo.cpp normalises the host's clock to seconds and takes the
      // frame delta from it, clamped; that delta is the scanner's budget. The
      // clamp is not optional here either — a backgrounded tab's multi-second
      // gap would otherwise ask the scanner for a minute of points in one go.
      //------------------------------------------------------------------
      const raw = lastTime === null ? 1 / 60 : time - lastTime;
      lastTime = time;
      const frameSeconds = clamp(raw, kMinFrame, kMaxFrame);

      const p = (id) => params.get(id);
      const aspect = width / height;

      const traceWidth = clamp(Math.round(integerValue('traceSize', p('traceSize'))), 16, 2048);
      const traceHeight = Math.max(8, Math.round(traceWidth / aspect));

      //------------------------------------------------------------------
      // Buffers. `PassBuffer.ensure()` returns `this` on both paths, so a
      // resize is detected by remembering the size rather than by asking it.
      //------------------------------------------------------------------
      const sizeChanged = stableBuffer[0].width !== traceWidth || stableBuffer[0].height !== traceHeight;
      const beamResized = accumulation[0].width !== width || accumulation[0].height !== height;

      copyBuffer.ensure(width, height, gl.RGBA16F);
      edgeBuffer.ensure(traceWidth, traceHeight, gl.RGBA16F);
      // RGBA8 and not RGBA16F, for the one reason at the top of this file:
      // WebGL2 will not readPixels a float framebuffer as bytes, and the mask
      // is a byte per texel in the plugin too.
      stableBuffer[0].ensure(traceWidth, traceHeight, gl.RGBA8);
      stableBuffer[1].ensure(traceWidth, traceHeight, gl.RGBA8);
      accumulation[0].ensure(width, height, gl.RGBA16F);
      accumulation[1].ensure(width, height, gl.RGBA16F);

      if (sizeChanged) historyValid = false;
      if (beamResized) {
        // A fresh allocation has no history in it.
        accumulation[0].clearTo(0, 0, 0, 0);
        accumulation[1].clearTo(0, 0, 0, 0);
      }

      // Changing what an edge *is* invalidates the history, because the numbers
      // being blended are no longer measuring the same thing. SetFloatParameter
      // does this in the plugin; there is no parameter callback here, so it is
      // noticed instead.
      if (p('detail') !== lastDetail || p('source') !== lastSource) historyValid = false;
      lastDetail = p('detail');
      lastSource = p('source');

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The picture, into a texture of ours, with a mip chain on it.
      //------------------------------------------------------------------
      copyBuffer.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      // The host hands an FFGL plugin a texture the picture may not fill; here
      // it always does, so MaxUV is 1.
      copyShader.set('MaxUV', 1, 1);
      copyShader.set('HalfTexel', 0.5 / input.width, 0.5 / input.height);
      quad.draw();
      copyBuffer.generateMipmap();

      //------------------------------------------------------------------
      // 2. Edge, at the trace resolution.
      //------------------------------------------------------------------
      {
        // The mip level that matches the trace resolution, plus Detail. The
        // taps and the level move together: detecting at level 2 with taps one
        // level-0 texel apart samples the same texel three times.
        const baseLod = Math.log2(Math.max(1.0, width / traceWidth));
        const detail = detailFromParam(p('detail'));
        const spread = Math.pow(2, detail);

        edgeBuffer.bind();
        edgeShader.use();
        bindTexture(gl, 0, copyBuffer.texture);
        edgeShader.setSampler('CopyTexture', 0);
        edgeShader.set('Step', spread / traceWidth, spread / traceHeight);
        edgeShader.set('Lod', baseLod + detail);
        edgeShader.set('SourceMode', p('source'));
        quad.draw();
      }

      //------------------------------------------------------------------
      // 3. Stabilise, ping-ponged against the previous frame's result.
      //------------------------------------------------------------------
      const history = stableCurrent;
      const target = 1 - stableCurrent;
      {
        stableBuffer[target].bind();
        stabiliseShader.use();
        bindTexture(gl, 0, edgeBuffer.texture);
        bindTexture(gl, 1, stableBuffer[history].texture);
        stabiliseShader.setSampler('EdgeTexture', 0);
        stabiliseShader.setSampler('HistoryTexture', 1);
        stabiliseShader.set('Attack', attackFromParam(p('stability')));
        stabiliseShader.set('Release', releaseFromParam(p('stability')));
        stabiliseShader.set('Threshold', thresholdFromParam(p('threshold')));
        stabiliseShader.set('Softness', kThresholdSoftness);
        stabiliseShader.set('Reset', historyValid ? 0.0 : 1.0);
        quad.draw();
      }
      stableCurrent = target;
      historyValid = true;

      //------------------------------------------------------------------
      // 4. Read the mask back and trace it.
      //
      // This is the pipeline stall the plugin's own notes single out as its
      // biggest cost, and it is the same stall here: readPixels waits for
      // every command above it to finish. It is kept small — the mask is BORN
      // at the trace resolution rather than shrunk to it — and the CPU chain
      // that follows is plain arithmetic over a few tens of kilobytes.
      //------------------------------------------------------------------
      const cpuStart = performance.now();
      const texels = traceWidth * traceHeight;
      if (mask.length !== texels) {
        mask = new Uint8Array(texels);
        readback = new Uint8Array(texels * 4);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, stableBuffer[target].fbo);
      gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, traceWidth, traceHeight, gl.RGBA, gl.UNSIGNED_BYTE, readback);
      for (let i = 0; i < texels; i += 1) mask[i] = readback[i * 4];

      contours = tracer.trace(mask, traceWidth, traceHeight, {
        threshold: 128,
        simplify: simplifyFromParam(p('simplify')),
        minLength: minLengthFromParam(p('minLength')),
      });

      // The tour starts from where the mirrors are now, in trace pixels.
      const mirror = scanner.mirrorPosition();
      contours = orderContours(contours, {
        x: (mirror.x / aspect) * traceWidth - 0.5,
        y: mirror.y * traceHeight - 0.5,
      });

      const colourMode = Math.round(p('colourMode'));
      const pointsPerSecond = 1000.0 * integerValue('galvoSpeed', p('galvoSpeed'));

      const built = buildStream(contours, traceWidth, traceHeight, aspect, {
        density: densityFromParam(p('density')),
        cornerDwell: integerValue('cornerDwell', p('cornerDwell')),
        blankSettle: kBlankSettle,
        colourMode,
        colour: [p('colourR'), p('colourG'), p('colourB')],
        hueSpread: hueSpreadFromParam(p('hueSpread')),
        pointsPerSecond,
        scanRateFloorHz: scanRateFloorFromParam(p('scanFloor')),
      });
      scanner.setStream(built.stream);

      //------------------------------------------------------------------
      // 5. Scan this host frame's worth of points.
      //------------------------------------------------------------------
      scanner.setPointRate(pointsPerSecond);
      scanner.setResponse(omegaFromPointRate(pointsPerSecond), dampingFromParam(p('damping')));
      scanner.blankingDelay = integerValue('blankingDelay', p('blankingDelay'));
      scanner.restart = Math.round(p('frameSync')) === 1;

      samples.clear();
      const budget = scanner.advance(frameSeconds, samples);

      // What the readback, the trace, the ordering, the stream and the scan
      // cost. `gvtest --bench` reports the same number for the plugin, and the
      // readback is the part worth watching in both: it is a pipeline stall.
      telemetry.cpuMillis = performance.now() - cpuStart;
      telemetry.contours = contours.length;
      telemetry.stream = built.stream.length;
      telemetry.wanted = built.wanted;
      telemetry.budget = budget;
      telemetry.trace = `${traceWidth} × ${traceHeight}`;

      //------------------------------------------------------------------
      // 6. Light along the path: decay, then deposit.
      //------------------------------------------------------------------
      const beamTarget = 1 - accumulationCurrent;
      const beamHistory = accumulationCurrent;
      const n = samples.count;
      const segments = Math.max(0, n - 1);

      if (n > 0) {
        // STREAM_DRAW and a fresh bufferData every frame, so the driver orphans
        // the old storage rather than waiting for last frame's draw to finish
        // reading it.
        gl.bindBuffer(gl.ARRAY_BUFFER, traceVBO);
        gl.bufferData(gl.ARRAY_BUFFER, samples.data.subarray(0, n * 8), gl.STREAM_DRAW);
        gl.bindBuffer(gl.ARRAY_BUFFER, null);
      }

      const tau = persistenceTauFromParam(p('persistence'));
      const beamPower = beamPowerFromParam(p('brightness'));
      const spotSigma = Math.max(spotSigmaFromParam(p('spot')), 1e-5);

      accumulation[beamTarget].bind();
      gl.disable(gl.BLEND);
      decayShader.use();
      bindTexture(gl, 0, accumulation[beamHistory].texture);
      decayShader.setSampler('HistoryTexture', 0);
      decayShader.set('Decay', tau > 0.0 ? Math.exp(-frameSeconds / tau) : 0.0);
      decayShader.set('Ceiling', BEAM_CEILING);
      quad.draw();

      if (segments > 0 && beamPower > 0.0) {
        // Sum, not max: two intervals crossing the same texel really did put
        // twice the light there, and a max() would throw away every crossing
        // and every dwell — which is where a laser frame is brightest and the
        // reason this plugin exists.
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE);

        traceShader.use();
        bindTexture(gl, 0, copyBuffer.texture);
        traceShader.setSampler('CopyTexture', 0);
        traceShader.set('BeamPower', beamPower);
        traceShader.set('SpotSigma', spotSigma);
        traceShader.set('Aspect', Math.max(aspect, 1e-3));
        traceShader.set('ColourMode', colourMode);
        // The spot's diameter in picture pixels, as a mip level, so the clip is
        // averaged over about as much of itself as the beam covers.
        traceShader.set('ColourLod', Math.log2(Math.max(1.0, spotSigma * 2.0 * height)));
        traceShader.set('Saturate', kLaserSaturation);

        gl.bindVertexArray(traceVAO);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, segments);
        gl.bindVertexArray(null);
        gl.disable(gl.BLEND);
      }

      accumulationCurrent = beamTarget;

      //------------------------------------------------------------------
      // 7. Composite, straight to the canvas. The kit bound it and set the
      //    viewport before calling us, and five framebuffers of other sizes
      //    have been bound since.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      compositeShader.use();
      bindTexture(gl, 0, copyBuffer.texture);
      bindTexture(gl, 1, accumulation[accumulationCurrent].texture);
      bindTexture(gl, 2, stableBuffer[target].texture);
      compositeShader.setSampler('CopyTexture', 0);
      compositeShader.setSampler('BeamTexture', 1);
      compositeShader.setSampler('StableTexture', 2);
      compositeShader.set('Background', p('background'));
      compositeShader.set('Dim', kDimLevel);
      compositeShader.set('MixAmount', p('mix'));
      quad.draw();
    },
  };
}

//===========================================================================
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: the About block, which is four buttons that open a browser.
//===========================================================================

/// The integer-typed parameters. FF_TYPE_INTEGER is exempt from the 0..1 clamp,
/// so the plugin stores these as the integer itself, in its own units — and the
/// demo kit has no integer control, so they are dropdowns of their own values
/// here. The index into the dropdown is not the plugin's value; `integerValue`
/// converts.
///
/// Trace Size is the one that is thinned: the plugin takes every integer from
/// 160 to 640 and a dropdown of 481 entries is not a control. Every twentieth
/// one, and 320 — the default — is among them.
const INTEGER_RANGES = {
  traceSize: [160, 640, 20],
  galvoSpeed: [8, 60, 1],
  cornerDwell: [0, 8, 1],
  blankingDelay: [-8, 8, 1],
};

const INTEGER_ELEMENTS = {};
for (const [id, [low, high, stride]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += stride) INTEGER_ELEMENTS[id].push(String(v));
}

const integerValue = (id, index) => {
  const [low, , stride] = INTEGER_RANGES[id];
  const last = INTEGER_ELEMENTS[id].length - 1;
  return low + stride * clamp(Math.round(index), 0, last);
};
const integerIndex = (id, value) => Math.round((value - INTEGER_RANGES[id][0]) / INTEGER_RANGES[id][2]);

const std = (id, name, def, group, extra = {}) => ({
  id,
  name,
  type: 'standard',
  default: def,
  group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});

const opt = (id, name, elements, def, group, hint) => ({
  id,
  name,
  type: 'option',
  elements,
  default: def,
  group,
  hint,
});

const integer = (id, name, value, group, hint) => ({
  id,
  name,
  type: 'option',
  elements: INTEGER_ELEMENTS[id],
  default: integerIndex(id, value),
  group,
  hint,
});

const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const demo = mountDemo({
  name: 'Galvo',
  pluginId: 'GV01',
  tagline:
    'An ILDA laser projector. The clip’s outlines are traced into contours, ordered into a tour, sampled into a point stream with dwell at every corner, and scanned by two second-order galvanometer mirrors at a fixed point rate — and the picture is where the beam went, at the brightness its dwell time gave it. Nothing is drawn: the bright dots on corners, the rounding at speed, the hooks from a mis-set blanking delay and the flicker of a frame too busy for the scanner all fall out of the mirrors. The shaders here are the plugin’s own; the tracer, the point stream and the galvo model are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/galvo',

  // Background = Alpha puts the light over nothing, premultiplied, for the
  // layer below — so what sits behind it is a real question.
  showBackdrop: true,

  // The accumulation buffer is RGBA16F and the trace pass adds into it by
  // blending, which needs a float render target. Eight bits mid-chain would
  // quantise the deposit, and the deposit is the whole claim: a corner is
  // brighter than a line because the mirror spent more intervals there, and a
  // quantiser would be what decided by how much.
  needFloat: true,

  params: [
    opt('source', 'Detect On', ['Luma', 'Alpha', 'Chroma', 'Luma or Alpha'], 3, 'Detect',
      'What "different" means between two pixels. A logo delivered with alpha has a perfect edge already in its alpha channel, and a luma Sobel over it throws away the only clean signal in the frame. Chroma finds the boundary between two colours of equal brightness, which luma is blind to.'),
    std('detail', 'Detail', 0.2, 'Detect', {
      display: (v) => `${detailFromParam(v).toFixed(2)} mip levels`,
      hint: 'Mip levels above the trace resolution that the Sobel runs at. 0 detects at the trace buffer’s own pixel; 3 finds only the shape of a logo and ignores everything inside it.',
    }),
    std('threshold', 'Threshold', 0.65, 'Detect', {
      display: (v) => thresholdFromParam(v).toFixed(3),
      hint: 'The gradient magnitude at which a pixel is an edge. A clean black-to-white step measures exactly 1.0, so the useful range is below 0.2 for footage and around 0.3 for artwork. Set it with Background on Edge Mask — judging a threshold through a layer of beam is guesswork.',
    }),
    std('stability', 'Stability', 0.35, 'Detect', {
      display: (v) => `release ${releaseFromParam(v).toFixed(3)}`,
      hint: 'An asymmetric temporal filter, not a low-pass. Edges DROP OUT for a frame on footage and the contour the tracer found vanishes and comes back, so an edge that appears is believed at once and an edge that vanishes is given a few frames to return.',
    }),
    integer('traceSize', 'Trace Size', 320, 'Detect',
      'The width of the mask the CPU walks. This is what the page reads back from the GPU every frame, one byte per texel, exactly as the plugin does — the mask is born at this size rather than shrunk to it. Larger finds finer outlines and costs a bigger readback and a longer walk.'),
    std('minLength', 'Min Length', 0.15, 'Detect', {
      display: (v) => `${minLengthFromParam(v).toFixed(1)} px`,
      hint: 'Contours shorter than this, in trace pixels after simplification, are not scanned. Specks on footage are short; letters are not.',
    }),
    std('simplify', 'Simplify', 0.25, 'Detect', {
      display: (v) => `${simplifyFromParam(v).toFixed(2)} px`,
      hint: 'The Douglas-Peucker tolerance: how far the simplified polyline may stray from the traced one. Below about 0.7 the staircase of a thresholded mask survives as a stitch along every edge.',
    }),

    integer('galvoSpeed', 'Galvo Speed', 30, 'Scanner',
      'kpps, like the number on the side of a real scanner. 8 is a hobby galvo, 60 is a good one. It sets both the point budget per frame and — through the settling-time rule in Controls.cpp — the mirrors’ natural frequency, so a slow scanner both rounds corners more and runs out of points sooner.'),
    std('damping', 'Damping', 0.375, 'Scanner', {
      display: (v) => `ζ ${dampingFromParam(v).toFixed(2)}`,
      hint: 'The galvo’s damping ratio. Under 1 overshoots and rings through every corner; over 1 rounds every corner without ringing. 0.7 is the default and is what the ILDA test pattern assumes.',
    }),
    std('density', 'Density', 0.54, 'Scanner', {
      display: (v) => `${densityFromParam(v).toFixed(0)} pts/height`,
      hint: 'Points per unit of path length, the unit being the picture height because the spot is. More points is a smoother line and a longer frame — which is how you run out of scanner.',
    }),
    integer('cornerDwell', 'Corner Dwell', 3, 'Scanner',
      'Extra copies of a sharp vertex, so the mirrors have time to turn. That repeat is what puts the bright dot on every corner of a laser-drawn square: the beam is on, and it is not going anywhere.'),
    integer('blankingDelay', 'Blanking Delay', 0, 'Scanner',
      'Points the blanking signal is delayed by relative to the position signal. Negative brings the beam on early and draws the HOOK of the approach; positive leaves it on into the next jump and draws a TAIL off the end of the stroke. Nothing draws those — they are what a mis-timed gun does.'),
    std('scanFloor', 'Scan Rate Floor', 0.0, 'Scanner', {
      display: (v) => (scanRateFloorFromParam(v) === 0 ? 'off' : `${scanRateFloorFromParam(v).toFixed(0)} Hz`),
      hint: 'The slowest scan rate allowed. When the frame has more points than the scanner can draw at that rate it is thinned to fit, the way show software does with a frame that is too busy. Off lets the picture crawl instead.',
    }),
    opt('frameSync', 'Frame Sync', ['Free Running', 'Restart Each Frame'], 0, 'Scanner',
      'Free Running carries on where it left off, so a frame that takes three host frames to draw is drawn whole and the picture crawls. Restart Each Frame begins afresh every host frame instead, so a too-long frame is cut off rather than crawled through.'),

    std('spot', 'Spot', 0.35, 'Beam', {
      display: (v) => `σ ${spotSigmaFromParam(v).toFixed(4)}`,
      hint: 'The spot’s Gaussian sigma as a fraction of the picture height. 0.0015 of 1080 lines is 1.6 pixels, which is where a spot stops being a spot and starts being a pixel.',
    }),
    std('brightness', 'Brightness', 0.5, 'Beam', {
      display: (v) => `${beamPowerFromParam(v).toFixed(2)} light/s`,
      hint: 'Light per second of beam-on time. Nothing anywhere divides by a speed: each scanner interval deposits BeamPower × dt spread over however far the mirrors moved in it, and brightness proportional to dwell falls out as the definition of that.',
    }),
    std('persistence', 'Persistence', 0.0, 'Beam', {
      display: (v) => (persistenceTauFromParam(v) === 0 ? 'off' : `τ ${persistenceTauFromParam(v).toFixed(3)} s`),
      hint: 'A camera’s shutter, not a phosphor: the accumulation buffer keeps exp(-dt/τ) of itself each frame. There is no cascade and no colour shift, only a shutter open for longer than one frame.',
    }),
    opt('colourMode', 'Colour Mode', ['Clip', 'Palette', 'White'], 0, 'Beam',
      'Clip reads the picture under the middle of each interval BLURRED, at a mip level matched to the spot, and pushes it toward a laser primary. Blurred because a contour runs along a boundary, which is the one place in the picture where a point sample is a coin flip between the two sides.'),
    colour('colourR', 'Colour', 0.1, 'Beam', 'Palette mode. A laser’s colour, not a tint over the clip.'),
    colour('colourG', 'Colour_Green', 1.0, 'Beam'),
    colour('colourB', 'Colour_Blue', 0.2, 'Beam'),
    std('hueSpread', 'Hue Spread', 0.0, 'Beam', {
      display: (v) => `${hueSpreadFromParam(v).toFixed(2)} turns`,
      hint: 'Turns of hue from the first point of the stream to the last, Palette mode only. Spread over the whole tour rather than per contour, so it reads as one sweep through the drawing.',
    }),

    opt('background', 'Background', ['Black', 'Clip', 'Dimmed Clip', 'Alpha', 'Edge Mask'], 0, 'Output',
      'Edge Mask is not a look: it is how Threshold, Detail and Trace Size are actually set, because judging a threshold through a layer of beam is guesswork. Alpha puts the light over nothing, premultiplied, for the layer below.'),
    std('mix', 'Mix', 1.0, 'Output'),
  ],

  // The clips with real outlines in them. Galvo is for logos, titles and line
  // art — a photograph gives it a thousand short contours and it says so by
  // running out of scanner.
  sources: ['alpha', 'spot', 'grid', 'bars', 'scene', 'detail'],

  // The plugin ships no factory presets, so unlike macroblock's and vectrix's
  // these are the page's own — expressed entirely in the plugin's parameters
  // and reachable with the sliders.
  presets: {
    'Edge Mask (set the threshold here)': { background: 4, mix: 1 },
    'Green laser': { colourMode: 1, colourR: 0.1, colourG: 1, colourB: 0.2 },
    'Hue sweep': { colourMode: 1, colourR: 1, colourG: 0.1, colourB: 0.1, hueSpread: 0.5 },
    'Hobby galvo (8 kpps)': { galvoSpeed: integerIndex('galvoSpeed', 8), damping: 0.2 },
    'Ringing mirrors': { galvoSpeed: integerIndex('galvoSpeed', 10), damping: 0.0 },
    // The one worth loading first. An 8 kpps scanner draws 133 points in a
    // 60 fps frame; these settings ask it for nearly two thousand, so the
    // stream is scanned across a dozen host frames and the picture crawls into
    // existence a piece at a time. The line under the canvas has the numbers.
    'Too busy to draw': {
      galvoSpeed: integerIndex('galvoSpeed', 8),
      density: 0.95,
      minLength: 0.05,
      simplify: 0.05,
    },
    // What show software does about it: thin the frame until it fits.
    'Scan rate floor on': {
      galvoSpeed: integerIndex('galvoSpeed', 8),
      density: 0.95,
      minLength: 0.05,
      simplify: 0.05,
      scanFloor: 0.5,
    },
    'Hooks and tails': { blankingDelay: integerIndex('blankingDelay', -5), galvoSpeed: integerIndex('galvoSpeed', 12) },
    'Long exposure': { persistence: 0.62, brightness: 0.34, galvoSpeed: integerIndex('galvoSpeed', 12) },
    'Over the clip': { background: 2, colourMode: 2, brightness: 0.42 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Galvo is not a shader: it is a GPU edge detector, a synchronous readback, and then a tracer, a nearest-neighbour tour, a point-stream generator and a second-order galvo model in plain C++ — Tracer.cpp, PointStream.cpp, Scanner.cpp and Controls.cpp. All four are ported here function for function, because without them the page would have nothing to show. Nothing checks a port but a reader; the repository’s gvtest --trace, --step, --budget, --energy and --dwell check the C++ and have never heard of this page.',
    'The beam renderer is not a port. The decay and trace passes are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the nine shaders drifts.',
    'The page traces at the plugin’s own Trace Size — 320 × 180 by default, 640 × 360 at most — and reads that mask back with readPixels every frame, which stalls the pipeline exactly as the plugin’s glReadPixels does. The two stabilise buffers are RGBA8 here rather than RGBA16F, because WebGL2 will not read a float framebuffer as bytes. The mask itself is a byte in both; what is coarser here is the pre-threshold value Stability feeds back to itself.',
    'Galvo Speed, Trace Size, Corner Dwell and Blanking Delay are FF_TYPE_INTEGER in the plugin and carry real units. The demo kit has no integer control, so they are dropdowns of their own values — and Trace Size offers every twentieth pixel from 160 to 640 rather than all 481.',
    'Push it past its budget and watch. Load Too busy to draw: an 8 kpps scanner draws 133 points per frame at 60 fps, the frame wants thousands, and the stream is scanned across many host frames — so the picture crawls into existence a piece at a time and never completes. That is not the page struggling; it is what a real projector does, and Scan rate floor on is the answer show software uses. The line under the canvas reports the numbers.',
    'There is no audio caveat on this page, which is worth saying because every other demo in this suite has one: Galvo has no audio path at all.',
    'The plugin’s numerical proof — the tracer on synthetic masks, the mirror’s overshoot against the closed form, the point budget against ceil(N / (P/fps)), and the light in a frame proved independent of the galvo to within half a percent — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The stats line.
//
// Not decoration and not a measurement: it is the only way a visitor can see
// the thing this plugin is most distinctive for. When the stream is longer than
// one frame's point budget the picture crawls, and without these numbers that
// reads as the page being broken rather than as the scanner being out of
// points. Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);

    const number = (n) => n.toLocaleString('en-GB');

    setInterval(() => {
      const { contours, stream, wanted, budget, trace, cpuMillis } = telemetry;
      const cost = `${cpuMillis.toFixed(1)} ms on the CPU half`;
      if (stream === 0) {
        line.textContent =
          `Tracing at ${trace || '—'}: nothing in the clip is over the threshold. `
          + 'Set Background to Edge Mask to see what the tracer sees.';
        return;
      }
      const frames = stream / Math.max(budget, 1);
      const thinned = wanted > stream ? `, thinned from ${number(wanted)} by the scan rate floor` : '';
      const verdict =
        frames > 1.05
          ? `one scan takes ${frames.toFixed(1)} host frames, so the picture crawls`
          : `the whole frame is scanned ${(1 / frames).toFixed(1)}× a frame`;
      line.textContent =
        `Tracing at ${trace}: ${number(contours)} contour${contours === 1 ? '' : 's'}, `
        + `${number(stream)} points in the stream${thinned}, ${number(budget)} scanned this frame. `
        + `At this speed ${verdict}. ${cost}.`;
    }, 250);
  }
}
