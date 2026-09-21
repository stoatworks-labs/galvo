#pragma once

/**
    The passes, as GLSL source.

    The detect chain, in the order it runs:

    1. **copy**      picture size, mipmapped. Resolves MaxUV once so nothing
                     after it has to think about the host's padding, and the
                     mip chain is what lets the edge pass detect at a scale.
    2. **edge**      TRACE size. Sobel on the selected channel, reading the
                     copy at the mip level that matches the trace resolution
                     plus Detail. This is the pass that makes the readback
                     small: the mask is born at 320 pixels wide, not shrunk to
                     it afterwards.
    3. **stabilise** trace size, ping-ponged against its own previous output.
                     The asymmetric temporal filter, then the threshold. Its
                     red channel is what the CPU reads back.

    Then the beam, in render/Beam.cpp's order:

    4. **decay**     picture size. The accumulation buffer times the
                     persistence, into the other accumulation buffer.
    5. **trace**     one instanced quad per scanner interval, additive, into
                     the buffer the decay just wrote. The energy-conserving
                     segment renderer from vectrix, with a colour per sample.
    6. **composite** output size. Background mode and mix, to the host.

    The trace shaders are assembled from a shared constants string plus a body,
    because the vertex stage sizing the quad and the fragment stage
    subtracting the Gaussian's pedestal have to agree about one number.
    `tools/verify.sh` mirrors that assembly when it compiles them.
*/

#include <string>

namespace galvo
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kEdgeShader;
extern const char* const kStabiliseShader;
extern const char* const kCompositeShader;
extern const char* const kDecayShader;

/// The beam's trace pass, assembled around kBeamConstants.
std::string TraceVertexSource();
std::string TraceFragmentSource();

} // namespace galvo
