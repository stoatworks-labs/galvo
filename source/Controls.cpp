#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace galvo
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}
} // namespace

float ThresholdFromParam( float value )
{
	return geometric( 0.02f, 1.0f, value );
}

float DetailFromParam( float value )
{
	return lerp( 0.0f, 3.0f, value );
}

float AttackFromParam( float value )
{
	//Stays fast across the whole range. Stability is allowed to buy
	//persistence but is not allowed to buy lag: a contour that arrives late is
	//attached to something the eye is already tracking.
	return lerp( 1.0f, 0.75f, value );
}

float ReleaseFromParam( float value )
{
	//1.0 is no persistence at all. 0.02 is a time constant of fifty frames,
	//which is about as long as an edge can hang on after the thing that made
	//it has gone before it reads as a ghost.
	return geometric( 1.0f, 0.02f, value );
}

float MinLengthFromParam( float value )
{
	return lerp( 0.0f, 60.0f, value );
}

float SimplifyFromParam( float value )
{
	return lerp( 0.2f, 4.0f, value );
}

float DampingFromParam( float value )
{
	return lerp( 0.4f, 1.2f, value );
}

double OmegaFromPointRate( double pointsPerSecond )
{
	return 4.0 * pointsPerSecond / ( 8.0 * 0.7 );
}

float DensityFromParam( float value )
{
	return geometric( 20.0f, 400.0f, value );
}

float ScanRateFloorFromParam( float value )
{
	const float hz = lerp( 0.0f, 30.0f, value );
	return hz < 1.0f ? 0.0f : hz;
}

float SpotSigmaFromParam( float value )
{
	return geometric( 0.0015f, 0.02f, value );
}

float BeamPowerFromParam( float value )
{
	return geometric( 0.3f, 30.0f, value );
}

float PersistenceTauFromParam( float value )
{
	if( value <= 0.0f )
		return 0.0f;
	return geometric( 0.004f, 1.0f, value );
}

float HueSpreadFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

} // namespace galvo
