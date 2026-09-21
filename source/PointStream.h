#pragma once

#include "Tracer.h"

#include <vector>

/**
    From ordered contours to an ILDA-style point stream. No GL in here.

    A laser frame is a list of points: where the mirrors are told to go, and
    whether the beam is on when they get there. Everything a real show
    controller does to a vector drawing before it becomes that list happens
    here, in the order it happens there:

    - **Order** the contours into a tour, greedily by nearest endpoint, with
      each open contour free to be scanned backwards and each closed one free
      to start at whichever vertex is nearest. Blanked travel is time the
      beam spends drawing nothing, so the tour exists to cut it.
    - **Sample** each segment at a fixed density of points per unit length.
    - **Dwell** at corners: a sharp vertex is emitted several times over, so
      the mirrors have time to turn. That repeat is what puts the bright dot
      on every corner of a laser-drawn square -- the beam is on, and it is
      not going anywhere.
    - **Blank** the jumps: a contour ends with one blanked copy of its last
      point, and the next begins with a few blanked copies of its first, so
      the mirrors have arrived before the beam comes on.
    - **Decimate** to a scan-rate floor when the frame has more points than
      the scanner can draw at that rate, the way show software does when a
      frame is too busy.

    Coordinates are picture units: x in [0, aspect], y in [0, 1], y up.
*/
namespace galvo
{

struct Point
{
	float x = 0.0f;
	float y = 0.0f;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	bool on = false;
};

enum class ColourMode
{
	Clip = 0,   ///< The clip's colour under the point, pushed toward a laser primary.
	Palette,    ///< One colour, or a hue sweep along the stream.
	White,
	Count
};

struct StreamParams
{
	/// Points per unit of path length, the unit being the picture height.
	float density = 100.0f;
	/// Extra copies of a corner vertex.
	int cornerDwell = 3;
	/// Blanked copies of a contour's first point, on top of cornerDwell.
	int blankSettle = 2;

	ColourMode colourMode = ColourMode::Clip;
	float colour[ 3 ]     = { 1.0f, 1.0f, 1.0f };
	/// Turns of hue from the first point of the stream to the last, Palette
	/// mode only.
	float hueSpread = 0.0f;

	/// The scanner's rate, and the slowest scan rate allowed. When the stream
	/// would scan slower than the floor it is thinned to fit; a floor of zero
	/// never thins.
	float pointsPerSecond = 30000.0f;
	float scanRateFloorHz = 0.0f;
};

/// Reorder `contours` in place into a nearest-neighbour tour starting from
/// `cursor`, reversing open contours and rotating closed ones as needed.
void OrderContours( std::vector< Contour >& contours, Vec2 cursor );

/// Build the stream. `contours` are in trace-pixel coordinates over a mask of
/// `traceWidth` x `traceHeight`; `aspect` is the picture's width over height.
/// The result replaces `out`. Returns the number of points the stream would
/// have had before any decimation, so a caller can report how far over budget
/// the frame was.
std::size_t BuildStream( const std::vector< Contour >& contours, int traceWidth, int traceHeight, float aspect,
                         const StreamParams& params, std::vector< Point >& out );

/// The number of lit points in a stream, for the harness.
std::size_t LitPoints( const std::vector< Point >& stream );

} // namespace galvo
