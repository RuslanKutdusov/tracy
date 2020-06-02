#ifndef __TRACYSCOPED_HPP__
#define __TRACYSCOPED_HPP__

#include <stdint.h>
#include <string.h>

#include "../common/TracySystem.hpp"
#include "../common/TracyAlign.hpp"
#include "../common/TracyAlloc.hpp"
#include "TracyProfiler.hpp"

namespace tracy
{

class ScopedZone
{
public:
    tracy_force_inline ScopedZone( const SourceLocationData* srcloc, bool active )
    {
        Profiler::BeginZone( srcloc, active );
    }

    tracy_force_inline ScopedZone( const SourceLocationData* srcloc, int depth, bool active )
    {
        Profiler::BeginZone<true>( srcloc, active, depth );
    }

    tracy_force_inline ~ScopedZone()
    {
        Profiler::EndZone();
    }

    tracy_force_inline void Text( const char* txt, size_t size )
    {
        Profiler::ZoneText( txt, size );
    }

    tracy_force_inline void Name( const char* txt, size_t size )
    {
        Profiler::ZoneName( txt, size );
    }

    tracy_force_inline void Value( uint64_t value )
    {
        Profiler::ZoneValue( value );
    }
};

}

#endif
