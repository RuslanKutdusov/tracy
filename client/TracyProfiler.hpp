#ifndef __TRACYPROFILER_HPP__
#define __TRACYPROFILER_HPP__

#include <assert.h>
#include <atomic>
#include <stdint.h>
#include <string.h>

#include "readerwriterqueue.h"
#include "TracyCallstack.hpp"
#include "TracySysTime.hpp"
#include "TracyFastVector.hpp"
#include "../common/TracyQueue.hpp"
#include "../common/TracyAlign.hpp"
#include "../common/TracyAlloc.hpp"
#include "../common/TracyMutex.hpp"
#include "../common/TracyProtocol.hpp"

#if defined _WIN32 || defined __CYGWIN__
#  include <intrin.h>
#endif
#ifdef __APPLE__
#  include <TargetConditionals.h>
#  include <mach/mach_time.h>
#endif

#if defined _WIN32 || defined __CYGWIN__ || ( ( defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64 ) && !defined __ANDROID__ ) || __ARM_ARCH >= 6
#  define TRACY_HW_TIMER
#endif

#if !defined TRACY_HW_TIMER || ( defined __ARM_ARCH && __ARM_ARCH >= 6 && !defined CLOCK_MONOTONIC_RAW )
  #include <chrono>
#endif

#ifndef TracyConcat
#  define TracyConcat(x,y) TracyConcatIndirect(x,y)
#endif
#ifndef TracyConcatIndirect
#  define TracyConcatIndirect(x,y) x##y
#endif

namespace
{
    // to avoid MSVC warning 4127: conditional expression is constant
    template <bool>
    struct compile_time_condition
    {
        static const bool value = false;
    };
    template <>
    struct compile_time_condition<true>
    {
        static const bool value = true;
    };
}

namespace tracy
{

class Profiler;
class Socket;
class UdpBroadcast;

TRACY_API uint64_t GetThreadHandle();
TRACY_API void InitRPMallocThread();

struct SourceLocationData
{
    const char* name;
    const char* function;
    const char* file;
    uint32_t line;
    uint32_t color;
};


#define TracyLfqPrepare( _type ) \
    QueueItem itemVar; \
    QueueItem* item = &itemVar; \
    MemWrite( &item->hdr.type, _type );

#define TracyLfqCommit \
    Profiler::GetThreadContext().queue.enqueue(*item);

#define TracyLfqPrepareC( _type ) \
    tracy::QueueItem itemVar; \
    tracy::QueueItem* item = &itemVar; \
    MemWrite( &item->hdr.type, _type );

#define TracyLfqCommitC \
    tracy::Profiler::GetThreadContext().queue.enqueue(*item);


typedef void(*ParameterCallback)( uint32_t idx, int32_t val );

struct ThreadContext
{
    struct Zone
    {
#ifndef TRACY_NO_VERIFY
       uint32_t id = 0;
#endif
#ifdef TRACY_ON_DEMAND
        uint64_t connectionId = 0;
        const SourceLocationData* srcLoc = nullptr;
#endif
        bool active = false;
    };

    static const uint32_t kQueueMaxBlockSize = 32 * 1024;
    static const uint32_t kInitialQueueSize = 32 * 1024;
    static const uint32_t kZoneStackSize = 256;
    static const uint32_t kZoneStackCanary0Value = 0xDEADBEE0;
    static const uint32_t kZoneStackCanary1Value = 0xDEADBEE1;

    uint64_t threadHandle;
#ifdef TRACY_ON_DEMAND
    std::atomic<bool> isActive = false;
    std::atomic_flag endZoneLock = ATOMIC_FLAG_INIT;
#endif

    moodycamel::ReaderWriterQueue<QueueItem, kQueueMaxBlockSize> queue;
    std::atomic<bool> markedToDeletion = false;
    int32_t zoneStackIdx = -1;
    uint32_t zoneStackCanary0 = kZoneStackCanary0Value;
    Zone zoneStack[kZoneStackSize];
    uint32_t zoneStackCanary1 = kZoneStackCanary1Value;

