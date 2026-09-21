#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every FF_TYPE_STANDARD parameter this plugin declares is a plain float in
    0..1, including the ones that stand for a length in trace pixels or a
    damping ratio. That is not a style preference: `SetParamInfo` clamps a
    standard default into 0..1 *before* returning, and `SetParamRange` can only
    be called afterwards -- so a parameter declared in pixels cannot declare a
    default in pixels. The conversions live here instead, in one file the
    plugin and the harness both use, so there is only ever one answer to what
    a slider position means.

    The integer parameters are the exception, and it is the SDK's exception,
    not ours: FF_TYPE_INTEGER passes its default through untouched, so Galvo
    Speed really is declared in kpps and Trace Size really is declared in
    pixels. Those need no mapping and have none here.

    Where a mapping is geometric rather than linear it is because the
    interesting range is at one end. A slider is a fixed number of pixels wide
    and spending half of them on a range nobody uses is the difference between
    a control that works and one with a sweet spot at 0.03.
*/
namespace galvo
{

//--- Detect -----------------------------------------------------------------

/// 0.02 to 1.0, geometrically. The gradient magnitude at which a pixel is an
/// edge; a clean black-to-white step measures 1.0, so the useful range for
/// footage is below 0.2 and for artwork around 0.3.
float ThresholdFromParam( float value );

/// 0 to 3, linear. Mip levels *above* the trace resolution that the Sobel
/// runs at: 0 detects at the trace buffer's own pixel, 3 finds only the shape
/// of a logo and ignores everything inside it.
float DetailFromParam( float value );

/// The two halves of the temporal filter, from one Stability control.
/// Asymmetric on purpose: rise nearly instantly, fall slowly. See the
/// stabilise shader.
float AttackFromParam( float value );
float ReleaseFromParam( float value );

/// 0 to 60 trace pixels, linear. Contours shorter than this are not scanned.
float MinLengthFromParam( float value );

/// 0.2 to 4 trace pixels, linear. The Douglas-Peucker tolerance: how far the
/// simplified polyline may stray from the traced one. Below about 0.7 the
/// staircase of a thresholded mask survives as a stitch along every edge.
float SimplifyFromParam( float value );

//--- Scanner ----------------------------------------------------------------

/// 0.4 to 1.2, linear. The galvo's damping ratio. Under 1 overshoots and
/// rings; over 1 rounds every corner without ringing.
float DampingFromParam( float value );

/// The mirror's natural frequency in rad/s, from the scanner's rating in
/// points per second.
///
/// The mapping is ours and it is stated so it can be argued with: a scanner
/// rated at P pps is taken to settle a step to within 2% in eight point
/// periods at a damping of 0.7, which is roughly what the ILDA test pattern's
/// corners demand of it. The 2% settling time of a second-order system is
/// 4 / (zeta * omega), so omega = 4 P / (8 * 0.7) = 0.714 P. A 30 kpps
/// scanner comes out at 3.4 kHz, which is the right order for a real one.
///
/// Note that 4 / (zeta * omega) is the decay ENVELOPE reaching 2%, not the
/// response, and the response is that envelope times 1 / sqrt(1 - zeta^2)
/// times a sine. So the real residual eight points after a step at zeta = 0.7
/// is 2.53%, which `gvtest --step` measures against the exact closed form
/// rather than against this rule of thumb. The rule of thumb is what sets
/// omega; it is not a claim about what the mirror then does.
double OmegaFromPointRate( double pointsPerSecond );

/// 20 to 400 points per picture height, geometrically. Points per unit length
/// along a contour; the picture height is the unit because the spot is.
float DensityFromParam( float value );

/// 0 (off) to 30 Hz, linear. Below 1 Hz reads as off.
float ScanRateFloorFromParam( float value );

//--- Beam -------------------------------------------------------------------

/// The spot's Gaussian sigma as a fraction of the picture height, 0.0015 to
/// 0.02, geometrically. 0.0015 of 1080 lines is 1.6 pixels, which is where a
/// spot stops being a spot and starts being a pixel.
float SpotSigmaFromParam( float value );

/// Beam power in light per second of beam-on time, 0.3 to 30, geometrically,
/// so that 0.5 is 3.0. Chosen so a default-density line at a default spot
/// reads at about unit brightness; see the note in Beam.h for the arithmetic.
float BeamPowerFromParam( float value );

/// The exposure's time constant in seconds, 0.004 to 1.0 geometrically, with
/// zero meaning no persistence at all. A camera's shutter, not a phosphor: the
/// accumulation buffer keeps exp( -dt / tau ) of itself each host frame.
float PersistenceTauFromParam( float value );

/// 0 to 2 turns of hue along the point stream.
float HueSpreadFromParam( float value );

//--- Constants the controls do not reach -------------------------------------

/// Turn angle, in degrees, above which a simplified vertex is a corner and
/// gets its dwell points.
constexpr float kCornerAngleDegrees = 30.0f;

/// Blanked points held at a contour's start before the beam comes on, on top
/// of Corner Dwell. Real show software calls these pre-blanking points: they
/// give the mirrors time to arrive before there is anything to see.
constexpr int kBlankSettle = 2;

/// The threshold's soft shoulder, as a fraction of the threshold. Fixed rather
/// than a control: the tracer needs a binary mask, so all a wider shoulder
/// buys is a fatter band for the thinner to eat.
constexpr float kThresholdSoftness = 0.3f;

/// How much the Dimmed Clip background dims the clip.
constexpr float kDimLevel = 0.25f;

/// How far a Clip-coloured beam is pushed toward a pure laser colour: 1 is
/// the colour normalised to its brightest channel, 0 is the clip as it is.
constexpr float kLaserSaturation = 0.85f;

} // namespace galvo
