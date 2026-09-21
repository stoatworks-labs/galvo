#include "PointStream.h"

#include "Controls.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace galvo
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

float distanceSquared( const Vec2& a, const Vec2& b )
{
	return ( a.x - b.x ) * ( a.x - b.x ) + ( a.y - b.y ) * ( a.y - b.y );
}

/// HSV to RGB, h in turns.
void hsvToRgb( float h, float s, float v, float& r, float& g, float& b )
{
	h = h - std::floor( h );
	const float i = std::floor( h * 6.0f );
	const float f = h * 6.0f - i;
	const float p = v * ( 1.0f - s );
	const float q = v * ( 1.0f - s * f );
	const float t = v * ( 1.0f - s * ( 1.0f - f ) );
	switch( static_cast< int >( i ) % 6 )
	{
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
}

void rgbToHsv( float r, float g, float b, float& h, float& s, float& v )
{
	const float mx = std::max( r, std::max( g, b ) );
	const float mn = std::min( r, std::min( g, b ) );
	v              = mx;
	const float d  = mx - mn;
	s              = mx > 1e-6f ? d / mx : 0.0f;
	if( d < 1e-6f )
	{
		h = 0.0f;
		return;
	}
	if( mx == r )
		h = ( g - b ) / d + ( g < b ? 6.0f : 0.0f );
	else if( mx == g )
		h = ( b - r ) / d + 2.0f;
	else
		h = ( r - g ) / d + 4.0f;
	h /= 6.0f;
}

/// One pass of the generator at a given density and dwell. Separated from
/// BuildStream so the scan-rate floor can run it again, thinner.
void generate( const std::vector< Contour >& contours, int traceWidth, int traceHeight, float aspect,
               const StreamParams& params, float density, int cornerDwell, std::vector< Point >& out )
{
	out.clear();

	const float sx = aspect / static_cast< float >( std::max( traceWidth, 1 ) );
	const float sy = 1.0f / static_cast< float >( std::max( traceHeight, 1 ) );
	auto toUnits    = [ & ]( const Vec2& p ) {
		return Vec2{ ( p.x + 0.5f ) * sx, ( p.y + 0.5f ) * sy };
	};

	const float cosCorner = std::cos( kCornerAngleDegrees * kPi / 180.0f );

	auto emit = [ & ]( const Vec2& p, bool on ) {
		Point point;
		point.x  = p.x;
		point.y  = p.y;
		point.on = on;
		out.push_back( point );
	};

	//Is vertex `v` a corner? Compare the direction in with the direction out.
	//The first vertex of an OPEN contour has no direction in and is not one.
	auto isCorner = [ & ]( const Contour& contour, std::size_t v ) {
		const std::size_t n = contour.points.size();
		if( !contour.closed && ( v == 0 || v + 1 >= n ) )
			return false;
		const Vec2 a    = toUnits( contour.points[ v ] );
		const Vec2 prev = toUnits( contour.points[ ( v + n - 1 ) % n ] );
		const Vec2 next = toUnits( contour.points[ ( v + 1 ) % n ] );
		const float ix = a.x - prev.x, iy = a.y - prev.y;
		const float ox = next.x - a.x, oy = next.y - a.y;
		const float il = std::sqrt( ix * ix + iy * iy );
		const float ol = std::sqrt( ox * ox + oy * oy );
		if( il <= 1e-6f || ol <= 1e-6f )
			return false;
		return ( ix * ox + iy * oy ) / ( il * ol ) < cosCorner;
	};

	for( const Contour& contour : contours )
	{
		const std::size_t n = contour.points.size();
		if( n < 2 )
			continue;

		//Pre-blanking: the mirrors are sent to the start and given time to get
		//there before there is anything to see.
		const Vec2 start = toUnits( contour.points[ 0 ] );
		for( int k = 0; k < params.blankSettle + cornerDwell; ++k )
			emit( start, false );

		const std::size_t segments = contour.closed ? n : n - 1;
		for( std::size_t s = 0; s < segments; ++s )
		{
			const Vec2 a = toUnits( contour.points[ s ] );
			const Vec2 b = toUnits( contour.points[ ( s + 1 ) % n ] );

			//A closed contour's first vertex is emitted at the ARRIVAL below,
			//not here. The beam passes through it once per scan, so it must be
			//emitted once per scan: emitting it at both the departure and the
			//arrival gave it one lit point more than every other corner, and
			//`gvtest --dwell` duly measured that one corner of a square as 19%
			//brighter than the other three. The arrival is the right end to
			//keep, because that is where the mirror is actually decelerating.
			if( !( contour.closed && s == 0 ) )
			{
				emit( a, true );
				if( isCorner( contour, s ) )
					for( int k = 0; k < cornerDwell; ++k )
						emit( a, true );
			}

			//Interior points along the segment, at the density. The endpoint is
			//the next segment's `a`, or the closing vertex below.
			const float length = std::sqrt( distanceSquared( a, b ) );
			const int steps    = std::max( 1, static_cast< int >( std::ceil( length * density ) ) );
			for( int i = 1; i < steps; ++i )
			{
				const float t = static_cast< float >( i ) / static_cast< float >( steps );
				emit( Vec2{ a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t }, true );
			}
		}

		//The arrival. A closed contour lands back on its first vertex and an
		//open one stops at its last, and both need the mirror to come to rest
		//before the beam goes out -- so both get the dwell a corner gets. That
		//is what a real controller's end-of-line points are for, and it is why
		//the end of a stroke is as bright as a corner rather than fading.
		const std::size_t endVertex = contour.closed ? 0 : n - 1;
		const Vec2 end              = toUnits( contour.points[ endVertex ] );
		emit( end, true );
		if( contour.closed ? isCorner( contour, 0 ) : true )
			for( int k = 0; k < cornerDwell; ++k )
				emit( end, true );

		//Post-blanking: the beam is off before the mirrors leave.
		emit( end, false );
	}

	//Colour. Assigned after the geometry so a hue sweep can be spread over
	//the whole stream rather than per contour.
	const std::size_t total = out.size();
	if( params.colourMode == ColourMode::Palette )
	{
		float h = 0.0f, s = 0.0f, v = 0.0f;
		rgbToHsv( params.colour[ 0 ], params.colour[ 1 ], params.colour[ 2 ], h, s, v );
		//A hue sweep on a grey would be no sweep at all, so the moment there
		//is a spread the colour is taken at full saturation -- which is what a
		//laser's colour is anyway.
		if( params.hueSpread > 0.0f )
			s = 1.0f;
		for( std::size_t i = 0; i < total; ++i )
		{
			const float along = total > 1 ? static_cast< float >( i ) / static_cast< float >( total - 1 ) : 0.0f;
			hsvToRgb( h + params.hueSpread * along, s, v, out[ i ].r, out[ i ].g, out[ i ].b );
		}
	}
	//Clip and White both carry unit colour here. Clip is applied on the GPU,
	//where the clip actually is, by the trace shader.
}
} // namespace