    ThreadContext() 
        : threadHandle( GetThreadHandle() )
        , queue( kInitialQueueSize )
    {
    }

    void Lock()
    {
        while( endZoneLock.test_and_set( std::memory_order_acquire ) ) {}
    }

    void Unlock()
    {
        endZoneLock.clear( std::memory_order_release );
    }
};

class Profiler
{
    struct FrameImageQueueItem
    {
        void* image;
        uint64_t frame;
        uint16_t w;
        uint16_t h;
        uint8_t offset;
        bool flip;
    };

public:
    Profiler();
    ~Profiler();

    static tracy_force_inline int64_t GetTime()
    {
#ifdef TRACY_HW_TIMER
#  if defined TARGET_OS_IOS && TARGET_OS_IOS == 1
        return mach_absolute_time();
#  elif defined __ARM_ARCH && __ARM_ARCH >= 6
#    ifdef CLOCK_MONOTONIC_RAW
        struct timespec ts;
        clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
        return int64_t( ts.tv_sec ) * 1000000000ll + int64_t( ts.tv_nsec );
#    else
        return std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::high_resolution_clock::now().time_since_epoch() ).count();
#    endif
#  elif defined _WIN32 || defined __CYGWIN__
#    ifdef TRACY_TIMER_QPC
        return GetTimeQpc();
#    else
        return int64_t( __rdtsc() );
#    endif
#  elif defined __i386 || defined _M_IX86
        uint32_t eax, edx;
        asm volatile ( "rdtsc" : "=a" (eax), "=d" (edx) );
        return ( uint64_t( edx ) << 32 ) + uint64_t( eax );
#  elif defined __x86_64__ || defined _M_X64
        uint64_t rax, rdx;
        asm volatile ( "rdtsc" : "=a" (rax), "=d" (rdx) );
        return ( rdx << 32 ) + rax;
#  endif
#else
        return std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::high_resolution_clock::now().time_since_epoch() ).count();
#endif
    }

#ifndef TRACY_NO_VERIFY
    tracy_force_inline uint32_t GetNextZoneId()
    {
        return m_zoneId.fetch_add( 1, std::memory_order_relaxed );
    }
#endif

    tracy_force_inline std::atomic<uint32_t>& GetLockCounter()
    {
        return m_lockCounter;
    }

    tracy_force_inline std::atomic<uint8_t>& GetGpuCtxCounter()
    {
        return m_gpuCtxCounter;
    }

    void InitThreadContext();
    void MarkThreadContextForDeletion();

    static tracy_force_inline Profiler& GetProfiler()
    {
        return *s_instance;
    }

    static tracy_force_inline ThreadContext& GetThreadContext()
    {
        return *s_threadContext;
    }

    static tracy_force_inline QueueItem* QueueSerial()
    {
        auto& p = GetProfiler();
        p.m_serialLock.lock();
        return p.m_serialQueue.prepare_next();
    }

    static tracy_force_inline void QueueSerialFinish()
    {
        auto& p = GetProfiler();
        p.m_serialQueue.commit_next();
        p.m_serialLock.unlock();
    }

    // @TODO: make private
    template<bool kCallstack = false, bool kAllocSrcLoc = false>
    static tracy_force_inline void BeginZoneBase( const SourceLocationData* srcloc, int depth, bool active )
    {
        ThreadContext& ctx = GetThreadContext();
        ThreadContext::Zone& zone = ctx.zoneStack[++ctx.zoneStackIdx];
        zone.active = active;

#ifndef TRACY_NO_VERIFY
        uint32_t id = GetProfiler().GetNextZoneId();
        ZoneVerify( id );
        zone.id = id;
#endif

#ifdef TRACY_ON_DEMAND
        zone.connectionId = GetProfiler().ConnectionId();
        zone.srcLoc = srcloc;
#endif

        QueueType type = QueueType::ZoneBegin;
        if( kAllocSrcLoc && kCallstack )
            type = QueueType::ZoneBeginAllocSrcLocCallstack;
        else if( kAllocSrcLoc && !kCallstack )
            type = QueueType::ZoneBeginAllocSrcLoc;

        TracyLfqPrepare( type );
        MemWrite( &item->zoneBegin.time, Profiler::GetTime() );
        MemWrite( &item->zoneBegin.srcloc, (uint64_t)srcloc );
        TracyLfqCommit;

        if( kCallstack )
           GetProfiler().SendCallstack( depth );
    }
    
    template<bool kCallstack = false>
    static tracy_force_inline void BeginZone( const SourceLocationData* srcloc, bool active, int depth = 0 )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if (!ctx.isActive.load(std::memory_order_acquire))
            return;
