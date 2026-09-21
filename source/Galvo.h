#pragma once

#include "PassBuffer.h"
#include "PointStream.h"
#include "Scanner.h"
#include "StoatworksAboutParams.h"
#include "Tracer.h"
#include "render/Beam.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
    Galvo -- an ILDA laser projector, as an FFGL effect for Resolume.

    **The one idea.** A laser show is a path scanned by two galvanometer
    mirrors with a finite point budget. The picture is where the beam went,
    at the brightness its dwell time gave it, and nothing else is drawn. The
    dwell dots on every corner, the rounded corners at speed, the hooks and
    tails from a mis-set blanking delay, the flicker of a frame too busy for
    the scanner -- none of it is drawn; it falls out of a second-order mirror
    following a point stream at a fixed rate, and a renderer that deposits
    equal energy per unit of time.

    **The pipeline**, and where each part lives:

      detect     GPU   Shaders.cpp   copy, edge at trace size, stabilise
      read back  ->    Galvo.cpp     one glReadPixels of the small mask
      trace      CPU   Tracer.cpp    thin, walk, simplify
      order      CPU   PointStream   nearest-neighbour tour
      points     CPU   PointStream   density, corner dwell, blanking
      scan       CPU   Scanner.cpp   the galvo model, the point budget
      render     GPU   render/Beam   decay, then energy along the path
      composite  GPU   Shaders.cpp   over black, the clip, or nothing

    Everything between the readback and the renderer has no GL in it, which
    is what lets gvtest check the tracer, the step response and the point
    budget with no context and lets CI run those checks on a runner with no
    GPU.

    See AGENTS.md for the traps.
*/
class Galvo : public CFFGLPlugin
{
public:
	Galvo();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale );

	//--- test hooks. The harness reads these; the plugin never does. -------
	const std::vector< galvo::Contour >& LastContours() const
	{
		return contours;
	}
	galvo::Scanner& GetScanner()
	{
		return scanner;
	}
	galvo::BeamRenderer& GetBeam()
	{
		return beam;
	}
	/// Milliseconds the CPU half of the last frame took: the readback, the
	/// trace, the ordering and the stream. The readback is a pipeline stall
	/// and is the part worth watching.
	double LastCpuMillis() const
	{
		return cpuMillis;
	}
	double LastReadbackMillis() const
	{
		return readbackMillis;
	}
	std::size_t LastStreamSize() const
	{
		return lastStreamSize;
	}
	std::size_t LastStreamWanted() const
	{
		return lastStreamWanted;
	}

	/// The order the host shows them in: find the outline, drive the scanner,
	/// shape the beam, and put it back over the picture.
	enum ParamID : FFUInt32
	{
		//Detect
		PT_SOURCE,
		PT_DETAIL,
		PT_THRESHOLD,
		PT_STABILITY,
		PT_TRACE_SIZE,
		PT_MIN_LENGTH,
		PT_SIMPLIFY,

		//Scanner
		PT_GALVO_SPEED,
		PT_DAMPING,
		PT_DENSITY,
		PT_CORNER_DWELL,
		PT_BLANKING_DELAY,
		PT_SCAN_FLOOR,
		PT_FRAME_SYNC,

		//Beam
		PT_SPOT,
		PT_BRIGHTNESS,
		PT_PERSISTENCE,
		PT_COLOUR_MODE,
		PT_COLOUR_R,
		PT_COLOUR_G,
		PT_COLOUR_B,
		PT_HUE_SPREAD,

		//Output
		PT_BACKGROUND,
		PT_MIX,

		//About. FFGL has no window, so the name, the version, the maker and
		//the links are parameters the host draws with everything else. Last
		//in the enum so no saved composition's ids shift if it grows.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader edgeShader;
	ffglex::FFGLShader stabiliseShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	galvo::PassBuffer copyBuffer;        ///< the picture, ours, mipmapped
	galvo::PassBuffer edgeBuffer;        ///< raw gradient magnitude, trace size
	galvo::PassBuffer stableBuffer[ 2 ]; ///< ping-pong: mask + stabilised edge, trace size
	int stableCurrent = 0;

	/// Set when the history buffers hold nothing worth blending against: the
	/// first frame, and any frame after a resize or a change of what an edge
	/// is.
	bool historyValid = false;

	galvo::BeamRenderer beam;

	//--- the CPU half ------------------------------------------------------
	std::vector< unsigned char > mask;
	galvo::Tracer tracer;
	std::vector< galvo::Contour > contours;
	std::vector< galvo::Point > stream;
	galvo::Scanner scanner;
	std::vector< galvo::Sample > samples;

	double cpuMillis            = 0.0;
	double readbackMillis       = 0.0;
	std::size_t lastStreamSize   = 0;
	std::size_t lastStreamWanted = 0;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS, the offline harness sends seconds.
	// Decided by comparing the host's deltas with the wall clock over a few
	// frames, as tinsel does, and until decided the wall clock is used --
	// wrong in origin, right in rate.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
