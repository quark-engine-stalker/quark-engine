#pragma once

// Budget for speculative resource preparation, expressed in source bytes.
// It is not a process working-set or driver-allocation measurement.
// The owner serializes access and applies this only to optional warm-up work.
template <class Key, class Set>
class resource_warm_budget
{
public:
    struct cost { Key key; u64 bytes; };

    void reset(u64 limit)
    {
        resources_.clear();
        used_ = 0;
        limit_ = limit;
    }

    bool admit(const cost* costs, size_t count)
    {
        Set additional_resources;
        u64 additional = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (resources_.find(costs[i].key) != resources_.end())
                continue;
            if (!additional_resources.insert(costs[i].key).second)
                continue;
            if (costs[i].bytes > limit_ - used_ - additional)
                return false;
            additional += costs[i].bytes;
        }
        try
        {
            for (const Key& key : additional_resources)
                resources_.insert(key);
        }
        catch (...)
        {
            for (const Key& key : additional_resources)
                resources_.erase(key);
            throw;
        }
        used_ += additional;
        return true;
    }

    void release_storage()
    {
        Set().swap(resources_);
        used_ = limit_ = 0;
    }

    u64 used() const { return used_; }
    u64 limit() const { return limit_; }

private:
    Set resources_;
    u64 used_ = 0;
    u64 limit_ = 0;
};