#endif
        BeginZoneBase<kCallstack, false>( srcloc, depth, active );
    }

    template<bool kCallstack = false>
    static tracy_force_inline void BeginZone( uint32_t line, const char* source, const char* function, bool active, int depth = 0 )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if (!ctx.isActive.load(std::memory_order_acquire))
            return;
#endif
        SourceLocationData* srcloc = (SourceLocationData*)AllocSourceLocation( line, source, function );
        BeginZoneBase<kCallstack, true>( srcloc, depth, active );
    }

    template<bool kCallstack = false>
    static tracy_force_inline void BeginZone( uint32_t line, const char* source, const char* function, const char* name, size_t nameSz, bool active, int depth = 0 )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if (!ctx.isActive.load(std::memory_order_acquire))
            return;
#endif
        SourceLocationData* srcloc = (SourceLocationData*)AllocSourceLocation( line, source, function, name, nameSz );
        BeginZoneBase<kCallstack, true>( srcloc, depth, active );
    }

    static tracy_force_inline void EndZone()
    {
        ThreadContext& ctx = GetThreadContext();
        ThreadContext::Zone& zone = ctx.zoneStack[ctx.zoneStackIdx--];
        if( !zone.active )
            return;

#ifdef TRACY_ON_DEMAND
        ctx.Lock();
        if( !ctx.isActive.load( std::memory_order_acquire ) )
            return;
#endif

#ifdef TRACY_ON_DEMAND
        if( zone.connectionId != GetProfiler().ConnectionId() )
        {
#   ifndef TRACY_NO_VERIFY
            ZoneVerify( zone.id );
#   endif
            TracyLfqPrepare( QueueType::ZoneBegin );
            MemWrite( &item->zoneBegin.time, time );
            MemWrite( &item->zoneBegin.srcloc, (uint64_t)zone.srcLoc );
            TracyLfqCommit;
        }
        ctx.Unlock();
#endif

#ifndef TRACY_NO_VERIFY
        ZoneVerify( zone.id );
#endif

        TracyLfqPrepare( QueueType::ZoneEnd );
        MemWrite( &item->zoneEnd.time, Profiler::GetTime() );
        TracyLfqCommit;
    }

    static tracy_force_inline void ZoneText( const char* txt, size_t size )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if( !ctx.isActive.load( std::memory_order_acquire ) )
            return;
#endif
        auto ptr = (char*)tracy_malloc( size+1 );
        memcpy( ptr, txt, size );
        ptr[size] = '\0';
        TracyLfqPrepare( QueueType::ZoneText );
        MemWrite( &item->zoneText.text, (uint64_t)ptr );
        TracyLfqCommit;
    }

    static tracy_force_inline void ZoneName( const char* txt, size_t size )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if( !ctx.isActive.load( std::memory_order_acquire ) )
            return;
