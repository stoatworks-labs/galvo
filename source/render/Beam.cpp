#include "render/Beam.h"

#include "Diag.h"
#include "Shaders.h"

// FFGLSDK.h includes every other scoped binding and omits this one (SDK
// b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

using namespace ffglex;

namespace galvo
{
namespace
{
/// A runaway cannot be allowed to climb to the top of a 16-bit float and
/// become an inf. Far above anything a picture holds, so it never shapes one.
constexpr float kCeiling = 6.0e4f;

/// The GL state this renderer changes, captured so it can be put back.
/// FFGL requires the context returned in a default state, and Resolume
/// renders the rest of the composition with whatever it finds -- a plugin
/// that leaves additive blending on makes the *next* effect look broken.
struct ScopedGLState
{
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	GLboolean blend      = GL_FALSE;
	GLint srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;

	ScopedGLState()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		blend = glIsEnabled( GL_BLEND );
		glGetIntegerv( GL_BLEND_SRC_RGB, &srcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &dstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &srcA );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &dstA );
	}
	~ScopedGLState()
	{
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glBlendFuncSeparate( static_cast< GLenum >( srcRGB ), static_cast< GLenum >( dstRGB ),
		                     static_cast< GLenum >( srcA ), static_cast< GLenum >( dstA ) );
		if( blend )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		glBindVertexArray( 0 );
	}
	ScopedGLState( const ScopedGLState& ) = delete;
	ScopedGLState& operator=( const ScopedGLState& ) = delete;
};
} // namespace

bool BeamRenderer::InitGL()
{
	const std::string traceVertex   = TraceVertexSource();
	const std::string traceFragment = TraceFragmentSource();

	if( !decayShader.Compile( kVertexShader, kDecayShader ) )
	{
		diag::error( "the decay shader failed to compile - the effect will do nothing" );
		return false;
	}
	if( !traceShader.Compile( traceVertex.c_str(), traceFragment.c_str() ) )
	{
		diag::error( "the trace shader failed to compile - the effect will do nothing" );
		return false;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		return false;
	}

	glGenVertexArrays( 1, &traceVAO );
	glGenBuffers( 1, &traceVBO );
	if( traceVAO == 0 || traceVBO == 0 )
	{
		diag::error( "failed to create the trace vertex array" );
		return false;
	}

	//One buffer of Samples, read twice. Attributes 0/1 start at the beginning
	//and 2/3 one Sample in, so instance i sees samples i and i+1. That is why
	//the draw must ask for n-1 instances: n would read one past the end.
	//
	//glVertexAttribDivisor is VAO state: set it with this VAO bound or it
	//lands somewhere worse than nowhere.
	glBindVertexArray( traceVAO );
	glBindBuffer( GL_ARRAY_BUFFER, traceVBO );

	const GLsizei stride = static_cast< GLsizei >( sizeof( Sample ) );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, stride, nullptr );
	glVertexAttribDivisor( 0, 1 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( float ) * 4 ) );
	glVertexAttribDivisor( 1, 1 );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( Sample ) ) );
	glVertexAttribDivisor( 2, 1 );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 4, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( sizeof( Sample ) + sizeof( float ) * 4 ) );
	glVertexAttribDivisor( 3, 1 );

	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	current = 0;
	return true;
}

void BeamRenderer::DeInitGL()
{
	decayShader.FreeGLResources();
	traceShader.FreeGLResources();
	quad.Release();

	if( traceVBO != 0 )
	{
		glDeleteBuffers( 1, &traceVBO );
		traceVBO = 0;
	}
	if( traceVAO != 0 )
	{
		glDeleteVertexArrays( 1, &traceVAO );
		traceVAO = 0;
	}
	accumulation[ 0 ].Destroy();
	accumulation[ 1 ].Destroy();
	width = height = 0;
}

