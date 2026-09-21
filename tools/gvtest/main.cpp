/**
    gvtest -- render Galvo offline, and check what its beam is doing.

    Where the mirrors went and how long they lingered are facts, not matters
    of taste. Everything between the readback and the renderer is plain C++
    with no GL in it, so half of these checks need no context at all; the rest
    drive the real renderer, or the real plugin class, in a headless CGL
    context on a synthetic 60 fps clock.

        gvtest --out /tmp/frame.png     a picture, on the test card
        gvtest --list                   every parameter, its type and range
        gvtest --trace                  the tracer: a square is one closed
                                        contour, a C is one open one -- on a
                                        synthetic mask AND through the plugin's
                                        own detect passes
        gvtest --step                   the galvo overshoots by the textbook
                                        amount for its damping, on both axes
        gvtest --budget                 a stream of N points at P pps takes
                                        ceil( N / (P/60) ) host frames
        gvtest --energy                 the light in a frame does not depend on
                                        the galvo
        gvtest --dwell                  a corner is brighter than the line by
                                        the ratio its dwell points predict
        gvtest --bench                  ms/frame at 720p through 4K, CPU and
                                        GPU separately
*/

#include "Controls.h"
#include "Galvo.h"
#include "PointStream.h"
#include "Scanner.h"
#include "Tracer.h"
#include "render/Beam.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace galvo;

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card. Not meant to look nice: each shape exercises one thing.
//
//   - a red filled square:      four straight edges and four real corners,
//                               which is what dwell dots and rounding show on
//   - a cyan hollow ring:       two concentric closed contours, close together
//   - a yellow bar:             a long thin closed contour
//   - a transparent hole:       an edge only Alpha and Luma or Alpha can see
//   - two equal-luma fields:    invisible to Luma, found by Chroma
//   - a scatter of specks:      what Min Length exists to drop
//   - a soft gradient:          no edge anywhere; anything found here is noise
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;
			float r = 0.06f + 0.10f * u, g = r, b = r, a = 1.0f;

			//Square, left.
			if( u > 0.08f && u < 0.08f + 0.36f / aspect && v > 0.30f && v < 0.66f )
			{
				r = 0.95f; g = 0.15f; b = 0.10f;
			}

			//Ring, middle.
			const float dx = ( u - 0.50f ) * aspect, dy = v - 0.50f;
			const float d  = std::sqrt( dx * dx + dy * dy );
			if( d < 0.17f && d > 0.11f )
			{
				r = 0.10f; g = 0.85f; b = 0.95f;
			}

			//Bar, right.
			if( u > 0.74f && u < 0.86f && v > 0.14f && v < 0.86f )
			{
				r = 1.0f; g = 0.9f; b = 0.1f;
			}

			//Transparent hole, top left.
			const float hx = ( u - 0.14f ) * aspect, hy = v - 0.14f;
			if( std::sqrt( hx * hx + hy * hy ) < 0.07f )
				a = 0.0f;

			//Equal-luma fields, bottom middle: 0.2126 R against 0.7152 G,
			//arranged to the same luma the shader computes.
			if( v < 0.10f && u > 0.36f && u < 0.64f )
			{
				const bool right = u > 0.50f;
				r = right ? 0.10f : 0.9333f;
				g = right ? 0.2775f : 0.0f;
				b = 0.0f;
			}

			//Short dashes, top right, of three different lengths. These exist
			//for Min Length: a control that drops contours shorter than N has
			//nothing to say about a card whose every contour is longer than N,
			//and the sweep correctly reported it dead until these were here.
			//As fractions of the width, so the traced lengths are the same
			//whatever the card is rendered at: 0.04, 0.07 and 0.11 of the
			//width come out at roughly 13, 22 and 35 trace pixels at the
			//default Trace Size, which straddles the control's range.
			{
				const float lengths[ 3 ] = { 0.04f, 0.07f, 0.11f };
				for( int k = 0; k < 3; ++k )
				{
					const float top = 0.94f - 0.05f * static_cast< float >( k );
					if( u > 0.60f && u < 0.60f + lengths[ k ] && v < top && v > top - 0.012f )
						r = g = b = 1.0f;
				}
			}

			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = static_cast< unsigned char >( std::min( 255.0f, r * a * 255.0f ) );
			p[ 1 ] = static_cast< unsigned char >( std::min( 255.0f, g * a * 255.0f ) );
			p[ 2 ] = static_cast< unsigned char >( std::min( 255.0f, b * a * 255.0f ) );
			p[ 3 ] = static_cast< unsigned char >( a * 255.0f );
		}
	}
	return image;
}

/// A card with nothing on it but one filled square of `side` pixels,
/// centred. For the end-to-end trace check.
std::vector< unsigned char > buildSquareCard( int width, int height, int side )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	const int x0 = ( width - side ) / 2, y0 = ( height - side ) / 2;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const bool in    = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side;
			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = in ? 240 : 20;
			p[ 3 ] = 255;
		}
	return image;
}

