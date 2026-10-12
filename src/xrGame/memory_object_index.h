#pragma once

#include <algorithm>

// An ID-to-slot index for an authoritative memory vector. It contains every ID,
// so an absent object needs no repeated scan. Store slots, never object pointers:
// vector reallocations and replacement by another object with the same ID must
// still return the current entry. Like the memory vector, this is owner-thread
// data. Erases/reorders invalidate it; appends/replacements can update it.
class CMemoryObjectIndex
{
    struct SEntry
    {
        u32 first;
        u32 count;
    };

    xr_unordered_flat_map<u16, SEntry> m_entries;
    const void* m_source = nullptr;
    u32 m_source_size = 0;
    bool m_valid = false;
    u32 m_cold_queries = 0;

    void insert(u16 id, u32 slot)
    {
        auto entry = m_entries.emplace(id, SEntry{slot, 1});
        if (!entry.second)
        {
            ++entry.first->second.count;
            if (slot < entry.first->second.first)
                entry.first->second.first = slot;
        }
    }

public:
    void invalidate()
    {
        m_valid = false;
        m_cold_queries = 0;
    }

    void reset()
    {
        m_entries.clear();
        m_source = nullptr;
        m_source_size = 0;
        m_valid = false;
        m_cold_queries = 0;
    }

    template <typename Vector, typename GetId>
    bool try_find(const Vector& objects, u16 id, GetId get_id, u32& result)
    {
        if (m_source != &objects || m_source_size != objects.size())
        {
            invalidate();
            m_source = &objects;
            m_source_size = static_cast<u32>(objects.size());
        }
        if (!m_valid)
        {
            // Do not pay for a complete index on a one-off lookup after spawn
            // or cleanup. These first queries still read the live source; no
            // negative result can outlive an append or replacement.
            if (m_cold_queries < 32)
            {
                ++m_cold_queries;
                return false;
            }
            m_entries.clear();
            m_entries.reserve(objects.size());
            m_source = &objects;
            m_source_size = static_cast<u32>(objects.size());
            for (u32 slot = 0; slot < m_source_size; ++slot)
                insert(get_id(objects[slot]), slot);
            m_valid = true;
        }

        const auto entry = m_entries.find(id);
        result = entry != m_entries.end() ? entry->second.first : u32(-1);
        return true;
    }

    bool ready() const { return m_valid; }

    template <typename Vector, typename GetId>
    u32 find(const Vector& objects, u16 id, GetId get_id)
    {
        u32 result;
        if (try_find(objects, id, get_id, result))
            return result;
        const auto found = std::find(objects.begin(), objects.end(), id);
        return found != objects.end() ? static_cast<u32>(found - objects.begin()) : u32(-1);
    }

    template <typename Vector, typename GetId>
    void appended(const Vector& objects, u32 slot, GetId get_id)
    {
        if (!m_valid)
            return;
        if (m_source != &objects || slot != m_source_size || objects.size() != m_source_size + 1)
        {
            invalidate();
            return;
        }

        insert(get_id(objects[slot]), slot);
        ++m_source_size;
    }

    template <typename Vector, typename GetId>
    void replaced(const Vector& objects, u32 slot, u16 old_id, GetId get_id)
    {
        if (!m_valid)
            return;
        if (m_source != &objects || m_source_size != objects.size() || slot >= objects.size())
        {
            invalidate();
            return;
        }

        const u16 new_id = get_id(objects[slot]);
        if (new_id == old_id)
            return;

        auto old_entry = m_entries.find(old_id);
        if (old_entry == m_entries.end())
        {
            invalidate();
            return;
        }
        if (!--old_entry->second.count)
            m_entries.erase(old_id);
        else if (old_entry->second.first == slot)
        {
            // Duplicate IDs are not normally inserted by the memory manager.
            // Still preserve std::find's first-match behaviour if they occur.
            u32 first = 0;
            while (first < m_source_size && get_id(objects[first]) != old_id)
                ++first;
            if (first == m_source_size)
            {
                invalidate();
                return;
            }
            old_entry->second.first = first;
        }
        insert(new_id, slot);
    }
};
