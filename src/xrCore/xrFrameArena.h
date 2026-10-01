#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

// Small monotonic allocator intended for data whose lifetime never exceeds one
// engine frame.  Blocks are retained and reused between frames, while individual
// deallocations are intentionally ignored.  Callers must discard all containers
// backed by the arena before reset()/reset_and_trim(). The arena is intentionally
// lock-free; ownership or external synchronization is required.
class xr_frame_arena
{
private:
    struct block
    {
        block* next;
        size_t capacity;
        size_t used;
    };

    block* m_head;
    block* m_tail;
    block* m_current;
    size_t m_initial_block_size;
    size_t m_total_capacity;

    static size_t align_up(size_t value, size_t alignment)
    {
        return (value + alignment - 1u) & ~(alignment - 1u);
    }

    block* create_block(size_t minimum_capacity)
    {
        const size_t previous_capacity = m_current ? m_current->capacity : 0u;
        size_t capacity = previous_capacity ? previous_capacity * 2u : m_initial_block_size;
        if (capacity < minimum_capacity)
            capacity = minimum_capacity;

        const size_t allocation_size = sizeof(block) + capacity;
        block* result = static_cast<block*>(xr_malloc_uninitialized(allocation_size));
        VERIFY(result);

        result->next = nullptr;
        result->capacity = capacity;
        result->used = 0u;

        if (!m_head)
        {
            m_head = m_tail = result;
        }
        else
        {
            m_tail->next = result;
            m_tail = result;
        }

        m_current = result;
        m_total_capacity += capacity;
        return result;
    }

public:
    explicit xr_frame_arena(size_t initial_block_size = 64u * 1024u)
        : m_head(nullptr), m_tail(nullptr), m_current(nullptr),
          m_initial_block_size(initial_block_size ? initial_block_size : 1024u),
          m_total_capacity(0u)
    {
    }

    xr_frame_arena(const xr_frame_arena&) = delete;
    xr_frame_arena& operator=(const xr_frame_arena&) = delete;

    ~xr_frame_arena()
    {
        release();
    }

    void* allocate_bytes(size_t bytes, size_t alignment)
    {
        if (!bytes)
            return nullptr;

        if (alignment < alignof(void*))
            alignment = alignof(void*);
        VERIFY((alignment & (alignment - 1u)) == 0u);

        const size_t required_capacity = bytes + alignment - 1u;
        block* current = m_current ? m_current : m_head;

        while (current)
        {
            const size_t data_begin = reinterpret_cast<size_t>(current + 1);
            const size_t aligned_address = align_up(data_begin + current->used, alignment);
            const size_t new_used = aligned_address + bytes - data_begin;
            if (new_used <= current->capacity)
            {
                current->used = new_used;
                m_current = current;
                return reinterpret_cast<void*>(aligned_address);
            }
            current = current->next;
        }

        current = create_block(required_capacity);
        const size_t data_begin = reinterpret_cast<size_t>(current + 1);
        const size_t aligned_address = align_up(data_begin, alignment);
        current->used = aligned_address + bytes - data_begin;
        return reinterpret_cast<void*>(aligned_address);
    }

    void reset()
    {
        for (block* current = m_head; current; current = current->next)
            current->used = 0u;
        m_current = m_head;
    }

    // Keeps complete blocks up to retain_capacity and releases only the tail.
    // The first block is retained unless retain_capacity is zero.
    void reset_and_trim(size_t retain_capacity)
    {
        reset();
        if (!m_head)
            return;

        if (!retain_capacity)
        {
            release();
            return;
        }

        // A pathological first allocation can make the first block larger than
        // the entire retention budget. Keeping that block forever defeats the
        // high-water cap, so drop the arena and rebuild lazily next frame.
        if (m_head->capacity > retain_capacity)
        {
            release();
            return;
        }

        size_t retained = 0u;
        block* current = m_head;
        block* previous = nullptr;
        while (current)
        {
            if (previous && retained + current->capacity > retain_capacity)
                break;
            retained += current->capacity;
            previous = current;
            current = current->next;
        }

        if (!current)
        {
            m_current = m_head;
            return;
        }

        VERIFY(previous);
        previous->next = nullptr;
        m_tail = previous;
        while (current)
        {
            block* next = current->next;
            void* memory = current;
            xr_free(memory);
            current = next;
        }

        m_total_capacity = retained;
        m_current = m_head;
    }

