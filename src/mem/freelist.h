#pragma once
/**
 * This file encapsulates the in disused object free lists
 * that are used per slab of small objects. The implementation
 * can be configured to introduce randomness to the reallocation,
 * and also provide signing to detect free list corruption.
 *
 * # Corruption
 *
 * The corruption detection works as follows
 *
 *   FreeObject
 *   -----------------------------
 *   | next | prev_encoded | ... |
 *   -----------------------------
 * A free object contains a pointer to next object in the free list, and
 * a prev pointer, but the prev pointer is really a signature with the
 * following property
 *
 *  If n = c->next && n != 0, then n->prev_encoded = f(c,n).
 *
 * If f just returns the first parameter, then this degenerates to a doubly
 * linked list.  (Note that doing the degenerate case can be useful for
 * debugging snmalloc bugs.) By making it a function of both pointers, it
 * makes it harder for an adversary to mutate prev_encoded to a valid value.
 *
 * This provides protection against the free-list being corrupted by memory
 * safety issues.
 *
 * # Randomness
 *
 * The randomness is introduced by building two free lists simulatenously,
 * and randomly deciding which list to add an element to.
 */

#include "../ds/address.h"
#include "allocconfig.h"
#include "entropy.h"

#include <cstdint>

namespace snmalloc
{
  /**
   * This function is used to sign back pointers in the free list.
   */
  inline static uintptr_t
  signed_prev(address_t curr, address_t next, const FreeListKey& key)
  {
    auto c = curr;
    auto n = next;
    return (c + key.key1) * (n + key.key2);
  }

  /**
   * Free objects within each slab point directly to the next.
   * There is an optional second field that is effectively a
   * back pointer in a doubly linked list, however, it is encoded
   * to prevent corruption.
   *
   * TODO: Consider putting prev_encoded at the end of the object, would
   * require size to be threaded through, but would provide more OOB
   * detection.
   */
  class FreeObject
  {
    union
    {
      capptr::AllocFull<FreeObject> next_object;
      // TODO: Should really use C++20 atomic_ref rather than a union.
      capptr::AtomicAllocFull<FreeObject> atomic_next_object;
    };
#ifdef SNMALLOC_CHECK_CLIENT
    // Encoded representation of a back pointer.
    // Hard to fake, and provides consistency on
    // the next pointers.
    address_t prev_encoded;
#endif

  public:
    static capptr::AllocFull<FreeObject> make(capptr::AllocFull<void> p)
    {
      return p.template as_static<FreeObject>();
    }

    /**
     * Encode next
     */
    inline static capptr::AllocFull<FreeObject> encode_next(
      address_t curr,
      capptr::AllocFull<FreeObject> next,
      const FreeListKey& key)
    {
      // Note we can consider other encoding schemes here.
      //   * XORing curr and next.  This doesn't require any key material
      //   * XORing (curr * key). This makes it harder to guess the underlying
      //     key, as each location effectively has its own key.
      // Curr is not used in the current encoding scheme.
      UNUSED(curr);

      if constexpr (CHECK_CLIENT && !aal_supports<StrictProvenance>)
      {
        return capptr::AllocFull<FreeObject>(reinterpret_cast<FreeObject*>(
          reinterpret_cast<uintptr_t>(next.unsafe_ptr()) ^ key.key_next));
      }
      else
      {
        UNUSED(key);
        return next;
      }
    }

    /**
     * Assign next_object and update its prev_encoded if SNMALLOC_CHECK_CLIENT.
     * Static so that it can be used on reference to a FreeObject.
     *
     * Returns a pointer to the next_object field of the next parameter as an
     * optimization for repeated snoc operations (in which
     * next->next_object is nullptr).
     */
    static capptr::AllocFull<FreeObject>* store_next(
      capptr::AllocFull<FreeObject>* curr,
      capptr::AllocFull<FreeObject> next,
      const FreeListKey& key)
    {
#ifdef SNMALLOC_CHECK_CLIENT
      next->prev_encoded =
        signed_prev(address_cast(curr), address_cast(next), key);
#else
      UNUSED(key);
#endif
      *curr = encode_next(address_cast(curr), next, key);
      return &(next->next_object);
    }

    static void
    store_null(capptr::AllocFull<FreeObject>* curr, const FreeListKey& key)
    {
      *curr = encode_next(address_cast(curr), nullptr, key);
    }

