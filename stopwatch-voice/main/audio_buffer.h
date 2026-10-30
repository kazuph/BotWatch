#pragma once
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <type_traits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
namespace audio_memory {
struct Free {
    void operator()(void *p) const noexcept {
#ifdef ESP_PLATFORM
        heap_caps_free(p);
#else
        std::free(p);
#endif
    }
};
// Reserve once after the model loads. All app arrays are views into this block;
// only one audio app is active, and leaving it rewinds the shared cursor.
class Arena {
    std::unique_ptr<std::byte,Free> memory;
    size_t capacity_=0,used_=0;
public:
    bool reserve(size_t bytes) noexcept {
        if(memory)return bytes<=capacity_;
#ifdef ESP_PLATFORM
        auto *p=static_cast<std::byte*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#else
        auto *p=static_cast<std::byte*>(std::malloc(bytes));
#endif
        if(!p)return false;
        memory.reset(p);capacity_=bytes;return true;
    }
    void *take(size_t bytes,size_t alignment) noexcept {
        if(!memory)return nullptr;
        void *p=memory.get()+used_;size_t space=capacity_-used_;
        if(!std::align(alignment,bytes,p,space))return nullptr;
        used_=static_cast<std::byte*>(p)-memory.get()+bytes;return p;
    }
    void rewind() noexcept {used_=0;}
    size_t capacity() const noexcept {return capacity_;}
    size_t used() const noexcept {return used_;}
};
inline Arena arena;
template<class T> class Buffer {
    static_assert(std::is_trivially_destructible_v<T>);
    T *memory=nullptr;
    size_t count=0;
public:
    bool allocate(size_t size) noexcept {
        reset();auto *p=static_cast<T*>(arena.take(size*sizeof(T),alignof(T)));
        if(!p)return false;
        std::uninitialized_value_construct_n(p,size);memory=p;count=size;return true;
    }
    void reset() noexcept {memory=nullptr;count=0;}
    T *data() noexcept {return memory;}
    const T *data() const noexcept {return memory;}
    size_t size() const noexcept {return count;}
    size_t capacity() const noexcept {return count;}
    size_t bytes() const noexcept {return count*sizeof(T);}
    T &operator[](size_t i) noexcept {return memory[i];}
    const T &operator[](size_t i) const noexcept {return memory[i];}
};
}
