#include "Galvo.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace galvo;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Galvo >,                                   // Create method
	"GV01",                                                   // Plugin unique ID of maximum length 4.
	"SW Galvo",                                               // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"Traces the outlines in the clip and scans them the way a laser projector does: two galvanometer mirrors following a point stream at a fixed rate, with a beam that is bright where it lingers.\n\nNothing is drawn. The bright dots on corners, the rounding at speed, the hooks from a mis-set blanking delay and the flicker of a frame too busy for the scanner all fall out of the mirrors and the beam.\n\nBest on logos, titles and line art, where there are real outlines to find. Set Background to Edge Mask to see what the tracer sees.",// Plugin description
	"Galvo FFGL effect"                                       // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[]     = { "Luma", "Alpha", "Chroma", "Luma or Alpha" };
const char* const kSyncNames[]       = { "Free Running", "Restart Each Frame" };
const char* const kColourModeNames[] = { "Clip", "Palette", "White" };
const char* const kBackgroundNames[] = { "Black", "Clip", "Dimmed Clip", "Alpha", "Edge Mask" };

constexpr int kSourceCount     = 4;
constexpr int kSyncCount       = 2;
constexpr int kColourModeCount = 3;
constexpr int kBackgroundCount = 5;

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// A host frame is between these, whatever the clock says. Shorter is a
/// scrub or a stall; longer is the machine having been asleep, and neither
/// should scan a minute of points in one go.
constexpr double kMinFrame = 1.0 / 240.0;
constexpr double kMaxFrame = 1.0 / 24.0;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

double millisSince( const std::chrono::steady_clock::time_point& since )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - since ).count();
}
} // namespace

