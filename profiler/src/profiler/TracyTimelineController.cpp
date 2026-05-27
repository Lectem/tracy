#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "TracyTimelineItem.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineController.hpp"
#include "TracyView.hpp"

#include "../Fonts.hpp"

namespace tracy
{

TimelineController::TimelineController( View& view, Worker& worker, bool threading )
    : m_sortMode( SortMode::Manual )
    , m_groupByType( false )
    , m_orderDirty( false )
    , m_lastItemCount( 0 )
    , m_height( 0 )
    , m_scroll( 0 )
    , m_centerItemkey( nullptr )
    , m_centerItemOffsetY( 0 )
    , m_firstFrame( true )
    , m_view( view )
    , m_worker( worker )
#ifdef __EMSCRIPTEN__
    , m_td( threading ? 2 : 0, "Render" )
#else
    , m_td( threading ? (size_t)std::max( 0, ( (int)std::thread::hardware_concurrency() - 2 ) / 2 ) : 0, "Render" )
#endif
{
}

TimelineController::~TimelineController()
{
}

void TimelineController::FirstFrameExpired()
{
    m_firstFrame = false;
}

void TimelineController::Begin()
{
    m_items.clear();
}

void TimelineController::UpdateCenterItem()
{
    ImVec2 mousePos = ImGui::GetMousePos();

    m_centerItemkey = nullptr;
    m_centerItemOffsetY = 0;

    if( m_firstFrame || !ImGui::IsMousePosValid( &mousePos ) ) return;

    const auto timelineMousePosY = mousePos.y - ImGui::GetWindowPos().y;
    int centerY = timelineMousePosY + ImGui::GetScrollY();

    int yBegin = 0;
    int yEnd = 0;
    for( auto& item : m_items )
    {
        m_centerItemkey = item->GetKey();
        yBegin = yEnd;
        yEnd += item->GetHeight();

        const auto inLowerBounds = m_centerItemkey == m_items.front()->GetKey() || yBegin <= centerY;
        const auto inUpperBounds = m_centerItemkey == m_items.back()->GetKey() || centerY < yEnd;

        if( inLowerBounds && inUpperBounds )
        {
            m_centerItemOffsetY = centerY - yBegin;
            break;
        }
    }
}

std::optional<int> TimelineController::CalculateScrollPosition() const
{
    if( !m_centerItemkey ) return std::nullopt;

    ImVec2 mousePos = ImGui::GetMousePos();

    if( !ImGui::IsMousePosValid( &mousePos ) ) return std::nullopt;

    const auto timelineMousePosY = mousePos.y - ImGui::GetWindowPos().y;

    int yBegin = 0;
    int yEnd = 0;
    for( auto& item : m_items )
    {
        yBegin = yEnd;
        yEnd += item->GetHeight();

        if( item->GetKey() != m_centerItemkey ) continue;

        int scrollY = yBegin + m_centerItemOffsetY - timelineMousePosY;

        return scrollY;
    }

    return std::nullopt;
}

void TimelineController::SetSortMode( SortMode mode )
{
    if( m_sortMode == mode ) return;
    m_sortMode = mode;
    m_orderDirty = true;
}

void TimelineController::SetGroupByType( bool v )
{
    if( m_groupByType == v ) return;
    m_groupByType = v;
    m_orderDirty = true;
}

int64_t TimelineController::ComputeActiveTime( const TimelineItem* item ) const
{
    switch( item->GetType() )
    {
    case TimelineItemGroup::Thread:
    {
        const auto* td = static_cast<const ThreadData*>( item->GetKey() );
        const auto ctx = m_worker.GetContextSwitchData( td->id );
        if( ctx ) return ctx->runningTime;
        return 0;
    }
    case TimelineItemGroup::Gpu:
    {
        const auto* gd = static_cast<const GpuCtxData*>( item->GetKey() );
        return (int64_t)gd->count;
    }
    case TimelineItemGroup::Cpu:
        return 0;
    case TimelineItemGroup::Plot:
    {
        const auto* pd = static_cast<const PlotData*>( item->GetKey() );
        return (int64_t)pd->data.size();
    }
    default:
        return 0;
    }
}

void TimelineController::SyncThreadOrder()
{
    auto& threadOrder = m_view.GetThreadOrderMutable();
    threadOrder.clear();
    for( const auto* item : m_items )
    {
        if( item->GetType() == TimelineItemGroup::Thread )
        {
            threadOrder.push_back( static_cast<const ThreadData*>( item->GetKey() ) );
        }
    }
}

void TimelineController::ApplyOrder()
{
    if( m_items.empty() ) return;

    if( m_items.size() != m_lastItemCount )
    {
        m_lastItemCount = m_items.size();
        m_orderDirty = true;
    }

    if( !m_orderDirty ) return;
    m_orderDirty = false;

    if( m_sortMode == SortMode::Name )
    {
        std::stable_sort( m_items.begin(), m_items.end(),
            []( const TimelineItem* a, const TimelineItem* b ) {
                return strcmp( a->HeaderLabel(), b->HeaderLabel() ) < 0;
            } );
    }
    else if( m_sortMode == SortMode::ActiveTime )
    {
        std::stable_sort( m_items.begin(), m_items.end(),
            [this]( const TimelineItem* a, const TimelineItem* b ) {
                return ComputeActiveTime( a ) > ComputeActiveTime( b );
            } );
    }
    else if( m_sortMode == SortMode::Manual && !m_userOrder.empty() )
    {
        unordered_flat_map<const void*, size_t> orderIndex;
        orderIndex.reserve( m_userOrder.size() + m_items.size() );
        for( size_t i = 0; i < m_userOrder.size(); ++i )
        {
            orderIndex.emplace( m_userOrder[i], i );
        }
        size_t nextIdx = m_userOrder.size();
        for( const auto* item : m_items )
        {
            if( orderIndex.find( item->GetKey() ) == orderIndex.end() )
            {
                orderIndex.emplace( item->GetKey(), nextIdx++ );
            }
        }

        const auto& constOrderIndex = orderIndex;
        std::stable_sort( m_items.begin(), m_items.end(),
            [&constOrderIndex]( const TimelineItem* a, const TimelineItem* b ) {
                return constOrderIndex.find( a->GetKey() )->second < constOrderIndex.find( b->GetKey() )->second;
            } );
    }

    if( m_groupByType )
    {
        std::stable_sort( m_items.begin(), m_items.end(),
            []( const TimelineItem* a, const TimelineItem* b ) {
                return (int)a->GetType() < (int)b->GetType();
            } );
    }

    std::stable_sort( m_items.begin(), m_items.end(),
        []( const TimelineItem* a, const TimelineItem* b ) {
            return a->IsPinned() && !b->IsPinned();
        } );

    SyncThreadOrder();
}

void TimelineController::MoveItem( const void* srcKey, const void* targetKey )
{
    if( srcKey == targetKey ) return;
    if( m_itemMap.find( srcKey ) == m_itemMap.end() ) return;

    if( m_userOrder.empty() )
    {
        for( const auto* it : m_items ) m_userOrder.push_back( it->GetKey() );
    }

    auto srcPos = std::find( m_userOrder.begin(), m_userOrder.end(), srcKey );
    if( srcPos == m_userOrder.end() ) return;
    m_userOrder.erase( srcPos );

    if( targetKey )
    {
        auto tgtPos = std::find( m_userOrder.begin(), m_userOrder.end(), targetKey );
        m_userOrder.insert( tgtPos, srcKey );
    }
    else
    {
        m_userOrder.push_back( srcKey );
    }

    m_sortMode = SortMode::Manual;
    m_orderDirty = true;
}

void TimelineController::End( double pxns, const ImVec2& wpos, bool hover, bool vcenter, float yMin, float yMax )
{
    auto shouldUpdateCenterItem = [&] () {
        const auto imguiChangedScroll = m_scroll != ImGui::GetScrollY();
        const auto& mouseDelta = ImGui::GetIO().MouseDelta;
        const auto mouseMoved = mouseDelta.x != 0.0f || mouseDelta.y != 0.0f;
        const auto& mousePos = ImGui::GetIO().MousePos;
        const auto mouseVisible = ImGui::IsMousePosValid( &mousePos );
        return ( ( imguiChangedScroll || mouseMoved || !mouseVisible ) && !ImGui::IsMouseDown( 1 ) ) || !m_centerItemkey;
    };

    ApplyOrder();

    if( !vcenter )
    {
        m_centerItemkey = nullptr;
        m_centerItemOffsetY = 0;
    }
    else if( shouldUpdateCenterItem() )
    {
        UpdateCenterItem();
    }

    const auto& viewData = m_view.GetViewData();

    TimelineContext ctx;
    ctx.w = ImGui::GetContentRegionAvail().x - 1;
    ctx.ty = ImGui::GetTextLineHeight();
    ImGui::PushFont( g_fonts.normal, FontSmall );
    ctx.sty = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ctx.scale = GetScale();
    ctx.yMin = yMin;
    ctx.yMax = yMax;
    ctx.pxns = pxns;
    ctx.nspx = 1.0 / pxns;
    ctx.vStart = viewData.zvStart;
    ctx.vEnd = viewData.zvEnd;
    ctx.wpos = wpos;
    ctx.hover = hover;

    int yOffset = 0;
    for( auto& item : m_items )
    {
        if( item->WantPreprocess() && item->IsVisible() )
        {
            const auto yPos = wpos.y + yOffset;
            const bool visible = m_firstFrame || ( yPos < yMax && yPos + item->GetHeight() >= yMin );
            item->Preprocess( ctx, m_td, visible, yPos );
        }
        yOffset += m_firstFrame ? 0 : item->GetHeight();
    }
    m_td.Sync();

    yOffset = 0;
    const void* dropSrc = nullptr;
    const void* dropTarget = nullptr;
    const auto* activePayload = ImGui::GetDragDropPayload();
    const bool dndActive = activePayload && activePayload->IsDataType( "TimelineItemOrder" );

    float bestDropY = wpos.y;
    const void* bestDropTarget = m_items.empty() ? nullptr : m_items.front()->GetKey();
    float bestDropDist = std::numeric_limits<float>::max();

    if( dndActive && !m_firstFrame && !m_items.empty() )
    {
        const float mouseY = ImGui::GetMousePos().y;
        const float d = std::abs( wpos.y - mouseY );
        bestDropY = wpos.y;
        bestDropTarget = m_items.front()->GetKey();
        bestDropDist = d;
    }

    for( size_t i = 0; i < m_items.size(); ++i )
    {
        auto& item = m_items[i];
        auto currentFrameItemHeight = item->GetHeight();
        item->Draw( m_firstFrame, ctx, yOffset );
        if( m_firstFrame ) currentFrameItemHeight = item->GetHeight();
        yOffset += currentFrameItemHeight;

        if( dndActive && !m_firstFrame )
        {
            const float gapY = wpos.y + yOffset;
            const float mouseY = ImGui::GetMousePos().y;
            const float d = std::abs( gapY - mouseY );
            if( d < bestDropDist )
            {
                bestDropDist = d;
                bestDropY = gapY;
                bestDropTarget = ( i + 1 < m_items.size() ) ? m_items[i + 1]->GetKey() : nullptr;
            }
        }
    }

    if( dndActive && !m_firstFrame && !m_items.empty() )
    {
        ImGui::SetCursorScreenPos( wpos );
        ImGui::InvisibleButton( "##timelineDropZone", ImVec2( ctx.w, std::max( 1.f, (float)yOffset ) ) );
        if( ImGui::BeginDragDropTarget() )
        {
            if( auto payload = ImGui::AcceptDragDropPayload( "TimelineItemOrder", ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) )
            {
                dropSrc = *(const void**)payload->Data;
                dropTarget = bestDropTarget;
            }
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2( wpos.x, bestDropY ),
                ImVec2( wpos.x + ctx.w, bestDropY ),
                ImGui::GetColorU32( ImGuiCol_DragDropTarget ), 2.f );
            ImGui::EndDragDropTarget();
        }
    }

    if( dropSrc )
    {
        MoveItem( dropSrc, dropTarget );
    }

    if( const auto scrollY = CalculateScrollPosition() )
    {
        int clampedScrollY = std::min<int>( *scrollY, std::max<int>( yOffset - ImGui::GetWindowHeight(), 0 ) );
        ImGui::SetScrollY( clampedScrollY );
        int minHeight = ImGui::GetWindowHeight() + clampedScrollY;
        yOffset = std::max( yOffset, minHeight );
    }

    const auto scrollPos = ImGui::GetScrollY();
    if( ( scrollPos == 0 && m_scroll != 0 ) || yOffset > m_height )
    {
        m_height = yOffset;
    }
    m_scroll = scrollPos;
}

}
