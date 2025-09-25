#pragma once

#include <iostream>
#include <vector>
#include <atomic>
#include <immintrin.h>

#include "macros.h"

namespace Common {
  template<typename T>
  class LFQueue final {
  public:
    explicit LFQueue(std::size_t num_elems) :
        store_(round_up_to_power_of_2(num_elems), T()),
        mask_(store_.size() - 1),
        capacity_(store_.size()) {
    }

    auto tryGetNextToWriteTo() noexcept -> T* {
      auto current_write = next_write_index_.load(std::memory_order_relaxed);
      auto current_read = next_read_index_.load(std::memory_order_acquire);
      
      if (UNLIKELY((current_write + 1) & mask_ == current_read & mask_)) {
        return nullptr;
      }
      return &store_[current_write & mask_];
    }

    auto getNextToWriteTo() noexcept -> T* {
      while (true) {
        auto slot = tryGetNextToWriteTo();
        if (LIKELY(slot != nullptr)) {
          return slot;
        }
        
        _mm_pause();
      }
    }

    auto updateWriteIndex() noexcept {
      auto current_write_index = next_write_index_.load(std::memory_order_relaxed);
      next_write_index_.store(current_write_index + 1, std::memory_order_release);
      num_elements_.fetch_add(1, std::memory_order_release);
    }

    auto getNextToRead() const noexcept -> const T * {
        auto current_read_index = next_read_index_.load(std::memory_order_relaxed);
        auto current_element_count = num_elements_.load(std::memory_order_acquire);
    
        if (LIKELY(current_element_count > 0)) {
            std::size_t target_index = current_read_index & mask_;
            return &store_[target_index];
        } else {
            return nullptr;
        }
    }

    auto updateReadIndex() noexcept {
      auto current_read_index = next_read_index_.load(std::memory_order_relaxed);
      next_read_index_.store(current_read_index + 1, std::memory_order_release);
      num_elements_.fetch_sub(1, std::memory_order_release);
    }

    auto size() const noexcept {
      return num_elements_.load(std::memory_order_acquire);
    }

    auto is_full() const noexcept -> bool {
      auto current_write = next_write_index_.load(std::memory_order_relaxed);
      auto current_read = next_read_index_.load(std::memory_order_relaxed);
      return (current_write + 1) & mask_ == current_read & mask_;
    }

    auto capacity() const noexcept -> std::size_t {
      return capacity_;
    }

    LFQueue() = delete;
    LFQueue(const LFQueue &) = delete;
    LFQueue(const LFQueue &&) = delete;
    LFQueue &operator=(const LFQueue &) = delete;
    LFQueue &operator=(const LFQueue &&) = delete;

  private:
    static std::size_t round_up_to_power_of_2(std::size_t v) {
      if (UNLIKELY(v == 0)) return 1;
      
      --v;
      v |= v >> 1;
      v |= v >> 2;
      v |= v >> 4;
      v |= v >> 8;
      v |= v >> 16;
      v |= v >> 32;
      ++v;
      
      return v;
    }

    std::vector<T> store_;
    const std::size_t mask_;
    const std::size_t capacity_;

    alignas(64) std::atomic<std::size_t> next_write_index_ = {0};
    alignas(64) std::atomic<std::size_t> next_read_index_ = {0};
    alignas(64) std::atomic<std::size_t> num_elements_ = {0};
  };
}