    void release()
    {
        block* current = m_head;
        while (current)
        {
            block* next = current->next;
            void* memory = current;
            xr_free(memory);
            current = next;
        }

        m_head = nullptr;
        m_tail = nullptr;
        m_current = nullptr;
        m_total_capacity = 0u;
    }

    size_t capacity() const { return m_total_capacity; }
};

template <typename T>
class xr_frame_allocator
{
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    template <typename U>
    struct rebind
    {
        using other = xr_frame_allocator<U>;
    };

    xr_frame_arena* arena;

    xr_frame_allocator() noexcept : arena(nullptr) {}
    explicit xr_frame_allocator(xr_frame_arena* value) noexcept : arena(value) {}

    template <typename U>
    xr_frame_allocator(const xr_frame_allocator<U>& other) noexcept : arena(other.arena) {}

    pointer allocate(size_type count)
    {
        if (count > max_size())
            throw std::bad_alloc();
        if (!count)
            return nullptr;
        if (!arena)
            return static_cast<pointer>(xr_malloc_uninitialized(count * sizeof(T)));
        return static_cast<pointer>(arena->allocate_bytes(count * sizeof(T), alignof(T)));
    }

    void deallocate(pointer value, size_type) noexcept
    {
        if (!arena)
            xr_free(value);
    }

    template <typename U, typename... Args>
    void construct(U* value, Args&&... args)
    {
        ::new (static_cast<void*>(value)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* value)
    {
        value->~U();
    }

    size_type max_size() const noexcept
    {
        return (std::numeric_limits<size_type>::max)() / sizeof(T);
    }

    template <typename U>
    friend class xr_frame_allocator;
};

template <typename T, typename U>
inline bool operator==(const xr_frame_allocator<T>& left, const xr_frame_allocator<U>& right) noexcept
{
    return left.arena == right.arena;
}

template <typename T, typename U>
inline bool operator!=(const xr_frame_allocator<T>& left, const xr_frame_allocator<U>& right) noexcept
{
    return !(left == right);
}

// std::vector-compatible container whose storage can be detached before the
// backing arena is reset.  Elements are still destroyed normally, which keeps
// intrusive pointers and other non-trivial values safe.
template <typename T>
class xr_frame_vector : public xr_vector<T, xr_frame_allocator<T>>
{
private:
    using inherited = xr_vector<T, xr_frame_allocator<T>>;
    size_t m_reserve_hint;

public:
    explicit xr_frame_vector(xr_frame_arena& arena)
        : inherited(xr_frame_allocator<T>(&arena)), m_reserve_hint(0u)
    {
    }

    xr_frame_vector(const xr_frame_vector&) = delete;
    xr_frame_vector& operator=(const xr_frame_vector&) = delete;

    void discard_storage()
    {
        m_reserve_hint = this->capacity();
        inherited empty(this->get_allocator());
        inherited::swap(empty);
    }

    // Recreates the previous frame's normal capacity with one bump allocation,
    // avoiding repeated vector growth/copies after the arena reset. The byte cap
    // prevents a one-off pathological frame from becoming a permanent reserve.
    void prepare_storage(size_t max_reserve_bytes)
    {
        const size_t max_elements = max_reserve_bytes / sizeof(T);
        const size_t target = m_reserve_hint < max_elements ? m_reserve_hint : max_elements;
        if (target)
            inherited::reserve(target);
    }
};
