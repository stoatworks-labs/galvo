#pragma once

#include "PointStream.h"

#include <cstddef>
#include <vector>

/**
    The galvanometer scanner: two mirrors, a point clock, and a frame budget.
    No GL in here.

    **Each mirror is a second-order system.** A galvo is a mass on a torsion
    spring driven by a servo, and to a first approximation that is a damped
    oscillator with a natural frequency and a damping ratio. The command is
    the point stream, held for one point period; the mirror follows it as
    fast as its bandwidth lets it. Everything a laser show looks like falls out
    of that one fact:

    - a corner is rounded because the mirror cannot change direction in zero
      time, and it is rounded *more* at a lower natural frequency;
    - an under-damped mirror overshoots a corner and rings back through it;
    - a jump takes the mirror a few points to complete, so a beam that comes
      on before it has arrived draws the tail of the approach -- the hook at
      the start of a stroke -- and a beam left on after the mirror has begun
      the next jump draws a tail off the end of one.

    The mirror is integrated with fourth-order Runge-Kutta at four substeps
    per point. Not for the picture -- one step per point would look the same
    -- but so that `gvtest --step` can measure the overshoot against the
    textbook figure to well under a percent, and so the renderer gets four
    intervals per point to spread the energy along.

    **The point clock is the host's clock.** A scanner rated at P points per
    second draws P / fps points in one host frame and no more. A stream
    shorter than that is scanned more than once a frame; a stream longer than
    that is scanned across several frames, continuing where it left off, and
    the picture visibly crawls -- which is exactly what a real projector does
    with a frame that is too busy. Frame Sync = Restart begins the stream
    afresh every host frame instead, so a too-long frame is cut off rather
    than crawled through.

    **Energy is dwell.** Every substep emits one Sample carrying its own dt
    and its beam state, and the renderer deposits BeamPower * dt for each
    interval however far the mirror moved in it. Nothing here scales
    brightness by speed; brightness proportional to dwell is what equal
    energy per unit time *means*.
*/
namespace galvo
{

/// One sample of where the mirrors are. This is also the vertex layout the
/// renderer reads, two per instance, so the fields are laid out for GL.
struct Sample
{
	float x = 0.0f;  ///< picture units, 0..aspect
	float y = 0.0f;  ///< picture units, 0..1, up
	float dt = 0.0f; ///< seconds this sample is held before the next
	float on = 0.0f; ///< 1 when the beam is on for the interval starting here
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float pad = 0.0f;
};

class Scanner
{
public:
	/// Substeps per point. See the class comment.
	static constexpr int kSubsteps = 4;

	void SetPointRate( double pointsPerSecond );
	/// Natural frequency in rad/s and damping ratio, for both mirrors.
	void SetResponse( double omega, double zeta );
	/// Points the blanking signal is delayed by relative to the position
	/// signal. Negative brings the beam on early.
	void SetBlankingDelay( int points );
	void SetRestartEachFrame( bool restart );
	/// Test hook: mirrors that follow the command exactly. Used by the checks
	/// whose predictions assume the geometry is what was asked for.
	void SetIdeal( bool ideal );

	/// Hand over a newly traced frame. Adopted at the next scan boundary, or
	/// at the next host frame when restarting each frame.
	void SetStream( std::vector< Point > stream );

	/// Scan one host frame's worth of points and append the samples produced.
	/// The first sample appended is the previous frame's last one, so the
	/// intervals chain across frames without a gap.
	void Advance( double frameSeconds, std::vector< Sample >& out );

	/// Complete scans of the stream since Reset.
	long ScansCompleted() const
	{
		return scans;
	}
	/// Points scanned since Reset, blanked ones included.
	long PointsScanned() const
	{
		return pointsScanned;
	}
	std::size_t StreamSize() const
	{
		return current.size();
	}
	std::size_t Cursor() const
	{
		return cursor;
	}
	/// Where the mirrors are, in picture units.
	Vec2 MirrorPosition() const
	{
		return Vec2{ static_cast< float >( position[ 0 ] ), static_cast< float >( position[ 1 ] ) };
	}

	void Reset();

private:
	void step( double target[ 2 ], double h );

	double pointsPerSecond = 30000.0;
	double omega           = 21428.0;
	double zeta            = 0.7;
	int blankingDelay      = 0;
	bool restart           = false;
	bool ideal             = false;

	double position[ 2 ] = { 0.0, 0.0 };
	double velocity[ 2 ] = { 0.0, 0.0 };

	std::vector< Point > current;
	std::vector< Point > pending;
	bool hasPending = false;

	std::size_t cursor  = 0;
	double carry        = 0.0;
	long scans          = 0;
	long pointsScanned  = 0;

	Sample last;
	bool hasLast = false;
};

} // namespace galvo