    /**
     * Assign next_object and update its prev_encoded if SNMALLOC_CHECK_CLIENT
     *
     * Uses the atomic view of next, so can be used in the message queues.
     */
    void atomic_store_next(
      capptr::AllocFull<FreeObject> next, const FreeListKey& key)
    {
#ifdef SNMALLOC_CHECK_CLIENT
      next->prev_encoded =
        signed_prev(address_cast(this), address_cast(next), key);
#else
      UNUSED(key);
#endif
      // Signature needs to be visible before item is linked in
      // so requires release semantics.
      atomic_next_object.store(
        encode_next(address_cast(&next_object), next, key),
        std::memory_order_release);
    }

    void atomic_store_null(const FreeListKey& key)
    {
      atomic_next_object.store(
        encode_next(address_cast(&next_object), nullptr, key),
        std::memory_order_relaxed);
    }

    capptr::AllocFull<FreeObject> atomic_read_next(const FreeListKey& key)
    {
      auto n = encode_next(
        address_cast(&next_object),
        atomic_next_object.load(std::memory_order_acquire),
        key);
#ifdef SNMALLOC_CHECK_CLIENT
      if (n != nullptr)
      {
        n->check_prev(signed_prev(address_cast(this), address_cast(n), key));
      }
#else
      UNUSED(key);
#endif
      return n;
    }

    /**
     * Check the signature of this FreeObject
     */
    void check_prev(address_t signed_prev)
    {
      UNUSED(signed_prev);
      check_client(
        signed_prev == prev_encoded, "Heap corruption - free list corrupted!");
    }

    /**
     * Read the next pointer
     */
    capptr::AllocFull<FreeObject> read_next(const FreeListKey& key)
    {
      return encode_next(address_cast(&next_object), next_object, key);
    }
  };

  static_assert(
    sizeof(FreeObject) <= MIN_ALLOC_SIZE,
    "Needs to be able to fit in smallest allocation.");

  /**
   * Used to iterate a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListIter
  {
    capptr::AllocFull<FreeObject> curr{nullptr};
#ifdef SNMALLOC_CHECK_CLIENT
    address_t prev{0};
#endif

  public:
    constexpr FreeListIter(
      capptr::AllocFull<FreeObject> head, address_t prev_value)
    : curr(head)
    {
#ifdef SNMALLOC_CHECK_CLIENT
      prev = prev_value;
#endif
      UNUSED(prev_value);
    }

    constexpr FreeListIter() = default;

    /**
     * Checks if there are any more values to iterate.
     */
    bool empty()
    {
      return curr == nullptr;
    }

    /**
     * Returns current head without affecting the iterator.
     */
    capptr::AllocFull<FreeObject> peek()
    {
      return curr;
    }