/// The same PCG hash the fleet uses, for reproducible per-frame noise.
uint32_t hashInt( uint32_t x )
{
	x          = x * 747796405u + 2891336453u;
	uint32_t w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

/// The card with per-frame noise on it, which is the only way to test
/// Stability: on a still picture it provably does nothing.
std::vector< unsigned char > addNoise( const std::vector< unsigned char >& card, int frame, float amount )
{
	std::vector< unsigned char > noisy = card;
	if( amount <= 0.0f )
		return noisy;
	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		const uint32_t seed = hashInt( static_cast< uint32_t >( i / 4 ) * 2654435761u ^ static_cast< uint32_t >( frame ) );
		const float jitter  = ( static_cast< float >( seed ) / 4294967296.0f - 0.5f ) * scale;
		for( int c = 0; c < 3; ++c )
		{
			const float value = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]    = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

/// The plugin's input and output, in one place.
struct Rig
{
	int width = 0, height = 0;
	GLuint source = 0, output = 0, fbo = 0;
	FFGLTextureStruct inputStruct   = {};
	FFGLTextureStruct* inputs[ 1 ]  = { nullptr };
	ProcessOpenGLStruct process     = {};

	Rig( int w, int h, const std::vector< unsigned char >& card ) : width( w ), height( h )
	{
		source = makeTexture( w, h, card.data() );
		output = makeTexture( w, h, nullptr );
		fbo    = makeFramebuffer( output );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle = source;
		inputs[ 0 ]        = &inputStruct;
		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = fbo;
	}
	~Rig()
	{
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &output );
		glDeleteTextures( 1, &source );
	}
	void upload( const std::vector< unsigned char >& pixels )
	{
		glBindTexture( GL_TEXTURE_2D, source );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	FFResult render( Galvo& plugin )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process );
	}
};

/// A synthetic clock, and it has to be synthetic: left to the wall clock the
/// harness renders a hundred frames in a few milliseconds, no time passes,
/// the scanner's budget is the minimum frame, and nothing is reproducible.
void driveClock( Galvo& plugin, int frame, double fps )
{
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( static_cast< double >( frame ) / fps );
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
const char* typeName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BOOLEAN: return "boolean";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_RED:
	case FF_TYPE_GREEN:
	case FF_TYPE_BLUE: return "colour";
	default: return "other";
	}
}

bool applySetting( Galvo& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( unsigned int i = 0; i < Galvo::PT_COUNT; ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared == nullptr || name != declared )
			continue;
		plugin.SetFloatParameter( i, std::strtof( value.c_str(), nullptr ) );
		return true;
	}
	error = "no parameter called '" + name + "'";
	return false;
}

