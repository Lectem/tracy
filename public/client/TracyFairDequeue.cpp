#ifdef TRACY_ENABLE
#ifdef TRACY_FAIR_DEQUEUE

#include "TracyFairDequeue.hpp"
#include "../common/TracyAlign.hpp"

namespace tracy
{

QueueItemHeadTime GetQueueItemHeadTime( const QueueItem& item )
{
    const auto idx = MemRead<uint8_t>( &item.hdr.idx );
    switch( (QueueType)idx )
    {
    case QueueType::ZoneBegin:
    case QueueType::ZoneBeginCallstack:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.zoneBegin.time ) );
    case QueueType::ZoneEnd:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.zoneEnd.time ) );
    case QueueType::ZoneBeginAllocSrcLoc:
    case QueueType::ZoneBeginAllocSrcLocCallstack:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.zoneBeginLean.time ) );
    case QueueType::PlotDataInt:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.plotDataInt.time ) );
    case QueueType::PlotDataFloat:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.plotDataFloat.time ) );
    case QueueType::PlotDataDouble:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.plotDataDouble.time ) );
    case QueueType::ContextSwitch:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.contextSwitch.time ) );
    case QueueType::ThreadWakeup:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.threadWakeup.time ) );
    case QueueType::GpuTime:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.gpuTime.gpuTime ) );
    case QueueType::GpuZoneBegin:
    case QueueType::GpuZoneBeginCallstack:
    case QueueType::GpuZoneBeginAllocSrcLoc:
    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.gpuZoneBegin.cpuTime ) );
    case QueueType::GpuZoneEnd:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.gpuZoneEnd.cpuTime ) );
    case QueueType::MemAlloc:
    case QueueType::MemAllocNamed:
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.memAlloc.time ) );
    case QueueType::MemFree:
    case QueueType::MemFreeNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.memFree.time ) );
    case QueueType::LockWait:
    case QueueType::LockSharedWait:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.lockWait.time ) );
    case QueueType::LockObtain:
    case QueueType::LockSharedObtain:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.lockObtain.time ) );
    case QueueType::LockRelease:
    case QueueType::LockSharedRelease:
        return QueueItemHeadTime( true, MemRead<int64_t>( &item.lockRelease.time ) );
    default:
        return {};
    }
}

bool FairDequeueState::ProducerHasWork( producer_t* prod ) const
{
    return prod->size_approx() > 0;
}

FairDequeueState::producer_t* FairDequeueState::NextProducer( producer_t* prod, producer_t* tail )
{
    if( prod == nullptr )
    {
        return tail;
    }
    auto* next = prod->next_prod();
    return next != nullptr ? static_cast<producer_t*>( next ) : tail;
}

bool FairDequeueState::ProducerOnRing( producer_t* prod, producer_t* tail )
{
    if( prod == nullptr || tail == nullptr )
    {
        return false;
    }
    auto* p = tail;
    do
    {
        if( p == prod )
        {
            return true;
        }
        p = NextProducer( p, tail );
    }
    while( p != tail );
    return false;
}

bool FairDequeueState::ProducerIsRetired( producer_t* prod )
{
    return prod->inactive.load( std::memory_order_relaxed ) && prod->size_approx() == 0;
}

FairDequeueState::producer_t* FairDequeueState::ResolveStart( producer_t* tail )
{
    if( m_rotate == nullptr )
    {
        return tail;
    }
    if( !ProducerOnRing( m_rotate, tail ) || ProducerIsRetired( m_rotate ) )
    {
        m_rotate = nullptr;
        return tail;
    }
    return m_rotate;
}

bool FairDequeueState::ProducerNeedsCatchUp( producer_t* prod ) const
{
    if( !ProducerHasWork( prod ) )
    {
        return false;
    }

    QueueItem item;
    if( !prod->try_peek_front( item ) )
    {
        return false;
    }

    const auto ht = GetQueueItemHeadTime( item );
    if( !ht.hasTime )
    {
        return true;
    }

    return ht.time < m_watermark;
}

FairDequeueState::producer_t* FairDequeueState::SelectProducer( moodycamel::ConcurrentQueue<QueueItem>& queue )
{
    producer_t* const tail = queue.producer_tail();
    if( tail == nullptr )
    {
        return nullptr;
    }

    producer_t* const start = ResolveStart( tail );
    producer_t* p = start;

    // Phase 1: walk the ring, skip caught-up producers, stop at first that needs catch-up.
    do
    {
        if( ProducerNeedsCatchUp( p ) )
        {
            return p;
        }
        p = NextProducer( p, tail );
    }
    while( p != start );

    // Phase 2: no head < watermark — drain any producer that still has data.
    p = NextProducer( start, tail );
    const producer_t* const phase2Start = p;
    do
    {
        if( ProducerHasWork( p ) )
        {
            return p;
        }
        p = NextProducer( p, tail );
    }
    while( p != phase2Start );

    return nullptr;
}

void FairDequeueState::NoteBatchShipped( const QueueItem* items, size_t count )
{
    while( count-- > 0 )
    {
        const auto ht = GetQueueItemHeadTime( *items );
        if( ht.hasTime && ht.time > m_watermark )
        {
            m_watermark = ht.time;
        }
        ++items;
    }
}

}

#endif
#endif