#endif
        auto ptr = (char*)tracy_malloc( size+1 );
        memcpy( ptr, txt, size );
        ptr[size] = '\0';
        TracyLfqPrepare( QueueType::ZoneName );
        MemWrite( &item->zoneText.text, (uint64_t)ptr );
        TracyLfqCommit;
    }

    static tracy_force_inline void ZoneValue( uint64_t value )
    {
#ifdef TRACY_ON_DEMAND
        ThreadContext& ctx = GetThreadContext();
        if( !ctx.isActive.load( std::memory_order_acquire ) )
            return;
#endif
        TracyLfqPrepare( QueueType::ZoneValue );
        MemWrite( &item->zoneValue.value, value );
        TracyLfqCommit;
    }

    static tracy_force_inline void SendFrameMark( const char* name )
    {
        if( !name ) GetProfiler().m_frameCount.fetch_add( 1, std::memory_order_relaxed );
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( QueueType::FrameMarkMsg );
        MemWrite( &item->frameMark.time, GetTime() );
        MemWrite( &item->frameMark.name, uint64_t( name ) );
        TracyLfqCommit;
    }

    static tracy_force_inline void SendFrameMark( const char* name, QueueType type )
    {
        assert( type == QueueType::FrameMarkMsgStart || type == QueueType::FrameMarkMsgEnd );
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        auto item = QueueSerial();
        MemWrite( &item->hdr.type, type );
        MemWrite( &item->frameMark.time, GetTime() );
        MemWrite( &item->frameMark.name, uint64_t( name ) );
        QueueSerialFinish();
    }

    static tracy_force_inline void SendFrameImage( const void* image, uint16_t w, uint16_t h, uint8_t offset, bool flip )
    {
        auto& profiler = GetProfiler();
#ifdef TRACY_ON_DEMAND
        if( !profiler.IsConnected() ) return;
#endif
        const auto sz = size_t( w ) * size_t( h ) * 4;
        auto ptr = (char*)tracy_malloc( sz );
        memcpy( ptr, image, sz );

        profiler.m_fiLock.lock();
        auto fi = profiler.m_fiQueue.prepare_next();
        fi->image = ptr;
        fi->frame = profiler.m_frameCount.load( std::memory_order_relaxed ) - offset;
        fi->w = w;
        fi->h = h;
        fi->flip = flip;
        profiler.m_fiQueue.commit_next();
        profiler.m_fiLock.unlock();
    }

    static tracy_force_inline void PlotData( const char* name, int64_t val )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( QueueType::PlotData );
        MemWrite( &item->plotData.name, (uint64_t)name );
        MemWrite( &item->plotData.time, GetTime() );
        MemWrite( &item->plotData.type, PlotDataType::Int );
        MemWrite( &item->plotData.data.i, val );
        TracyLfqCommit;
    }

    static tracy_force_inline void PlotData( const char* name, float val )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( QueueType::PlotData );
        MemWrite( &item->plotData.name, (uint64_t)name );
        MemWrite( &item->plotData.time, GetTime() );
        MemWrite( &item->plotData.type, PlotDataType::Float );
        MemWrite( &item->plotData.data.f, val );
        TracyLfqCommit;
    }

    static tracy_force_inline void PlotData( const char* name, double val )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( QueueType::PlotData );
        MemWrite( &item->plotData.name, (uint64_t)name );
        MemWrite( &item->plotData.time, GetTime() );
        MemWrite( &item->plotData.type, PlotDataType::Double );
        MemWrite( &item->plotData.data.d, val );
        TracyLfqCommit;
    }

    static tracy_force_inline void ConfigurePlot( const char* name, PlotFormatType type )
    {
        TracyLfqPrepare( QueueType::PlotConfig );
        MemWrite( &item->plotConfig.name, (uint64_t)name );
        MemWrite( &item->plotConfig.type, (uint8_t)type );

#ifdef TRACY_ON_DEMAND
        GetProfiler().DeferItem( *item );
#endif

        TracyLfqCommit;
    }

    static tracy_force_inline void Message( const char* txt, size_t size, int callstack )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( callstack == 0 ? QueueType::Message : QueueType::MessageCallstack );
        auto ptr = (char*)tracy_malloc( size+1 );
        memcpy( ptr, txt, size );
        ptr[size] = '\0';
        MemWrite( &item->message.time, GetTime() );
        MemWrite( &item->message.text, (uint64_t)ptr );
        TracyLfqCommit;

        if( callstack != 0 ) GetProfiler().SendCallstack( callstack );
    }

    static tracy_force_inline void Message( const char* txt, int callstack )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( callstack == 0 ? QueueType::MessageLiteral : QueueType::MessageLiteralCallstack );
        MemWrite( &item->message.time, GetTime() );
        MemWrite( &item->message.text, (uint64_t)txt );
        TracyLfqCommit;

        if( callstack != 0 ) GetProfiler().SendCallstack( callstack );
    }

    static tracy_force_inline void MessageColor( const char* txt, size_t size, uint32_t color, int callstack )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( callstack == 0 ? QueueType::MessageColor : QueueType::MessageColorCallstack );
        auto ptr = (char*)tracy_malloc( size+1 );
        memcpy( ptr, txt, size );
        ptr[size] = '\0';
        MemWrite( &item->messageColor.time, GetTime() );
        MemWrite( &item->messageColor.text, (uint64_t)ptr );
        MemWrite( &item->messageColor.r, uint8_t( ( color       ) & 0xFF ) );
        MemWrite( &item->messageColor.g, uint8_t( ( color >> 8  ) & 0xFF ) );
        MemWrite( &item->messageColor.b, uint8_t( ( color >> 16 ) & 0xFF ) );
        TracyLfqCommit;

        if( callstack != 0 ) GetProfiler().SendCallstack( callstack );
    }

    static tracy_force_inline void MessageColor( const char* txt, uint32_t color, int callstack )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        TracyLfqPrepare( callstack == 0 ? QueueType::MessageLiteralColor : QueueType::MessageLiteralColorCallstack );
        MemWrite( &item->messageColor.time, GetTime() );
        MemWrite( &item->messageColor.text, (uint64_t)txt );
        MemWrite( &item->messageColor.r, uint8_t( ( color       ) & 0xFF ) );
        MemWrite( &item->messageColor.g, uint8_t( ( color >> 8  ) & 0xFF ) );
        MemWrite( &item->messageColor.b, uint8_t( ( color >> 16 ) & 0xFF ) );
        TracyLfqCommit;

        if( callstack != 0 ) GetProfiler().SendCallstack( callstack );
    }

    static tracy_force_inline void MessageAppInfo( const char* txt, size_t size )
    {
        InitRPMallocThread();
        auto ptr = (char*)tracy_malloc( size+1 );
        memcpy( ptr, txt, size );
        ptr[size] = '\0';
        TracyLfqPrepare( QueueType::MessageAppInfo );
        MemWrite( &item->message.time, GetTime() );
        MemWrite( &item->message.text, (uint64_t)ptr );

#ifdef TRACY_ON_DEMAND
        GetProfiler().DeferItem( *item );
#endif

        TracyLfqCommit;
    }

    static tracy_force_inline void MemAlloc( const void* ptr, size_t size )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        const auto thread = GetThreadHandle();

        GetProfiler().m_serialLock.lock();
        SendMemAlloc( QueueType::MemAlloc, thread, ptr, size );
        GetProfiler().m_serialLock.unlock();
    }

    static tracy_force_inline void MemFree( const void* ptr )
    {
#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) return;
#endif
        const auto thread = GetThreadHandle();

        GetProfiler().m_serialLock.lock();
        SendMemFree( QueueType::MemFree, thread, ptr );
        GetProfiler().m_serialLock.unlock();
    }

    static tracy_force_inline void MemAllocCallstack( const void* ptr, size_t size, int depth )
    {
#ifdef TRACY_HAS_CALLSTACK
        auto& profiler = GetProfiler();
#  ifdef TRACY_ON_DEMAND
        if( !profiler.IsConnected() ) return;
#  endif
        const auto thread = GetThreadHandle();

        InitRPMallocThread();
        auto callstack = Callstack( depth );

        profiler.m_serialLock.lock();
        SendMemAlloc( QueueType::MemAllocCallstack, thread, ptr, size );
        SendCallstackMemory( callstack );
        profiler.m_serialLock.unlock();
#else
        MemAlloc( ptr, size );
#endif
    }

    static tracy_force_inline void MemFreeCallstack( const void* ptr, int depth )
    {
#ifdef TRACY_HAS_CALLSTACK
        auto& profiler = GetProfiler();
#  ifdef TRACY_ON_DEMAND
        if( !profiler.IsConnected() ) return;
#  endif
        const auto thread = GetThreadHandle();

        InitRPMallocThread();
        auto callstack = Callstack( depth );

        profiler.m_serialLock.lock();
        SendMemFree( QueueType::MemFreeCallstack, thread, ptr );
        SendCallstackMemory( callstack );
        profiler.m_serialLock.unlock();
#else
        MemFree( ptr );
#endif
    }

    static tracy_force_inline void SendCallstack( int depth )
    {
#ifdef TRACY_HAS_CALLSTACK
        auto ptr = Callstack( depth );
        TracyLfqPrepare( QueueType::Callstack );
        MemWrite( &item->callstack.ptr, (uint64_t)ptr );
        TracyLfqCommit;
#endif
    }

    static tracy_force_inline void ParameterRegister( ParameterCallback cb ) { GetProfiler().m_paramCallback = cb; }
    static tracy_force_inline void ParameterSetup( uint32_t idx, const char* name, bool isBool, int32_t val )
    {
        TracyLfqPrepare( QueueType::ParamSetup );
        tracy::MemWrite( &item->paramSetup.idx, idx );
        tracy::MemWrite( &item->paramSetup.name, (uint64_t)name );
        tracy::MemWrite( &item->paramSetup.isBool, (uint8_t)isBool );
        tracy::MemWrite( &item->paramSetup.val, val );

#ifdef TRACY_ON_DEMAND
        GetProfiler().DeferItem( *item );
#endif

        TracyLfqCommit;
    }

    void SendCallstack( int depth, const char* skipBefore );
    static void CutCallstack( void* callstack, const char* skipBefore );

    static bool ShouldExit();