int listParameters( Galvo& plugin )
{
	std::printf( "%-3s %-20s %-9s %10s %9s %9s\n", "id", "name", "type", "default", "min", "max" );
	for( unsigned int i = 0; i < Galvo::PT_COUNT; ++i )
	{
		const char* name        = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );
		RangeStruct range       = plugin.GetParamRange( i );
		if( type == FF_TYPE_OPTION )
			range = { 0.0f, static_cast< float >( plugin.GetNumParamElements( i ) ) - 1.0f };
		std::printf( "%-3u %-20s %-9s %10.4f %9.4f %9.4f\n", i, name ? name : "?", typeName( type ),
		             plugin.GetFloatParameter( i ), range.min, range.max );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --trace
//---------------------------------------------------------------------------
/// Draw a one-pixel line into a mask with Bresenham.
void drawLine( std::vector< unsigned char >& mask, int w, int x0, int y0, int x1, int y1 )
{
	const int dx = std::abs( x1 - x0 ), sx = x0 < x1 ? 1 : -1;
	const int dy = -std::abs( y1 - y0 ), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	while( true )
	{
		mask[ static_cast< size_t >( y0 ) * w + x0 ] = 255;
		if( x0 == x1 && y0 == y1 )
			break;
		const int e2 = 2 * err;
		if( e2 >= dy ) { err += dy; x0 += sx; }
		if( e2 <= dx ) { err += dx; y0 += sy; }
	}
}

int runTraceCheck( bool haveContext )
{
	int failures = 0;
	constexpr int W = 320, H = 180;
	TraceParams tp;
	tp.threshold = 128;
	tp.simplify  = 1.0f;
	tp.minLength = 8.0f;
	Tracer tracer;
	std::vector< Contour > contours;

	//1. A one-pixel square outline of side S: one closed contour, perimeter 4S.
	{
		constexpr int S = 80, x0 = 100, y0 = 40;
		std::vector< unsigned char > mask( W * H, 0 );
		drawLine( mask, W, x0, y0, x0 + S, y0 );
		drawLine( mask, W, x0 + S, y0, x0 + S, y0 + S );
		drawLine( mask, W, x0 + S, y0 + S, x0, y0 + S );
		drawLine( mask, W, x0, y0 + S, x0, y0 );
		tracer.Trace( mask.data(), W, H, tp, contours );
		const float perimeter = contours.size() == 1 ? PolylineLength( contours[ 0 ].points, contours[ 0 ].closed ) : 0.0f;
		const bool ok = contours.size() == 1 && contours[ 0 ].closed
		                && std::fabs( perimeter - 4.0f * S ) <= 0.02f * 4.0f * S
		                && contours[ 0 ].points.size() <= 8;
		std::printf( "  square outline (side %d): %zu contour(s), %s, perimeter %.1f (expected %d), %zu vertices  %s\n",
		             S, contours.size(), contours.empty() ? "-" : ( contours[ 0 ].closed ? "closed" : "open" ),
		             perimeter, 4 * S, contours.empty() ? size_t( 0 ) : contours[ 0 ].points.size(), ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	//2. The same square as a three-pixel band: thinning must reduce it to one
	//   closed contour, not three.
	{
		constexpr int S = 80, x0 = 100, y0 = 40;
		std::vector< unsigned char > mask( W * H, 0 );
		for( int k = -1; k <= 1; ++k )
		{
			drawLine( mask, W, x0 + k, y0 + k, x0 + S - k, y0 + k );
			drawLine( mask, W, x0 + S - k, y0 + k, x0 + S - k, y0 + S - k );
			drawLine( mask, W, x0 + S - k, y0 + S - k, x0 + k, y0 + S - k );
			drawLine( mask, W, x0 + k, y0 + S - k, x0 + k, y0 + k );
		}
		tracer.Trace( mask.data(), W, H, tp, contours );
		const float perimeter = contours.size() == 1 ? PolylineLength( contours[ 0 ].points, contours[ 0 ].closed ) : 0.0f;
		const bool ok = contours.size() == 1 && contours[ 0 ].closed
		                && std::fabs( perimeter - 4.0f * S ) <= 0.04f * 4.0f * S;
		std::printf( "  square band (3 px thick): %zu contour(s), %s, perimeter %.1f (expected %d)  %s\n",
		             contours.size(), contours.empty() ? "-" : ( contours[ 0 ].closed ? "closed" : "open" ),
		             perimeter, 4 * S, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	//3. A letter C: a 270-degree arc, one pixel wide. One OPEN contour whose
	//   length is three quarters of the circumference.
	{
		constexpr int R = 50, cx = 160, cy = 90;
		std::vector< unsigned char > mask( W * H, 0 );
		int px = -1, py = -1;
		for( int i = 0; i <= 540; ++i )
		{
			const double angle = ( 45.0 + 270.0 * i / 540.0 ) * kPi / 180.0;
			const int x = cx + static_cast< int >( std::lround( R * std::cos( angle ) ) );
			const int y = cy + static_cast< int >( std::lround( R * std::sin( angle ) ) );
			if( px >= 0 )
				drawLine( mask, W, px, py, x, y );
			px = x;
			py = y;
		}
		tracer.Trace( mask.data(), W, H, tp, contours );
		const float length   = contours.size() == 1 ? PolylineLength( contours[ 0 ].points, contours[ 0 ].closed ) : 0.0f;
		const float expected = static_cast< float >( 1.5 * kPi * R );
		const bool ok = contours.size() == 1 && !contours[ 0 ].closed
		                && std::fabs( length - expected ) <= 0.05f * expected;
		std::printf( "  letter C (radius %d, 270 deg): %zu contour(s), %s, length %.1f (expected %.1f)  %s\n",
		             R, contours.size(), contours.empty() ? "-" : ( contours[ 0 ].closed ? "closed" : "open" ),
		             length, expected, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	//4. Through the plugin's own detect passes: a filled square in a 1280x720
	//   picture, traced at 320 wide, comes out as one closed contour of the
	//   right perimeter in trace pixels.
	if( haveContext )
	{
		constexpr int width = 1280, height = 720, side = 240;
		Galvo plugin;
		FFGLViewportStruct viewport = {};
		viewport.width  = width;
		viewport.height = height;
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::printf( "  through the plugin: InitGL FAILED\n" );
			return failures + 1;
		}
		Rig rig( width, height, buildSquareCard( width, height, side ) );
		for( int frame = 0; frame < 3; ++frame )
		{
			driveClock( plugin, frame, 60.0 );
			rig.render( plugin );
		}
		const std::vector< Contour >& found = plugin.LastContours();
		const float perimeter = found.size() == 1 ? PolylineLength( found[ 0 ].points, found[ 0 ].closed ) : 0.0f;
		const float expected  = 4.0f * side / 4.0f;//320 wide over 1280 is a quarter
		const bool ok = found.size() == 1 && found[ 0 ].closed
		                && std::fabs( perimeter - expected ) <= 0.08f * expected;
		std::printf( "  through the plugin (filled %d px square, traced at 320): %zu contour(s), %s, perimeter %.1f (expected %.0f +-8%%)  %s\n",
		             side, found.size(), found.empty() ? "-" : ( found[ 0 ].closed ? "closed" : "open" ),
		             perimeter, expected, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
		plugin.DeInitGL();
	}
	else
		std::printf( "  through the plugin: skipped, no GL context\n" );

	std::printf( failures == 0 ? "trace: ok\n" : "trace: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --step
//---------------------------------------------------------------------------
// A step on both mirrors at once, of different sizes, so the axes are checked
// to be independent as well as right.
//
// Two predictions, and the second is the interesting one.
//
// **Overshoot** is the textbook exp( -pi zeta / sqrt( 1 - zeta^2 ) ) for
// zeta < 1, and zero above.
//
// **The residual after eight point periods** is compared against the EXACT
// step response,
//
//     1 - x(t) = exp( -zeta w t ) / sqrt( 1 - zeta^2 ) * sin( wd t + acos zeta )
//
// rather than against the 2% settling figure the kpps mapping in Controls.cpp
// is derived from. Those are not the same number and the difference is worth
// knowing: the familiar ts = 4 / (zeta w) is the decay ENVELOPE reaching 2%,
// and the response itself is that envelope times 1 / sqrt( 1 - zeta^2 ) times a
// sine. At zeta = 0.7 that factor is 1.4, so the true residual at the quoted
// settling time is 2.53% and not 2% -- which is the mapping being an
// engineering rule of thumb, exactly as documented, and not the integrator
// being wrong. Checking the closed form is the stronger claim of the two: it
// says the RK4 integration really is the second-order system it says it is, to
// a fraction of a percent, at a point in time where the sine matters.
int runStepCheck()
{
	int failures = 0;
	constexpr double pps = 30000.0;
	const double zetas[] = { 0.4, 0.5, 0.7, 1.0, 1.2 };

	for( double zeta : zetas )
	{
		Scanner scanner;
		scanner.Reset();
		scanner.SetPointRate( pps );
		scanner.SetResponse( OmegaFromPointRate( pps ), zeta );

		std::vector< Point > stream;
		constexpr int settle = 400, after = 400;
		for( int i = 0; i < settle; ++i )
			stream.push_back( Point{ 0.2f, 0.3f, 1, 1, 1, true } );
		for( int i = 0; i < after; ++i )
			stream.push_back( Point{ 0.7f, 0.6f, 1, 1, 1, true } );
		scanner.SetStream( stream );

		std::vector< Sample > samples;
		scanner.Advance( ( settle + after + 0.5 ) / pps, samples );

		const size_t stepAt = static_cast< size_t >( settle ) * Scanner::kSubsteps;
		double peakX = -1e9, peakY = -1e9;
		for( size_t i = stepAt; i < samples.size(); ++i )
		{
			peakX = std::max( peakX, static_cast< double >( samples[ i ].x ) );
			peakY = std::max( peakY, static_cast< double >( samples[ i ].y ) );
		}
		const double overshootX = ( peakX - 0.7 ) / 0.5;
		const double overshootY = ( peakY - 0.6 ) / 0.3;
		const double predicted  = zeta < 1.0 ? std::exp( -kPi * zeta / std::sqrt( 1.0 - zeta * zeta ) ) : 0.0;

		bool ok;
		if( zeta < 1.0 )
			ok = std::fabs( overshootX - predicted ) <= 0.05 * predicted
			     && std::fabs( overshootY - predicted ) <= 0.05 * predicted;
		else
			ok = overshootX < 0.005 && overshootY < 0.005;

		//After exactly eight point periods, how far from the target, as a
		//fraction of the step -- against the closed form above. The sample at
		//stepAt + 8 * kSubsteps - 1 is the last substep of the eighth point, so
		//its time since the step is exactly 8 / pps.
		const size_t eight     = stepAt + 8 * Scanner::kSubsteps - 1;
		const double residualX = eight < samples.size() ? std::fabs( samples[ eight ].x - 0.7 ) / 0.5 : 1.0;
		const double residualY = eight < samples.size() ? std::fabs( samples[ eight ].y - 0.6 ) / 0.3 : 1.0;

		const double omega = OmegaFromPointRate( pps );
		const double t     = 8.0 / pps;
		double exact;
		if( zeta < 1.0 )
		{
			const double wd = omega * std::sqrt( 1.0 - zeta * zeta );
			exact = std::exp( -zeta * omega * t ) / std::sqrt( 1.0 - zeta * zeta )
			        * std::fabs( std::sin( wd * t + std::acos( zeta ) ) );
		}
		else if( zeta == 1.0 )
		{
			exact = std::exp( -omega * t ) * ( 1.0 + omega * t );
		}
		else
		{
			//Over-damped: two real roots, and the response is the weighted sum
			//of their decays.
			const double root = std::sqrt( zeta * zeta - 1.0 );
			const double s1   = -omega * ( zeta - root );
			const double s2   = -omega * ( zeta + root );
			exact = std::fabs( ( s2 * std::exp( s1 * t ) - s1 * std::exp( s2 * t ) ) / ( s2 - s1 ) );
		}

		const bool settled = std::fabs( residualX - exact ) <= 0.02 * exact
		                     && std::fabs( residualY - exact ) <= 0.02 * exact;
		ok = ok && settled;

		std::printf( "  zeta %.2f: overshoot x %.4f y %.4f, predicted %.4f; after 8 points %.3f%% off, "
		             "closed form %.3f%%  %s\n",
		             zeta, overshootX, overshootY, predicted, residualX * 100.0, exact * 100.0, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	std::printf( failures == 0 ? "step: ok\n" : "step: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --budget
//---------------------------------------------------------------------------
int runBudgetCheck()
{
	int failures = 0;
	constexpr double fps = 60.0;

	struct Case
	{
		double pps;
		int points;
	};
	const Case cases[] = { { 30000.0, 1234 }, { 25000.0, 977 }, { 8000.0, 4000 }, { 60000.0, 333 } };

	for( const Case& c : cases )
	{
		Scanner scanner;
		scanner.Reset();
		scanner.SetIdeal( true );
		scanner.SetPointRate( c.pps );
		std::vector< Point > stream( static_cast< size_t >( c.points ), Point{ 0.5f, 0.5f, 1, 1, 1, true } );
		scanner.SetStream( stream );

		const double perFrame = c.pps / fps;
		const long expectFirst = static_cast< long >( std::ceil( c.points / perFrame ) );
		long firstAt = -1;
		bool ok      = true;
		std::vector< Sample > samples;

		for( long frame = 1; frame <= 40; ++frame )
		{
			samples.clear();
			scanner.Advance( 1.0 / fps, samples );
			const long expectedPoints = static_cast< long >( std::floor( frame * perFrame + 1e-6 ) );
			const long expectedScans  = expectedPoints / c.points;
			if( std::labs( scanner.PointsScanned() - expectedPoints ) > 1 )
				ok = false;
			if( scanner.ScansCompleted() != expectedScans )
				ok = false;
			if( firstAt < 0 && scanner.ScansCompleted() >= 1 )
				firstAt = frame;
		}
		ok = ok && firstAt == expectFirst;

		std::printf( "  %d points at %.0f pps: %.2f points/frame, first scan complete at frame %ld (expected %ld), %ld scans in 40 frames  %s\n",
		             c.points, c.pps, perFrame, firstAt, expectFirst, scanner.ScansCompleted(), ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	//Restart Each Frame: a stream longer than the budget never completes and
	//the cursor is back at the start every frame.
	{
		Scanner scanner;
		scanner.Reset();
		scanner.SetIdeal( true );
		scanner.SetPointRate( 30000.0 );
		scanner.SetRestartEachFrame( true );
		scanner.SetStream( std::vector< Point >( 1234, Point{ 0.5f, 0.5f, 1, 1, 1, true } ) );
		std::vector< Sample > samples;
		bool ok = true;
		for( int frame = 0; frame < 10; ++frame )
		{
			samples.clear();
			scanner.Advance( 1.0 / fps, samples );
			if( scanner.Cursor() != 500 || scanner.ScansCompleted() != 0 )
				ok = false;
		}
		std::printf( "  restart each frame, 1234 points at 30000 pps: cursor %zu after every frame, %ld scans  %s\n",
		             scanner.Cursor(), scanner.ScansCompleted(), ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	std::printf( failures == 0 ? "budget: ok\n" : "budget: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// The bench rig for the renderer: a square path, built by the real stream
// generator from a real contour, so the harness scans what the plugin scans.
//---------------------------------------------------------------------------
constexpr int kTraceW = 320, kTraceH = 180;
constexpr float kAspect = 16.0f / 9.0f;

std::vector< Point > squareStream( float density, int dwell, float lo, float hi )
{
	//In trace pixels: (x + 0.5) / 320 * aspect = u, so x = u * 320 / aspect - 0.5.
	auto px = [ & ]( float u ) { return u * kTraceW / kAspect - 0.5f; };
	auto py = [ & ]( float v ) { return v * kTraceH - 0.5f; };
	Contour square;
	square.closed = true;
	square.points = { { px( lo ), py( lo ) }, { px( hi ), py( lo ) }, { px( hi ), py( hi ) }, { px( lo ), py( hi ) } };
	std::vector< Contour > contours{ square };

	StreamParams sp;
	sp.density         = density;
	sp.cornerDwell     = dwell;
	sp.blankSettle     = kBlankSettle;
	sp.colourMode      = ColourMode::White;
	sp.scanRateFloorHz = 0.0f;
	std::vector< Point > stream;
	BuildStream( contours, kTraceW, kTraceH, kAspect, sp, stream );
	return stream;
}

double sumRgb( const std::vector< float >& rgba )
{
	double sum = 0.0;
	for( size_t i = 0; i < rgba.size(); i += 4 )
		sum += static_cast< double >( rgba[ i ] ) + rgba[ i + 1 ] + rgba[ i + 2 ];
	return sum;
}

//---------------------------------------------------------------------------
// --energy
//---------------------------------------------------------------------------
// The whole claim of the renderer in one number: the light in a frame is
// BeamPower times the beam-on time, whatever the mirrors did with the path.
// The same stream is scanned by twelve different galvos -- four natural
// frequencies spanning the Galvo Speed range and three dampings from ringing
// to sluggish -- at one point rate, so the beam-on time is identical and the
// only thing that changes is where the light lands. Then the point rate is
// varied too, and the light per second of beam-on time must not move.
int runEnergyCheck()
{
	int failures = 0;
	constexpr int W = 1280, H = 720;
	constexpr float kSigma = 0.005f, kPower = 2.0f;

	BeamRenderer beam;
	if( !beam.InitGL() || !beam.Ensure( W, H ) )
	{
		std::fprintf( stderr, "energy: the beam renderer would not initialise\n" );
		return 1;
	}

	const std::vector< Point > stream = squareStream( 100.0f, 3, 0.3f, 0.7f );
	const size_t lit = LitPoints( stream );

	BeamRenderer::Params bp;
	bp.beamPower    = kPower;
	bp.spotSigma    = kSigma;
	bp.decay        = 0.0f;
	bp.colourMode   = 2;
	bp.clearHistory = true;

	std::vector< float > rgba;
	std::vector< Sample > samples;

	//Part one: one point rate, twelve galvos.
	{
		constexpr double pps = 30000.0;
		const double kpps[]  = { 8.0, 15.0, 30.0, 60.0 };
		const double zetas[] = { 0.4, 0.7, 1.2 };
		std::vector< double > totals;

		//The buffer holds deposit per unit picture area; a pixel is 1/H on a
		//side, so the sum over pixels is the energy times H^2.
		const double expected = kPower * static_cast< double >( lit ) / pps * static_cast< double >( H ) * H;

		std::printf( "  %zu points, %zu lit, at %.0f pps: %.4g s of beam on, expected total light %.6e\n",
		             stream.size(), lit, pps, static_cast< double >( lit ) / pps, expected );
		for( double k : kpps )
			for( double zeta : zetas )
			{
				Scanner scanner;
				scanner.Reset();
				scanner.SetPointRate( pps );
				scanner.SetResponse( OmegaFromPointRate( k * 1000.0 ), zeta );
				scanner.SetStream( stream );
				samples.clear();
				scanner.Advance( ( static_cast< double >( stream.size() ) + 0.5 ) / pps, samples );

				if( !beam.Deposit( samples.data(), static_cast< int >( samples.size() ), bp, 0, kAspect ) || !beam.Read( rgba ) )
				{
					std::fprintf( stderr, "energy: render failed\n" );
					return 1;
				}
				const double total = sumRgb( rgba ) / 3.0;//white: r = g = b = deposit
				totals.push_back( total );
				std::printf( "  galvo %2.0f kpps, zeta %.1f: total light %.6e  (%+.3f%% of expected)\n", k, zeta, total,
				             ( total - expected ) / expected * 100.0 );
			}

		double lowest = 1e300, highest = 0.0, mean = 0.0;
		for( double t : totals )
		{
			lowest = std::min( lowest, t );
			highest = std::max( highest, t );
			mean += t / static_cast< double >( totals.size() );
		}
		const double spread = ( highest - lowest ) / mean * 100.0;
		const bool ok       = spread <= 0.5 && std::fabs( mean - expected ) / expected <= 0.02;
		std::printf( "  spread %.4f%% of the mean across twelve galvos (tolerance 0.5%%); mean %+.3f%% of expected (tolerance 2%%)  %s\n",
		             spread, ( mean - expected ) / expected * 100.0, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	//Part two: the light per second of beam-on time across point rates.
	{
		const double rates[] = { 8000.0, 15000.0, 30000.0, 60000.0 };
		std::vector< double > perSecond;
		for( double pps : rates )
		{
			Scanner scanner;
			scanner.Reset();
			scanner.SetPointRate( pps );
			scanner.SetResponse( OmegaFromPointRate( pps ), 0.7 );
			scanner.SetStream( stream );
			samples.clear();
			scanner.Advance( ( static_cast< double >( stream.size() ) + 0.5 ) / pps, samples );
			if( !beam.Deposit( samples.data(), static_cast< int >( samples.size() ), bp, 0, kAspect ) || !beam.Read( rgba ) )
			{
				std::fprintf( stderr, "energy: render failed\n" );
				return 1;
			}
			const double total = sumRgb( rgba ) / 3.0;
			perSecond.push_back( total / ( static_cast< double >( lit ) / pps ) );
			std::printf( "  %5.0f pps, matched galvo: %.6e light per second of beam\n", pps, perSecond.back() );
		}
		double lowest = 1e300, highest = 0.0, mean = 0.0;
		for( double t : perSecond )
		{
			lowest = std::min( lowest, t );
			highest = std::max( highest, t );
			mean += t / static_cast< double >( perSecond.size() );
		}
		const double spread = ( highest - lowest ) / mean * 100.0;
		const bool ok       = spread <= 0.5;
		std::printf( "  spread %.4f%% across 8-60 kpps (tolerance 0.5%%)  %s\n", spread, ok ? "ok" : "FAILED" );
		failures += ok ? 0 : 1;
	}

	beam.DeInitGL();
	std::printf( failures == 0 ? "energy: ok\n" : "energy: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --dwell
//---------------------------------------------------------------------------
// A corner vertex is emitted k extra times, so the beam sits there for k
// point periods with nowhere to go: k Gaussian dots of energy P*T on top of
// the two arms meeting there. The arms' line density is P*T*d per unit length
// and its peak across the line is that over sigma*sqrt(2pi); a dot's peak is
// P*T over 2 pi sigma^2. So the corner is brighter than the middle of a side by
//
//     1 + k / ( d * sigma * sqrt( 2 pi ) )
//
// Ideal mirrors, so the geometry is what was asked for and nothing else adds
// to the corner; with real ones the deceleration adds more, which is the look
// and not something this arithmetic predicts.
int runDwellCheck()
{
	int failures = 0;
	constexpr int W = 1280, H = 720;
	constexpr float kSigma = 0.005f, kPower = 2.0f, kDensity = 100.0f;
	constexpr int kDwell = 4;
	constexpr double pps = 30000.0;

	BeamRenderer beam;
	if( !beam.InitGL() || !beam.Ensure( W, H ) )
	{
		std::fprintf( stderr, "dwell: the beam renderer would not initialise\n" );
		return 1;
	}

	const float lo = 0.3f, hi = 0.7f;
	const std::vector< Point > stream = squareStream( kDensity, kDwell, lo, hi );

	Scanner scanner;
	scanner.Reset();
	scanner.SetIdeal( true );
	scanner.SetPointRate( pps );
	scanner.SetStream( stream );
	std::vector< Sample > samples;
	scanner.Advance( ( static_cast< double >( stream.size() ) + 0.5 ) / pps, samples );

	BeamRenderer::Params bp;
	bp.beamPower    = kPower;
	bp.spotSigma    = kSigma;
	bp.decay        = 0.0f;
	bp.colourMode   = 2;
	bp.clearHistory = true;
	std::vector< float > rgba;
	if( !beam.Deposit( samples.data(), static_cast< int >( samples.size() ), bp, 0, kAspect ) || !beam.Read( rgba ) )
	{
		std::fprintf( stderr, "dwell: render failed\n" );
		return 1;
	}
	beam.DeInitGL();

	auto at = [ & ]( int x, int y ) {
		x = std::clamp( x, 0, W - 1 );
		y = std::clamp( y, 0, H - 1 );
		return static_cast< double >( rgba[ ( static_cast< size_t >( y ) * W + x ) * 4 + 1 ] );
	};
	auto peakAround = [ & ]( double u, double v, int radius ) {
		//Picture units to pixels: x = u / aspect * W, y = v * H, texel centres.
		const int cx = static_cast< int >( std::lround( u / kAspect * W - 0.5 ) );
		const int cy = static_cast< int >( std::lround( v * H - 0.5 ) );
		double peak  = 0.0;
		for( int dy = -radius; dy <= radius; ++dy )
			for( int dx = -radius; dx <= radius; ++dx )
				peak = std::max( peak, at( cx + dx, cy + dy ) );
		return peak;
	};

	//The corners are where BuildStream put them: the trace-pixel vertices
	//converted back to units, which is lo and hi to within a pixel of 320.
	auto unitX = [ & ]( float u ) { return ( std::round( u * kTraceW / kAspect - 0.5f ) + 0.5f ) / kTraceW * kAspect; };
	auto unitY = [ & ]( float v ) { return ( std::round( v * kTraceH - 0.5f ) + 0.5f ) / kTraceH; };
	const double x0 = unitX( lo ), x1 = unitX( hi ), y0 = unitY( lo ), y1 = unitY( hi );

	const int radius = static_cast< int >( std::ceil( 3.0 * kSigma * H ) );
	const double corners[ 4 ] = { peakAround( x0, y0, radius ), peakAround( x1, y0, radius ),
	                              peakAround( x1, y1, radius ), peakAround( x0, y1, radius ) };
	const double sides[ 4 ] = { peakAround( 0.5 * ( x0 + x1 ), y0, radius ), peakAround( x1, 0.5 * ( y0 + y1 ), radius ),
	                            peakAround( 0.5 * ( x0 + x1 ), y1, radius ), peakAround( x0, 0.5 * ( y0 + y1 ), radius ) };

	double cornerMean = 0.0, sideMean = 0.0;
	for( int i = 0; i < 4; ++i )
	{
		cornerMean += corners[ i ] / 4.0;
		sideMean += sides[ i ] / 4.0;
	}
	const double measured  = cornerMean / sideMean;
	const double predicted = 1.0 + kDwell / ( kDensity * kSigma * std::sqrt( 2.0 * kPi ) );

	//And the line itself against its closed form, since the ratio is only as
	//good as its denominator: P * T * d / ( sigma sqrt( 2 pi ) ), in deposit
	//per unit area.
	const double linePeak = kPower / pps * kDensity / ( kSigma * std::sqrt( 2.0 * kPi ) );

	std::printf( "  density %.0f, dwell %d, sigma %.4f: corners %.4g %.4g %.4g %.4g, sides %.4g %.4g %.4g %.4g\n",
	             kDensity, kDwell, kSigma, corners[ 0 ], corners[ 1 ], corners[ 2 ], corners[ 3 ],
	             sides[ 0 ], sides[ 1 ], sides[ 2 ], sides[ 3 ] );
	std::printf( "  side peak %.4g against the closed form %.4g (%+.2f%%)\n", sideMean, linePeak,
	             ( sideMean - linePeak ) / linePeak * 100.0 );
	std::printf( "  corner over side: %.4f measured, %.4f predicted (%+.2f%%, tolerance 5%%)\n", measured, predicted,
	             ( measured - predicted ) / predicted * 100.0 );

	const bool ok = std::fabs( measured - predicted ) <= 0.05 * predicted
	                && std::fabs( sideMean - linePeak ) <= 0.05 * linePeak;
	failures += ok ? 0 : 1;

	std::printf( failures == 0 ? "dwell: ok\n" : "dwell: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
// glFinish on both sides, or this times how fast the driver accepts commands.
// The CPU figure is the plugin's own stopwatch round the readback, the trace
// and the stream -- and the readback is a pipeline stall, so it is reported
// on its own as well.
int runBench( int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides, default controls, the test card.\n\n", frames );
	std::printf( "resolution   ms/frame   of which CPU (readback+trace+stream)   readback alone   stream points\n" );

	for( const Size& size : sizes )
	{
		Galvo plugin;
		FFGLViewportStruct viewport = {};
		viewport.width  = static_cast< FFUInt32 >( size.width );
		viewport.height = static_cast< FFUInt32 >( size.height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return 1;
		}
		Rig rig( size.width, size.height, buildCard( size.width, size.height ) );

		for( int frame = 0; frame < 20; ++frame )
		{
			driveClock( plugin, frame, fps );
			rig.render( plugin );
		}
		glFinish();

		double cpu = 0.0, readback = 0.0;
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
		{
			driveClock( plugin, 20 + frame, fps );
			rig.render( plugin );
			cpu += plugin.LastCpuMillis();
			readback += plugin.LastReadbackMillis();
		}
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;

		std::printf( "%s   %7.3f    %7.3f                                %7.3f          %zu\n", size.name, ms, cpu / frames,
		             readback / frames, plugin.LastStreamSize() );
		plugin.DeInitGL();
	}
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"gvtest -- render and check the Galvo laser-projector effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/galvo.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 12)\n"
		"  --fps N           synthetic frame rate (default 60)\n"
		"  --noise F         per-frame noise on the source, 0..1. What Stability is for.\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its type, default and range, then exit\n"
		"  --trace           the contour tracer, on synthetic masks and through the plugin\n"
		"  --step            the galvo's step response against the second-order prediction\n"
		"  --budget          the point budget: a long frame takes the frames it should\n"
		"  --energy          the light in a frame does not depend on the galvo\n"
		"  --dwell           a corner is brighter than a side by what its dwell predicts\n"
		"  --bench           time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/galvo.png";
	std::string cardPath;
	int width = 1280, height = 720, frames = 12;
	double fps  = 60.0;
	float noise = 0.0f;
	bool wantList = false, wantTrace = false, wantStep = false, wantBudget = false;
	bool wantEnergy = false, wantDwell = false, wantBench = false;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			if( std::sscanf( argv[ ++i ], "%dx%d", &width, &height ) != 2 )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--trace" )
			wantTrace = true;
		else if( argument == "--step" )
			wantStep = true;
		else if( argument == "--budget" )
			wantBudget = true;
		else if( argument == "--energy" )
			wantEnergy = true;
		else if( argument == "--dwell" )
			wantDwell = true;
		else if( argument == "--bench" )
			wantBench = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "size, frames and fps must all be positive\n" );
		return 2;
	}

	//The checks that need no GPU, answered before a context is made -- which
	//also means they run on a machine where making one fails.
	if( wantStep )
		return runStepCheck();
	if( wantBudget )
		return runBudgetCheck();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	if( wantList )
	{
		Galvo plugin;
		return listParameters( plugin );
	}

	CGLContextObj context = createContext();
	if( wantTrace )
	{
		const int result = runTraceCheck( context != nullptr );
		if( context )
			CGLDestroyContext( context );
		return result;
	}
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( wantEnergy )
		result = runEnergyCheck();
	else if( wantDwell )
		result = runDwellCheck();
	else if( wantBench )
		result = runBench( std::max( frames, 30 ), fps );
	else
	{
		Galvo plugin;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
		}

		FFGLViewportStruct viewport = {};
		viewport.width  = static_cast< FFUInt32 >( width );
		viewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return 1;
		}

		const std::vector< unsigned char > card = buildCard( width, height );
		Rig rig( width, height, card );

		//Several frames, not one: the temporal filter needs a few to settle,
		//and the scanner needs at least one frame to have scanned anything.
		for( int frame = 0; frame < frames; ++frame )
		{
			driveClock( plugin, frame, fps );
			if( noise > 0.0f )
				rig.upload( addNoise( card, frame, noise ) );
			if( rig.render( plugin ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
				return 1;
			}
		}

		const std::vector< unsigned char > image = flipRows( readBackRaw( rig.fbo, width, height ), width, height );
		if( !writePng( outPath, width, height, image ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s (%dx%d, %d frames, %zu contours, %zu points in the stream)\n", outPath.c_str(), width,
		             height, frames, plugin.LastContours().size(), plugin.LastStreamSize() );
		plugin.DeInitGL();
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