Galvo::Galvo()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the scanner's clock where it can, so that rendering the
	//same frame twice gives the same picture twice.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a 30 kpps scanner drawing the clip's outlines in the
	// clip's own colours over black: the picture an operator expects when
	// they drop a laser effect on a logo.
	//---------------------------------------------------------------------
	params[ PT_SOURCE ]     = 3.0f;//Luma or Alpha -- right for artwork either way
	params[ PT_DETAIL ]     = 0.20f;
	params[ PT_THRESHOLD ]  = 0.65f;//about 0.25 after mapping
	params[ PT_STABILITY ]  = 0.35f;
	params[ PT_TRACE_SIZE ] = 320.0f;
	params[ PT_MIN_LENGTH ] = 0.15f;//9 trace pixels
	params[ PT_SIMPLIFY ]   = 0.25f;//1.15 trace pixels

	params[ PT_GALVO_SPEED ]    = 30.0f;
	params[ PT_DAMPING ]        = 0.375f;//0.7 after mapping
	params[ PT_DENSITY ]        = 0.54f; //about 100 points per picture height
	params[ PT_CORNER_DWELL ]   = 3.0f;
	params[ PT_BLANKING_DELAY ] = 0.0f;
	params[ PT_SCAN_FLOOR ]     = 0.0f;//off
	params[ PT_FRAME_SYNC ]     = 0.0f;//Free Running

	params[ PT_SPOT ]        = 0.35f;
	params[ PT_BRIGHTNESS ]  = 0.50f;//3.0 after mapping
	params[ PT_PERSISTENCE ] = 0.0f;
	params[ PT_COLOUR_MODE ] = 0.0f;//Clip
	params[ PT_COLOUR_R ]    = 0.10f;
	params[ PT_COLOUR_G ]    = 1.00f;
	params[ PT_COLOUR_B ]    = 0.20f;
	params[ PT_HUE_SPREAD ]  = 0.0f;

	params[ PT_BACKGROUND ] = 0.0f;//Black
	params[ PT_MIX ]        = 1.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every FF_TYPE_STANDARD parameter is a plain 0..1 float even where it
	// stands for pixels or a damping ratio. SetParamInfo clamps a STANDARD
	// default into 0..1 *before* a range can be attached (SDK b1afaf9), so the
	// conversions live in Controls.cpp. FF_TYPE_INTEGER is exempt, which is why
	// Galvo Speed really is declared in kpps and Trace Size in pixels.
	//
	// Option lists are declared in enum order, unsorted: they are short, and
	// each one reads as a progression rather than a list to look a name up in.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto declareInteger = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};

	declareOptions( PT_SOURCE, "Detect On", kSourceNames, kSourceCount );
	SetParamInfof( PT_DETAIL, "Detail", FF_TYPE_STANDARD );
	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_STABILITY, "Stability", FF_TYPE_STANDARD );
	declareInteger( PT_TRACE_SIZE, "Trace Size", 160.0f, 640.0f );
	SetParamInfof( PT_MIN_LENGTH, "Min Length", FF_TYPE_STANDARD );
	SetParamInfof( PT_SIMPLIFY, "Simplify", FF_TYPE_STANDARD );

	//kpps, like the number on the side of a real scanner. 8 is a hobby
	//galvo; 60 is a good one.
	declareInteger( PT_GALVO_SPEED, "Galvo Speed", 8.0f, 60.0f );
	SetParamInfof( PT_DAMPING, "Damping", FF_TYPE_STANDARD );
	SetParamInfof( PT_DENSITY, "Density", FF_TYPE_STANDARD );
	declareInteger( PT_CORNER_DWELL, "Corner Dwell", 0.0f, 8.0f );
	declareInteger( PT_BLANKING_DELAY, "Blanking Delay", -8.0f, 8.0f );
	SetParamInfof( PT_SCAN_FLOOR, "Scan Rate Floor", FF_TYPE_STANDARD );
	declareOptions( PT_FRAME_SYNC, "Frame Sync", kSyncNames, kSyncCount );

	SetParamInfof( PT_SPOT, "Spot", FF_TYPE_STANDARD );
	SetParamInfof( PT_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSISTENCE, "Persistence", FF_TYPE_STANDARD );
	declareOptions( PT_COLOUR_MODE, "Colour Mode", kColourModeNames, kColourModeCount );
	//Consecutive red/green/blue parameters are what a host needs to show a
	//colour swatch instead of three sliders, so the naming follows the SDK's
	//own convention.
	SetParamInfof( PT_COLOUR_R, "Colour", FF_TYPE_RED );
	SetParamInfof( PT_COLOUR_G, "Colour_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_COLOUR_B, "Colour_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_HUE_SPREAD, "Hue Spread", FF_TYPE_STANDARD );

	declareOptions( PT_BACKGROUND, "Background", kBackgroundNames, kBackgroundCount );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses RUNS of consecutive same-group ids, so the enum
	//order is load-bearing: append only.
	for( FFUInt32 i = PT_SOURCE; i <= PT_SIMPLIFY; ++i )
		SetParamGroup( i, "Detect" );
	for( FFUInt32 i = PT_GALVO_SPEED; i <= PT_FRAME_SYNC; ++i )
		SetParamGroup( i, "Scanner" );
	for( FFUInt32 i = PT_SPOT; i <= PT_HUE_SPREAD; ++i )
		SetParamGroup( i, "Beam" );
	for( FFUInt32 i = PT_BACKGROUND; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Galvo effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Galvo::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &edgeShader, kEdgeShader, "edge" },
		{ &stabiliseShader, kStabiliseShader, "stabilise" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect simply
		//does nothing in Resolume, with no message anywhere. These two lines
		//are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Galvo: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !beam.InitGL() )
	{
		FFGLLog::LogToHost( "Galvo: beam renderer failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	historyValid = false;
	scanner.Reset();

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Galvo::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );
	const float aspect      = static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight );

	//The host's viewport, read before anything of ours changes it.
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding (SDK b1afaf9). Every pass's ResizeViewPort() leaks
	//into the pass after it, and the composite -- which draws to the host's own
	//framebuffer -- inherits whatever the last pass left. Here that would be
	//the trace-size stabilise buffer, and the effect would render into a
	//320-pixel corner of the frame.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. Normalise the host's clock to seconds -- see Galvo.h -- and take
	// the frame delta from it, clamped. That delta is the scanner's budget.
	//---------------------------------------------------------------------
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
	double frameSeconds = 1.0 / 60.0;
	if( lastHostTime >= 0.0 )
		frameSeconds = std::clamp( now - lastHostTime, kMinFrame, kMaxFrame );
	lastHostTime = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// FFGLFBO::Initialise sizes its colour texture under a Scoped binding, and
	// every ffglex Scoped* binding *clears* to 0 on scope exit rather than
	// restoring what was there. Allocating mid-chain would unbind the input
	// texture, correctly on every frame except the one that allocates.
	//---------------------------------------------------------------------
	const int traceWidth  = std::clamp( static_cast< int >( std::lround( params[ PT_TRACE_SIZE ] ) ), 16, 2048 );
	const int traceHeight = std::max( 8, static_cast< int >( std::lround( traceWidth / aspect ) ) );

	const bool sizeChanged = !stableBuffer[ 0 ].IsValid()
	                         || static_cast< int >( stableBuffer[ 0 ].GetWidth() ) != traceWidth
	                         || static_cast< int >( stableBuffer[ 0 ].GetHeight() ) != traceHeight;

	const bool allocated =
		copyBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
		&& edgeBuffer.Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& stableBuffer[ 0 ].Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& stableBuffer[ 1 ].Ensure( traceWidth, traceHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		&& beam.Ensure( pictureWidth, pictureHeight );

	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}
	if( sizeChanged )
		historyValid = false;

	//---------------------------------------------------------------------
	// 1. The picture, into a texture of ours, with a mip chain on it.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel", 0.5f / static_cast< float >( pictureWidth ), 0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
	}
	copyBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. Edge, at the trace resolution.
	//---------------------------------------------------------------------
	{
		//The mip level that matches the trace resolution, plus Detail. The
		//taps and the level move together: detecting at level 2 with taps one
		//level-0 texel apart samples the same texel three times.
		const float baseLod = std::log2( std::max( 1.0f, static_cast< float >( pictureWidth ) / static_cast< float >( traceWidth ) ) );
		const float detail  = DetailFromParam( params[ PT_DETAIL ] );
		const float spread  = std::exp2( detail );

		ScopedFBOBinding fbo( edgeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		edgeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( edgeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		edgeShader.Set( "CopyTexture", 0 );
		edgeShader.Set( "Step", spread / static_cast< float >( traceWidth ), spread / static_cast< float >( traceHeight ) );
		edgeShader.Set( "Lod", baseLod + detail );
		edgeShader.Set( "SourceMode", params[ PT_SOURCE ] );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. Stabilise, ping-ponged against the previous frame's result.
	//---------------------------------------------------------------------
	const int history = stableCurrent;
	const int target  = 1 - stableCurrent;
	{
		ScopedFBOBinding fbo( stableBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		stableBuffer[ target ].ResizeViewPort();
		ScopedShaderBinding shader( stabiliseShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding edgeTexture( edgeBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding historyTexture( stableBuffer[ history ].TextureID() );

		stabiliseShader.Set( "EdgeTexture", 0 );
		stabiliseShader.Set( "HistoryTexture", 1 );
		stabiliseShader.Set( "Attack", AttackFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Release", ReleaseFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Threshold", ThresholdFromParam( params[ PT_THRESHOLD ] ) );
		stabiliseShader.Set( "Softness", kThresholdSoftness );
		stabiliseShader.Set( "Reset", historyValid ? 0.0f : 1.0f );
		quad.Draw();
	}
	stableCurrent = target;
	historyValid  = true;

	//---------------------------------------------------------------------
	// 4. Read the mask back and trace it. This is the one synchronous
	//    readback in the fleet's effect plugins, and it is a pipeline stall:
	//    glReadPixels waits for every command above it to finish. It is kept
	//    small -- one byte per trace texel, 57 KB at the default size -- and
	//    it is timed, so `gvtest --bench` reports what it costs rather than
	//    what it is hoped to cost.
	//---------------------------------------------------------------------
	const auto cpuStart = std::chrono::steady_clock::now();
	{
		mask.resize( static_cast< std::size_t >( traceWidth ) * static_cast< std::size_t >( traceHeight ) );
		ScopedFBOBinding fbo( stableBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, traceWidth, traceHeight, GL_RED, GL_UNSIGNED_BYTE, mask.data() );
	}
	readbackMillis = millisSince( cpuStart );

	{
		TraceParams traceParams;
		traceParams.threshold = 128;
		traceParams.simplify  = SimplifyFromParam( params[ PT_SIMPLIFY ] );
		traceParams.minLength = MinLengthFromParam( params[ PT_MIN_LENGTH ] );
		tracer.Trace( mask.data(), traceWidth, traceHeight, traceParams, contours );

		//The tour starts from where the mirrors are now, in trace pixels.
		const Vec2 mirror = scanner.MirrorPosition();
		OrderContours( contours, Vec2{ mirror.x / aspect * static_cast< float >( traceWidth ) - 0.5f,
		                               mirror.y * static_cast< float >( traceHeight ) - 0.5f } );

		StreamParams streamParams;
		streamParams.density         = DensityFromParam( params[ PT_DENSITY ] );
		streamParams.cornerDwell     = static_cast< int >( std::lround( params[ PT_CORNER_DWELL ] ) );
		streamParams.blankSettle     = kBlankSettle;
		streamParams.colourMode      = static_cast< ColourMode >( static_cast< int >( std::lround( params[ PT_COLOUR_MODE ] ) ) );
		streamParams.colour[ 0 ]     = params[ PT_COLOUR_R ];
		streamParams.colour[ 1 ]     = params[ PT_COLOUR_G ];
		streamParams.colour[ 2 ]     = params[ PT_COLOUR_B ];
		streamParams.hueSpread       = HueSpreadFromParam( params[ PT_HUE_SPREAD ] );
		streamParams.pointsPerSecond = 1000.0f * std::lround( params[ PT_GALVO_SPEED ] );
		streamParams.scanRateFloorHz = ScanRateFloorFromParam( params[ PT_SCAN_FLOOR ] );

		lastStreamWanted = BuildStream( contours, traceWidth, traceHeight, aspect, streamParams, stream );
		lastStreamSize   = stream.size();
		scanner.SetStream( stream );
	}

	//---------------------------------------------------------------------
	// 5. Scan this host frame's worth of points.
	//---------------------------------------------------------------------
	{
		const double pps = 1000.0 * static_cast< double >( std::lround( params[ PT_GALVO_SPEED ] ) );
		scanner.SetPointRate( pps );
		scanner.SetResponse( OmegaFromPointRate( pps ), DampingFromParam( params[ PT_DAMPING ] ) );
		scanner.SetBlankingDelay( static_cast< int >( std::lround( params[ PT_BLANKING_DELAY ] ) ) );
		scanner.SetRestartEachFrame( std::lround( params[ PT_FRAME_SYNC ] ) == 1 );

		samples.clear();
		scanner.Advance( frameSeconds, samples );
	}
	cpuMillis = millisSince( cpuStart );

	//---------------------------------------------------------------------
	// 6. Light along the path.
	//---------------------------------------------------------------------
	{
		BeamRenderer::Params beamParams;
		beamParams.beamPower  = BeamPowerFromParam( params[ PT_BRIGHTNESS ] );
		beamParams.spotSigma  = SpotSigmaFromParam( params[ PT_SPOT ] );
		const float tau       = PersistenceTauFromParam( params[ PT_PERSISTENCE ] );
		beamParams.decay      = tau > 0.0f ? std::exp( -static_cast< float >( frameSeconds ) / tau ) : 0.0f;
		beamParams.colourMode = static_cast< int >( std::lround( params[ PT_COLOUR_MODE ] ) );
		beamParams.saturate   = kLaserSaturation;

		if( !beam.Deposit( samples.data(), static_cast< int >( samples.size() ), beamParams, copyBuffer.TextureID(), aspect ) )
			return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 7. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding beamTexture( beam.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding stableTexture( stableBuffer[ target ].TextureID() );

		compositeShader.Set( "CopyTexture", 0 );
		compositeShader.Set( "BeamTexture", 1 );
		compositeShader.Set( "StableTexture", 2 );
		compositeShader.Set( "Background", params[ PT_BACKGROUND ] );
		compositeShader.Set( "Dim", kDimLevel );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Galvo::DeInitGL()
{
	copyShader.FreeGLResources();
	edgeShader.FreeGLResources();
	stabiliseShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	edgeBuffer.Destroy();
	stableBuffer[ 0 ].Destroy();
	stableBuffer[ 1 ].Destroy();
	beam.DeInitGL();

	historyValid = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Galvo::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;

	//Changing what an edge *is* invalidates the history, because the numbers
	//being blended are no longer measuring the same thing.
	if( index == PT_DETAIL || index == PT_SOURCE )
		historyValid = false;

	return FF_SUCCESS;
}

float Galvo::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

//---------------------------------------------------------------------------
char* Galvo::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Galvo::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Galvo::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Galvo::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
