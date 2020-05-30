#ifndef __TRACYSYSTEM_HPP__
#define __TRACYSYSTEM_HPP__

#include <stdint.h>

#include "TracyApi.h"

namespace tracy
{

TRACY_API uint64_t GetThreadHandle();
TRACY_API void SetThreadName( const char* name );
TRACY_API const char* GetThreadName( uint64_t id );

}

#endif
