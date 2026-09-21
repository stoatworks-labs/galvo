#include "Scanner.h"

#include <algorithm>
#include <cmath>

namespace galvo
{

void Scanner::SetPointRate( double pps )
{
	pointsPerSecond = std::max( pps, 1.0 );
}

void Scanner::SetResponse( double naturalFrequency, double damping )
{
	omega = std::max( naturalFrequency, 1.0 );
	zeta  = std::max( damping, 0.05 );
}

void Scanner::SetBlankingDelay( int points )
{
	blankingDelay = points;
}

void Scanner::SetRestartEachFrame( bool value )
{
	restart = value;
}

void Scanner::SetIdeal( bool value )
{
	ideal = value;
}

void Scanner::SetStream( std::vector< Point > stream )
{
	pending    = std::move( stream );
	hasPending = true;

	//Nothing is being scanned, so there is nothing to wait for the end of.
	if( current.empty() )
	{
		current.swap( pending );
		hasPending = false;
		cursor     = 0;
	}
}

void Scanner::Reset()
{
	position[ 0 ] = position[ 1 ] = 0.0;
	velocity[ 0 ] = velocity[ 1 ] = 0.0;
	current.clear();
	pending.clear();
	hasPending    = false;
	cursor        = 0;
	carry         = 0.0;
	scans         = 0;
	pointsScanned = 0;
	hasLast       = false;
}

//---------------------------------------------------------------------------
// One RK4 step of x'' = omega^2 (u - x) - 2 zeta omega x', with the command u
// held constant across the step -- which it is: the DAC holds each point for
// a whole point period and the substeps subdivide that.
//---------------------------------------------------------------------------
void Scanner::step( double target[ 2 ], double h )
{
	const double w2 = omega * omega;
	const double c  = 2.0 * zeta * omega;

	for( int axis = 0; axis < 2; ++axis )
	{
		const double u = target[ axis ];
		const double x = position[ axis ];
		const double v = velocity[ axis ];

		auto accel = [ & ]( double px, double pv ) { return w2 * ( u - px ) - c * pv; };

		const double k1x = v;
		const double k1v = accel( x, v );
		const double k2x = v + 0.5 * h * k1v;
		const double k2v = accel( x + 0.5 * h * k1x, v + 0.5 * h * k1v );
		const double k3x = v + 0.5 * h * k2v;
		const double k3v = accel( x + 0.5 * h * k2x, v + 0.5 * h * k2v );
		const double k4x = v + h * k3v;
		const double k4v = accel( x + h * k3x, v + h * k3v );

		position[ axis ] = x + h / 6.0 * ( k1x + 2.0 * k2x + 2.0 * k3x + k4x );
		velocity[ axis ] = v + h / 6.0 * ( k1v + 2.0 * k2v + 2.0 * k3v + k4v );
	}
}

//---------------------------------------------------------------------------
void Scanner::Advance( double frameSeconds, std::vector< Sample >& out )
{
	if( restart )
	{
		//Every host frame starts the newest stream from its first point.
		if( hasPending )
		{
			current.swap( pending );
			pending.clear();
			hasPending = false;
		}
		cursor = 0;
	}

	//The budget: pps * dt points, with the fraction carried so the average
	//rate is exact over many frames rather than rounded down every frame.
	const double exact = pointsPerSecond * std::max( frameSeconds, 0.0 ) + carry;
	long budget        = static_cast< long >( std::floor( exact ) );
	carry              = exact - static_cast< double >( budget );

	if( hasLast )
		out.push_back( last );

	if( current.empty() )
	{
		//Nothing to draw. The mirrors sit; the clock still runs.
		pointsScanned += budget;
		return;
	}

	const double period = 1.0 / pointsPerSecond;
	const double h      = period / static_cast< double >( kSubsteps );

	for( long i = 0; i < budget; ++i )
	{
		//Re-read every point: a scan boundary below may have swapped in a
		//stream of a different length.
		const long size = static_cast< long >( current.size() );
		const Point& p  = current[ cursor ];

		//The blanking signal, shifted against the position signal. Modular,
		//because the stream loops: the point before the first is the last.
		long blankIndex = ( static_cast< long >( cursor ) - blankingDelay ) % size;
		if( blankIndex < 0 )
			blankIndex += size;
		const bool on = current[ static_cast< std::size_t >( blankIndex ) ].on;

		double target[ 2 ] = { p.x, p.y };
		const double from[ 2 ] = { position[ 0 ], position[ 1 ] };
		for( int s = 0; s < kSubsteps; ++s )
		{
			if( ideal )
			{
				//The test hook: a mirror with no dynamics, reaching the point
				//by moving in a straight line across the point period. Linear
				//rather than a jump on the first substep, so a lit segment is
				//a uniform line and not a string of dots -- the predictions in
				//gvtest --dwell assume exactly that.
				const double f = static_cast< double >( s + 1 ) / static_cast< double >( kSubsteps );
				position[ 0 ]  = from[ 0 ] + ( target[ 0 ] - from[ 0 ] ) * f;
				position[ 1 ]  = from[ 1 ] + ( target[ 1 ] - from[ 1 ] ) * f;
				velocity[ 0 ] = velocity[ 1 ] = 0.0;
			}
			else
				step( target, h );

			Sample sample;
			sample.x  = static_cast< float >( position[ 0 ] );
			sample.y  = static_cast< float >( position[ 1 ] );
			sample.dt = static_cast< float >( h );
			sample.on = on ? 1.0f : 0.0f;
			sample.r  = p.r;
			sample.g  = p.g;
			sample.b  = p.b;
			out.push_back( sample );
		}

		++pointsScanned;
		if( ++cursor >= current.size() )
		{
			cursor = 0;
			++scans;
			//A scan boundary is where a new frame is taken up, so a frame that
			//takes three host frames to draw is drawn whole and then replaced,
			//rather than torn between two traces.
			if( hasPending )
			{
				current.swap( pending );
				pending.clear();
				hasPending = false;
				if( current.empty() )
					break;
			}
		}
	}

	if( !out.empty() )
	{
		last    = out.back();
		hasLast = true;
	}
}

} // namespace galvo