//---------------------------------------------------------------------------
void OrderContours( std::vector< Contour >& contours, Vec2 cursor )
{
	std::vector< Contour > ordered;
	ordered.reserve( contours.size() );
	std::vector< bool > taken( contours.size(), false );

	for( std::size_t placed = 0; placed < contours.size(); ++placed )
	{
		std::size_t best  = contours.size();
		std::size_t bestAt = 0;
		float bestCost     = 1e30f;
		bool bestReverse   = false;

		for( std::size_t i = 0; i < contours.size(); ++i )
		{
			if( taken[ i ] )
				continue;
			const Contour& c = contours[ i ];
			if( c.closed )
			{
				//Any vertex can start a loop, so the nearest one does.
				for( std::size_t k = 0; k < c.points.size(); ++k )
				{
					const float d = distanceSquared( c.points[ k ], cursor );
					if( d < bestCost )
					{
						bestCost    = d;
						best        = i;
						bestAt      = k;
						bestReverse = false;
					}
				}
			}
			else
			{
				const float df = distanceSquared( c.points.front(), cursor );
				const float db = distanceSquared( c.points.back(), cursor );
				if( df < bestCost )
				{
					bestCost    = df;
					best        = i;
					bestAt      = 0;
					bestReverse = false;
				}
				if( db < bestCost )
				{
					bestCost    = db;
					best        = i;
					bestAt      = 0;
					bestReverse = true;
				}
			}
		}

		if( best >= contours.size() )
			break;

		taken[ best ] = true;
		Contour c     = contours[ best ];
		if( c.closed && bestAt != 0 )
			std::rotate( c.points.begin(), c.points.begin() + static_cast< std::ptrdiff_t >( bestAt ), c.points.end() );
		if( bestReverse )
			std::reverse( c.points.begin(), c.points.end() );

		cursor = c.closed ? c.points.front() : c.points.back();
		ordered.push_back( std::move( c ) );
	}

	contours.swap( ordered );
}

//---------------------------------------------------------------------------
std::size_t BuildStream( const std::vector< Contour >& contours, int traceWidth, int traceHeight, float aspect,
                         const StreamParams& params, std::vector< Point >& out )
{
	generate( contours, traceWidth, traceHeight, aspect, params, params.density, params.cornerDwell, out );
	const std::size_t wanted = out.size();

	if( params.scanRateFloorHz <= 0.0f || params.pointsPerSecond <= 0.0f )
		return wanted;

	const std::size_t budget =
		static_cast< std::size_t >( std::max( 1.0f, params.pointsPerSecond / params.scanRateFloorHz ) );
	if( wanted <= budget )
		return wanted;

	//Over budget: thin the density and the dwell in proportion and try again.
	//Twice, because the per-segment ceil() and the fixed blanking points do
	//not scale, so the first attempt lands a little over. Whatever is still
	//over after that is cut from the end, which is what a real controller does
	//when a frame will not fit: the tail of the drawing is simply not drawn.
	float scale = static_cast< float >( budget ) / static_cast< float >( wanted );
	for( int attempt = 0; attempt < 2 && out.size() > budget; ++attempt )
	{
		const float density = std::max( 1.0f, params.density * scale * 0.95f );
		const int dwell     = static_cast< int >( std::floor( static_cast< float >( params.cornerDwell ) * scale ) );
		generate( contours, traceWidth, traceHeight, aspect, params, density, dwell, out );
		scale *= static_cast< float >( budget ) / static_cast< float >( std::max< std::size_t >( out.size(), 1 ) );
	}
	if( out.size() > budget )
		out.resize( budget );

	return wanted;
}

std::size_t LitPoints( const std::vector< Point >& stream )
{
	std::size_t lit = 0;
	for( const Point& p : stream )
		lit += p.on ? 1 : 0;
	return lit;
}

} // namespace galvo
