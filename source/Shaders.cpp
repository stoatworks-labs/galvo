#include "Shaders.h"

namespace galvo
{

const char* const kVertexShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 2: edge, at the trace resolution.
//---------------------------------------------------------------------------
const char* const kEdgeShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 3: stabilise.
//---------------------------------------------------------------------------
const char* const kStabiliseShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 4: decay. The accumulation buffer is a camera's exposure: it keeps a
// fraction of itself each frame and the trace pass adds this frame's light on
// top. Not a phosphor -- there is no cascade and no colour shift, only a
// shutter that is open for longer than one frame.
//---------------------------------------------------------------------------
const char* const kDecayShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 5: the trace. One instanced quad per scanner interval.
//
// Shared constants first: `Extent` is the half-width of the quad around each
// segment in units of the spot's sigma. 4.5 sigma keeps 99.9993% of a
// Gaussian, and the fragment stage subtracts the profile's value at exactly
// that distance so what is thrown away is thrown away smoothly. Two stages
// have to agree about the number, which is why it is one string.
//---------------------------------------------------------------------------
const char* const kBeamConstants = R"(
const float Extent     = 4.5;
const float Sqrt2Pi    = 2.50662827463100050;
const float InvSqrt2Pi = 0.39894228040143268;
)";

// The two attributes per sample are one buffer bound twice, the second offset
// by one Sample, so instance i sees sample i and sample i+1 with nothing
// duplicated. That is also why the draw asks for n-1 instances: n would read
// one Sample past the end of the buffer.
//
// `sampleA`/`sampleB`, not `sample`: `sample` is a GLSL reserved word.
const char* const kTraceVertexBody = R"(
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
	//there. `on` is the interval's blanking state; `dt` is the interval's
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
)";

// The closed form, evaluated per fragment:
//
//     f(u,v) = (E/L) * G_sigma(v) * [ Phi((u + L/2)/sigma) - Phi((u - L/2)/sigma) ]
//
// with u measured from the segment's centre. It integrates to E over the
// plane for any L and any sigma, which is the energy conservation gvtest
// --energy checks, and it tends to a finite Gaussian spot as L goes to zero,
// which is why a mirror that stops needs no special case.
const char* const kTraceFragmentBody = R"(
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
		float halfLen = 0.5 * segLength;//`half` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	float deposit = segEnergy * across * along;
	fragColor = vec4( segColour * deposit, deposit );
}
)";

//---------------------------------------------------------------------------
// Pass 6: composite.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Assembly. Mirrored by the ASSEMBLED table in tools/verify.sh.
//---------------------------------------------------------------------------
std::string TraceVertexSource()
{
	return std::string( "#version 410 core\n" ) + kBeamConstants + kTraceVertexBody;
}

std::string TraceFragmentSource()
{
	return std::string( "#version 410 core\n" ) + kBeamConstants + kTraceFragmentBody;
}

} // namespace galvo
