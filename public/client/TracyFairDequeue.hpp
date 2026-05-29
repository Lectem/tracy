#ifndef __TRACYFAIRDEQUEUE_HPP__
#define __TRACYFAIRDEQUEUE_HPP__

// Experimental watermark-based comm-thread scheduling.
// Define TRACY_FAIR_DEQUEUE when building TracyClient to enable.
//
// Phase 1: drain producers whose queue head is older than the global watermark.
// Phase 2: if none, drain any producer that still has work (forward progress).

#ifdef TRACY_FAIR_DEQUEUE

#include <stdint.h>
#include <limits>
#include "tracy_concurrentqueue.h"
#include "../common/TracyQueue.hpp"

namespace tracy
{

struct QueueItemHeadTime
{
    bool hasTime;
    int64_t time;

    QueueItemHeadTime()
        : hasTime( false )
        , time( 0 )
    {
    }

    QueueItemHeadTime( bool ht, int64_t t )
        : hasTime( ht )
        , time( t )
    {
    }
};

QueueItemHeadTime GetQueueItemHeadTime( const QueueItem& item );

class FairDequeueState
{
public:
    using producer_t = moodycamel::ConcurrentQueue<QueueItem>::ExplicitProducer;

    void Reset()
    {
        m_watermark = 0;
        m_rotate = nullptr;
    }

    int64_t Watermark() const { return m_watermark; }

    producer_t* SelectProducer( moodycamel::ConcurrentQueue<QueueItem>& queue );
    void NoteBatchShipped( const QueueItem* items, size_t count );
    void SetRotate( producer_t* prod, producer_t* tail ) { m_rotate = NextProducer( prod, tail ); }

private:
    static producer_t* NextProducer( producer_t* prod, producer_t* tail );
    static bool ProducerOnRing( producer_t* prod, producer_t* tail );
    static bool ProducerIsRetired( producer_t* prod );
    producer_t* ResolveStart( producer_t* tail );
    bool ProducerNeedsCatchUp( producer_t* prod ) const;
    bool ProducerHasWork( producer_t* prod ) const;

    int64_t m_watermark = 0;
    producer_t* m_rotate = nullptr;
};

}

#endif

#endif
