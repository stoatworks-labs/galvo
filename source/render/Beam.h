#pragma once

#include "PassBuffer.h"
#include "Scanner.h"

#include <FFGLSDK.h>

#include <vector>

namespace galvo
{
/**
    Where the beam went, as light.

    A conventional line renderer draws a polyline and multiplies by something
    like 1/speed so slow parts look brighter, then clamps the reciprocal where
    the beam stops. This one never computes a reciprocal. Each scanner
    interval deposits a fixed quantum, BeamPower * dt * on, spread over the
    distance the mirrors actually covered in it, by the exact convolution of a
    uniform segment with a Gaussian spot. Brightness proportional to dwell
    falls out as the definition of equal energy per unit time; a mirror that
    stops deposits a finite Gaussian dot with no special case; and the total
    light in a frame is BeamPower times the beam-on time whatever the galvo
    did with the path, which is what `gvtest --energy` measures.

    Two passes into a ping-ponged RGBA16F buffer at picture size:

      1. decay   history * Decay, blending off. The exposure.
      2. trace   one instanced quad per interval, additive, on top.

    Sixteen-bit float rather than thirty-two, and the choice was measured
    rather than assumed. Additive blending at 16F swamps a deposit that falls
    below half an ULP of what a texel already holds, and the texels holding
    the most are the dwell dots -- which are this plugin's signature. So it is
    a fair question. The numbers, on an M4 Max:

      format   light vs the analytic   spread across 12 galvos   4K ms/frame
      RGBA16F  -0.324%                 0.026%                    1.35
      RGBA32F  -0.016%                 0.005%                    1.86

    Both pass the 0.5% conservation claim with margin; 32F buys back a third
    of a percent of brightness -- a third of one part in 255 -- for half a
    millisecond a frame at 4K and 133 MB more VRAM for the ping-pair. So 16F
    ships. Reverse it here if a future look accumulates for much longer than
    the exposure does, which is the case that would change the answer.

    Brightness arithmetic, for the reader tuning BeamPower: a line at density
    d points per unit length, scanned at P points per second, carries
    BeamPower / P per point and so BeamPower * d / P per unit length; its
    peak, convolved with a spot of sigma, is BeamPower * d / (P * sigma *
    sqrt(2 pi)). At the defaults -- 3.0, 100, 30000, 0.0037 -- that is 1.08.
*/
class BeamRenderer
{
public:
	struct Params
	{
		float beamPower  = 3.0f;
		float spotSigma  = 0.0037f;///< picture units; 1 = the picture height
		float decay      = 0.0f;   ///< fraction of last frame kept; 0 clears
		int colourMode   = 0;      ///< 0 clip, 1 palette, 2 white
		float saturate   = 0.85f;
		bool clearHistory = false;
	};

	bool InitGL();
	void DeInitGL();

	/// Allocate the accumulation buffers at this size. Call before anything
	/// binds a texture: allocating unbinds the active unit (SDK trap).
	bool Ensure( int width, int height );

	/// Decay, then deposit `n` samples' worth of intervals (n - 1 of them).
	/// `copyTexture` is the picture, read for Colour Mode = Clip; `aspect` is
	/// its width over height, which is where x = aspect lands.
	bool Deposit( const Sample* samples, int n, const Params& params, GLuint copyTexture, float aspect );

	/// The accumulation after the last Deposit.
	GLuint TextureID() const
	{
		return accumulation[ current ].TextureID();
	}

	/// Read the accumulation back as RGBA floats, bottom row first. For the
	/// harness: 16-bit float texels come back through the driver's conversion,
	/// so what is measured is what was stored.
	bool Read( std::vector< float >& rgba ) const;

	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}

private:
	ffglex::FFGLShader decayShader;
	ffglex::FFGLShader traceShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer accumulation[ 2 ];
	int current = 0;
	int width   = 0;
	int height  = 0;

	GLuint traceVAO = 0;
	GLuint traceVBO = 0;
};

} // namespace galvo