#ifdef TRACY_ON_DEMAND
    tracy_force_inline bool IsConnected() const
    {
        return m_isConnected.load( std::memory_order_acquire );
    }

    tracy_force_inline uint64_t ConnectionId() const
    {
        return m_connectionId.load( std::memory_order_acquire );
    }

    tracy_force_inline void DeferItem( const QueueItem& item )
    {
        m_deferredLock.lock();
        auto dst = m_deferredQueue.push_next();
        memcpy( dst, &item, sizeof( item ) );
        m_deferredLock.unlock();
    }
#endif

    void RequestShutdown() { m_shutdown.store( true, std::memory_order_relaxed ); m_shutdownManual.store( true, std::memory_order_relaxed ); }
    bool HasShutdownFinished() const { return m_shutdownFinished.load( std::memory_order_relaxed ); }

    void SendString( uint64_t ptr, const char* str, QueueType type );


    // Allocated source location data layout:
    //  4b  payload size
    //  4b  color
    //  4b  source line
    //  fsz function name
    //  1b  null terminator
    //  ssz source file name
    //  1b  null terminator
    //  nsz zone name (optional)

    static tracy_force_inline uint64_t AllocSourceLocation( uint32_t line, const char* source, const char* function )
    {
        const auto fsz = strlen( function );
        const auto ssz = strlen( source );
        const uint32_t sz = uint32_t( 4 + 4 + 4 + fsz + 1 + ssz + 1 );
        auto ptr = (char*)tracy_malloc( sz );
        memcpy( ptr, &sz, 4 );
        memset( ptr + 4, 0, 4 );
        memcpy( ptr + 8, &line, 4 );
        memcpy( ptr + 12, function, fsz+1 );
        memcpy( ptr + 12 + fsz + 1, source, ssz + 1 );
        return uint64_t( ptr );
    }

    static tracy_force_inline uint64_t AllocSourceLocation( uint32_t line, const char* source, const char* function, const char* name, size_t nameSz )
    {
        const auto fsz = strlen( function );
        const auto ssz = strlen( source );
        const uint32_t sz = uint32_t( 4 + 4 + 4 + fsz + 1 + ssz + 1 + nameSz );
        auto ptr = (char*)tracy_malloc( sz );
        memcpy( ptr, &sz, 4 );
        memset( ptr + 4, 0, 4 );
        memcpy( ptr + 8, &line, 4 );
        memcpy( ptr + 12, function, fsz+1 );
        memcpy( ptr + 12 + fsz + 1, source, ssz + 1 );
        memcpy( ptr + 12 + fsz + 1 + ssz + 1, name, nameSz );
        return uint64_t( ptr );
    }