bool BeamRenderer::Ensure( int requestedWidth, int requestedHeight )
{
	//Nearest, because the composite reads it texel-for-texel at the same size
	//and the harness reads it back as data.
	if( !accumulation[ 0 ].Ensure( requestedWidth, requestedHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	    || !accumulation[ 1 ].Ensure( requestedWidth, requestedHeight, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
		return false;
	width  = requestedWidth;
	height = requestedHeight;
	return true;
}

bool BeamRenderer::Deposit( const Sample* samples, int n, const Params& params, GLuint copyTexture, float aspect )
{
	if( width <= 0 || height <= 0 || !decayShader.IsReady() || !traceShader.IsReady() )
		return false;

	ScopedGLState state;

	if( samples == nullptr )
		n = 0;
	const int segments = std::max( 0, n - 1 );

	if( params.clearHistory )
	{
		accumulation[ 0 ].Clear();
		accumulation[ 1 ].Clear();
	}

	const int target  = 1 - current;
	const int history = current;

	//Upload. GL_STREAM_DRAW and a fresh glBufferData every frame, so the
	//driver orphans the old storage rather than waiting for last frame's draw
	//to finish reading it.
	if( n > 0 )
	{
		glBindBuffer( GL_ARRAY_BUFFER, traceVBO );
		glBufferData( GL_ARRAY_BUFFER,
		              static_cast< GLsizeiptr >( static_cast< std::size_t >( n ) * sizeof( Sample ) ),
		              samples, GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}

	{
		ScopedFBOBinding fbo( accumulation[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		//ScopedFBOBinding restores the framebuffer and says nothing about the
		//viewport.
		glViewport( 0, 0, width, height );

		//1. Decay.
		{
			glDisable( GL_BLEND );
			ScopedShaderBinding shader( decayShader.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, accumulation[ history ].TextureID() );
			decayShader.Set( "HistoryTexture", 0 );
			decayShader.Set( "Decay", std::clamp( params.decay, 0.0f, 1.0f ) );
			decayShader.Set( "Ceiling", kCeiling );
			quad.Draw();
		}

		//2. Trace, added on top. Sum, not max: two intervals crossing the same
		//texel really did put twice the light there, and a max() would throw
		//away every crossing and every dwell -- which is where a laser frame is
		//brightest and the reason this plugin exists.
		if( segments > 0 && params.beamPower > 0.0f )
		{
			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE );

			ScopedShaderBinding shader( traceShader.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			//The harness renders with no clip at all, and the shader never
			//samples CopyTexture unless Colour Mode is Clip -- but the sampler
			//is still declared and still bound to a unit, and binding texture 0
			//there makes the driver log "unit 0 ... is unloadable and bound to
			//sampler type (Float)". The picture is correct either way; it is
			//also a warning per instance in Resolume's log from a plugin that is
			//working perfectly. Point the unit at the buffer we are NOT drawing
			//into -- which exists, and is never sampled.
			glBindTexture( GL_TEXTURE_2D, copyTexture != 0 ? copyTexture : accumulation[ history ].TextureID() );

			traceShader.Set( "CopyTexture", 0 );
			traceShader.Set( "BeamPower", params.beamPower );
			traceShader.Set( "SpotSigma", std::max( params.spotSigma, 1e-5f ) );
			traceShader.Set( "Aspect", std::max( aspect, 1e-3f ) );
			traceShader.Set( "ColourMode", static_cast< float >( params.colourMode ) );
			//The spot's diameter in picture pixels, as a mip level, so the clip
			//is averaged over about as much of itself as the beam covers.
			traceShader.Set( "ColourLod",
			                 std::log2( std::max( 1.0f, std::max( params.spotSigma, 1e-5f ) * 2.0f * static_cast< float >( height ) ) ) );
			traceShader.Set( "Saturate", std::clamp( params.saturate, 0.0f, 1.0f ) );

			glBindVertexArray( traceVAO );
			glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, segments );
			glBindVertexArray( 0 );
		}
	}

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );

	current = target;
	return true;
}

bool BeamRenderer::Read( std::vector< float >& rgba ) const
{
	if( width <= 0 || height <= 0 )
		return false;

	rgba.assign( static_cast< std::size_t >( width ) * static_cast< std::size_t >( height ) * 4, 0.0f );

	GLint previous = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	glBindFramebuffer( GL_FRAMEBUFFER, accumulation[ current ].GetGLID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, rgba.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previous ) );
	return true;
}

} // namespace galvo
