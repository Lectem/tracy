#ifndef __TRACYTIMELINECONTROLLER_HPP__
#define __TRACYTIMELINECONTROLLER_HPP__

#include <assert.h>
#include <optional>
#include <string>
#include <vector>

#include "TracyImGui.hpp"
#include "../server/TracyTaskDispatch.hpp"
#include "../server/tracy_robin_hood.h"
#include "../public/common/TracyForceInline.hpp"

namespace tracy
{

class TimelineItem;
class View;
class Worker;

class TimelineController
{
public:
    enum class SortMode
    {
        Manual,
        Name,
        ActiveTime,
    };

    TimelineController( View& view, Worker& worker, bool threading );
    ~TimelineController();

    void FirstFrameExpired();
    void Begin();
    void End( double pxns, const ImVec2& wpos, bool hover, bool vcenter, float yMin, float yMax );

    template<class T, class U>
    void AddItem( U* data )
    {
        auto it = m_itemMap.find( data );
        if( it == m_itemMap.end() ) it = m_itemMap.emplace( data, std::make_unique<T>( m_view, m_worker, data ) ).first;
        m_items.emplace_back( it->second.get() );
    }

    float GetHeight() const { return m_height; }
    const unordered_flat_map<const void*, std::unique_ptr<TimelineItem>>& GetItemMap() const { return m_itemMap; }
    const std::vector<TimelineItem*>& GetCurrentOrder() const { return m_items; }

    tracy_force_inline TimelineItem& GetItem( const void* data )
    {
        auto it = m_itemMap.find( data );
        assert( it != m_itemMap.end() );
        return *it->second;
    }

    void MoveItem( const void* srcKey, const void* targetKey );
    void MarkDirty() { m_orderDirty = true; }

    struct LaneConfigPreset
    {
        std::string name;
        SortMode sortMode;
        bool groupByType;
        struct ItemState { const void* key; bool visible; bool pinned; };
        std::vector<ItemState> orderedItems;
    };
    const std::vector<LaneConfigPreset>& GetPresets() const { return m_presets; }
    size_t SavePreset( const char* name );
    void ApplyPreset( size_t idx );
    void DeletePreset( size_t idx );

    SortMode GetSortMode() const { return m_sortMode; }
    void SetSortMode( SortMode mode );

    bool GetGroupByType() const { return m_groupByType; }
    void SetGroupByType( bool v );

private:
    void UpdateCenterItem();
    std::optional<int> CalculateScrollPosition() const;
    void ApplyOrder();
    int64_t ComputeActiveTime( const TimelineItem* item ) const;
    void SyncThreadOrder();

    std::vector<TimelineItem*> m_items;
    unordered_flat_map<const void*, std::unique_ptr<TimelineItem>> m_itemMap;
    std::vector<const void*> m_userOrder;

    SortMode m_sortMode;
    bool m_groupByType;
    bool m_orderDirty;
    size_t m_lastItemCount;
    std::vector<LaneConfigPreset> m_presets;

    float m_height;
    float m_scroll;

    const void* m_centerItemkey;
    int m_centerItemOffsetY;

    bool m_firstFrame;

    View& m_view;
    Worker& m_worker;

    TaskDispatch m_td;
};

}

#endif