private:
    enum class DequeueStatus { DataDequeued, ConnectionLost, QueueEmpty };

    static void LaunchWorker( void* ptr ) { ((Profiler*)ptr)->Worker(); }
    void Worker();

    static void LaunchCompressWorker( void* ptr ) { ((Profiler*)ptr)->CompressWorker(); }
    void CompressWorker();

    void ValidateThreadContexts_NoLock();
    void ClearQueues();
    void ClearSerial();
    void RemoveMarkedThreadContexts();
    DequeueStatus Dequeue();
    DequeueStatus DequeueContextSwitches( int64_t& timeStop );
    DequeueStatus DequeueSerial();
    bool CommitData();

    tracy_force_inline bool AppendData( const void* data, size_t len )
    {
        const auto ret = NeedDataSize( len );
        AppendDataUnsafe( data, len );
        return ret;
    }

    tracy_force_inline bool NeedDataSize( size_t len )
    {
        assert( len <= TargetFrameSize );
        bool ret = true;
        if( m_bufferOffset - m_bufferStart + len > TargetFrameSize )
        {
            ret = CommitData();
        }
        return ret;
    }

    tracy_force_inline void AppendDataUnsafe( const void* data, size_t len )
    {
        memcpy( m_buffer + m_bufferOffset, data, len );
        m_bufferOffset += int( len );
    }

    bool SendData( const char* data, size_t len );
    void SendLongString( uint64_t ptr, const char* str, size_t len, QueueType type );
    void SendSourceLocation( uint64_t ptr );
    void SendSourceLocationPayload( uint64_t ptr );
    void SendCallstackPayload( uint64_t ptr );
    void SendCallstackPayload64( uint64_t ptr );
    void SendCallstackAlloc( uint64_t ptr );
    void SendCallstackFrame( uint64_t ptr );
    void SendCodeLocation( uint64_t ptr );

    bool HandleServerQuery();
    void HandleDisconnect();
    void HandleParameter( uint64_t payload );
    void HandleSymbolQuery( uint64_t symbol );
    void HandleSymbolCodeQuery( uint64_t symbol, uint32_t size );

    void CalibrateTimer();
    void CalibrateDelay();
    void ReportTopology();

    static tracy_force_inline void ZoneVerify( uint32_t id )
    {
#ifndef TRACY_NO_VERIFY
        TracyLfqPrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, id );
        TracyLfqCommitC;