    /**
     * Moves the iterator on, and returns the current value.
     */
    capptr::AllocFull<FreeObject> take(const FreeListKey& key)
    {
      auto c = curr;
      auto next = curr->read_next(key);

      Aal::prefetch(next.unsafe_ptr());
      curr = next;
#ifdef SNMALLOC_CHECK_CLIENT
      c->check_prev(prev);
      prev = signed_prev(address_cast(c), address_cast(next), key);
#else
      UNUSED(key);
#endif

      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Adds signing of pointers in the SNMALLOC_CHECK_CLIENT mode
   *
   * If RANDOM is enabled, the builder uses two queues, and
   * "randomly" decides to add to one of the two queues.  This
   * means that we will maintain a randomisation of the order
   * between allocations.
   *
   * The fields are paired up to give better codegen as then they are offset
   * by a power of 2, and the bit extract from the interleaving seed can
   * be shifted to calculate the relevant offset to index the fields.
   *
   * If RANDOM is set to false, then the code does not perform any
   * randomisation.
   */
  template<bool RANDOM, bool INIT = true>
  class FreeListBuilder
  {
    static constexpr size_t LENGTH = RANDOM ? 2 : 1;

    // Pointer to the first element.
    std::array<capptr::AllocFull<FreeObject>, LENGTH> head;
    // Pointer to the reference to the last element.
    // In the empty case end[i] == &head[i]
    // This enables branch free enqueuing.
    std::array<capptr::AllocFull<FreeObject>*, LENGTH> end{nullptr};

    std::array<uint16_t, RANDOM ? 2 : 0> length{};

  public:
    constexpr FreeListBuilder()
    {
      if (INIT)
        init();
    }

    /**
     * Checks if the builder contains any elements.
     */
    bool empty()
    {
      for (size_t i = 0; i < LENGTH; i++)
      {
        if (address_cast(end[i]) != address_cast(&head[i]))
          return false;
      }
      return true;
    }

    /**
     * Adds an element to the builder
     */
    void add(
      capptr::AllocFull<FreeObject> n,
      const FreeListKey& key,
      LocalEntropy& entropy)
    {
      uint32_t index;
      if constexpr (RANDOM)
        index = entropy.next_bit();
      else
        index = 0;

      end[index] = FreeObject::store_next(end[index], n, key);
      if constexpr (RANDOM)
      {
        length[index]++;
      }
    }

    /**
     * Adds an element to the builder, if we are guaranteed that
     * RANDOM is false.  This is useful in certain construction
     * cases that do not need to introduce randomness, such as
     * during the initialisation construction of a free list, which
     * uses its own algorithm, or during building remote deallocation
     * lists, which will be randomised at the other end.
     */
    template<bool RANDOM_ = RANDOM>
    std::enable_if_t<!RANDOM_>
    add(capptr::AllocFull<FreeObject> n, const FreeListKey& key)
    {
      static_assert(RANDOM_ == RANDOM, "Don't set template parameter");
      end[0] = FreeObject::store_next(end[0], n, key);
    }

    /**
     * Makes a terminator to a free list.
     */
    SNMALLOC_FAST_PATH void
    terminate_list(uint32_t index, const FreeListKey& key)
    {
      FreeObject::store_null(end[index], key);
    }

    /**
     * Read head removing potential encoding
     *
     * Although, head does not require meta-data protection
     * as it is not stored in an object allocation. For uniformity
     * it is treated like the next_object field in a FreeObject
     * and is thus subject to encoding if the next_object pointers
     * encoded.
     */
    capptr::AllocFull<FreeObject>
    read_head(uint32_t index, const FreeListKey& key)
    {
      return FreeObject::encode_next(
        address_cast(&head[index]), head[index], key);
    }

    address_t get_fake_signed_prev(uint32_t index, const FreeListKey& key)
    {
      return signed_prev(
        address_cast(&head[index]), address_cast(read_head(index, key)), key);
    }

    /**
     * Close a free list, and set the iterator parameter
     * to iterate it.
     *
     * In the RANDOM case, it may return only part of the freelist.
     *
     * The return value is how many entries are still contained in the builder.
     */
    SNMALLOC_FAST_PATH uint16_t close(FreeListIter& fl, const FreeListKey& key)
    {
      uint32_t i;
      if constexpr (RANDOM)
      {
        SNMALLOC_ASSERT(end[1] != &head[0]);
        SNMALLOC_ASSERT(end[0] != &head[1]);

        // Select longest list.
        i = length[0] > length[1] ? 0 : 1;
      }
      else
      {
        i = 0;
      }

      terminate_list(i, key);

      fl = {read_head(i, key), get_fake_signed_prev(i, key)};

      end[i] = &head[i];

      if constexpr (RANDOM)
      {
        length[i] = 0;
        return length[1 - i];
      }
      else
      {
        return 0;
      }
    }

    /**
     * Set the builder to a not building state.
     */
    constexpr void init()
    {
      for (size_t i = 0; i < LENGTH; i++)
      {
        end[i] = &head[i];
        if (RANDOM)
        {
          length[i] = 0;
        }
      }
    }

    template<bool RANDOM_ = RANDOM>
    std::enable_if_t<
      !RANDOM_,
      std::pair<capptr::AllocFull<FreeObject>, capptr::AllocFull<FreeObject>>>
    extract_segment(const FreeListKey& key)
    {
      static_assert(RANDOM_ == RANDOM, "Don't set SFINAE parameter!");
      SNMALLOC_ASSERT(!empty());

      auto first = read_head(0, key);
      // end[0] is pointing to the first field in the object,
      // this is doing a CONTAINING_RECORD like cast to get back
      // to the actual object.  This isn't true if the builder is
      // empty, but you are not allowed to call this in the empty case.
      auto last =
        capptr::AllocFull<FreeObject>(reinterpret_cast<FreeObject*>(end[0]));
      init();
      return {first, last};
    }
  };
} // namespace snmalloc