#endif
    }

    static tracy_force_inline void SendCallstackMemory( void* ptr )
    {
#ifdef TRACY_HAS_CALLSTACK
        auto item = GetProfiler().m_serialQueue.prepare_next();
        MemWrite( &item->hdr.type, QueueType::CallstackMemory );
        MemWrite( &item->callstackMemory.ptr, (uint64_t)ptr );
        GetProfiler().m_serialQueue.commit_next();
#endif
    }

    static tracy_force_inline void SendMemAlloc( QueueType type, const uint64_t thread, const void* ptr, size_t size )
    {
        assert( type == QueueType::MemAlloc || type == QueueType::MemAllocCallstack );

        auto item = GetProfiler().m_serialQueue.prepare_next();
        MemWrite( &item->hdr.type, type );
        MemWrite( &item->memAlloc.time, GetTime() );
        MemWrite( &item->memAlloc.thread, thread );
        MemWrite( &item->memAlloc.ptr, (uint64_t)ptr );
        if( compile_time_condition<sizeof( size ) == 4>::value )
        {
            memcpy( &item->memAlloc.size, &size, 4 );
            memset( &item->memAlloc.size + 4, 0, 2 );
        }
        else
        {
            assert( sizeof( size ) == 8 );
            memcpy( &item->memAlloc.size, &size, 4 );
            memcpy( ((char*)&item->memAlloc.size)+4, ((char*)&size)+4, 2 );
        }
        GetProfiler().m_serialQueue.commit_next();
    }

    static tracy_force_inline void SendMemFree( QueueType type, const uint64_t thread, const void* ptr )
    {
        assert( type == QueueType::MemFree || type == QueueType::MemFreeCallstack );

        auto item = GetProfiler().m_serialQueue.prepare_next();
        MemWrite( &item->hdr.type, type );
        MemWrite( &item->memFree.time, GetTime() );
        MemWrite( &item->memFree.thread, thread );
        MemWrite( &item->memFree.ptr, (uint64_t)ptr );
        GetProfiler().m_serialQueue.commit_next();
    }

#if ( defined _WIN32 || defined __CYGWIN__ ) && defined TRACY_TIMER_QPC
    static int64_t GetTimeQpc();
#endif

    double m_timerMul;
    uint64_t m_resolution;
    uint64_t m_delay;
    std::atomic<int64_t> m_timeBegin;
    uint64_t m_mainThread;
    uint64_t m_epoch;
    std::atomic<bool> m_shutdown;
    std::atomic<bool> m_shutdownManual;
    std::atomic<bool> m_shutdownFinished;
    Socket* m_sock;
    UdpBroadcast* m_broadcast;
    bool m_noExit;
    uint32_t m_userPort;
#ifndef TRACY_NO_VERIFY
    std::atomic<uint32_t> m_zoneId;
#endif
    std::atomic<uint32_t> m_lockCounter;
    std::atomic<uint8_t> m_gpuCtxCounter;
    int64_t m_samplingPeriod;

    bool m_active;
    
    TracyMutex m_threadsCtxsLock;
    FastVector<ThreadContext*> m_threadsCtxs;
    static thread_local ThreadContext* s_threadContext;

    int64_t m_refTimeSerial;
    int64_t m_refTimeCtx;
    int64_t m_refTimeGpu;

    void* m_stream;     // LZ4_stream_t*
    char* m_buffer;
    int m_bufferOffset;
    int m_bufferStart;

    char* m_lz4Buf;

    FastVector<QueueItem> m_serialQueue, m_serialDequeue;
    TracyMutex m_serialLock;

    FastVector<FrameImageQueueItem> m_fiQueue, m_fiDequeue;
    TracyMutex m_fiLock;

    std::atomic<uint64_t> m_frameCount;
#ifdef TRACY_ON_DEMAND
    std::atomic<bool> m_isConnected;
    std::atomic<uint64_t> m_connectionId;

    TracyMutex m_deferredLock;
    FastVector<QueueItem> m_deferredQueue;
#endif

#ifdef TRACY_HAS_SYSTIME
    void ProcessSysTime();

    SysTime m_sysTime;
    uint64_t m_sysTimeLast = 0;
#else
    void ProcessSysTime() {}
#endif

    ParameterCallback m_paramCallback;

    static Profiler* s_instance;
};


tracy_force_inline Profiler& GetProfiler()
{
    return Profiler::GetProfiler();
}

}

#endif
