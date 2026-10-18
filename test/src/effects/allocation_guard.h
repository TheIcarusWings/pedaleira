#pragma once
// A test-only check that code under test does not allocate: while an
// AllocationGuard is alive, every call to the global operator new on the
// creating thread is counted (the replacement lives in
// allocation_guard_tests.cpp, linked into the same test binary). Other threads
// are not counted. count() sees the std::vector, std::string, std::function
// and make_unique kind of allocation.
//
// heapCount() also sees what goes to malloc directly (Eigen's matrices, a
// juce::HeapBlock): on glibc the binary wraps malloc, calloc and realloc as
// well. Elsewhere it is count() again; heapCounted() says which.
//
//   AllocationGuard guard;
//   kind.render(input);
//   EXPECT_EQ(guard.count(), 0);

namespace fxtest {

namespace allocation_guard_detail {
// Defined in allocation_guard_tests.cpp.
bool& armed() noexcept;
long& counter() noexcept;
long& heapCounter() noexcept;
bool heapCounted() noexcept;
}  // namespace allocation_guard_detail

class AllocationGuard {
public:
  AllocationGuard() noexcept : previous(allocation_guard_detail::armed()) {
    start = allocation_guard_detail::counter();
    heapStart = allocation_guard_detail::heapCounter();
    allocation_guard_detail::armed() = true;
  }
  ~AllocationGuard() { allocation_guard_detail::armed() = previous; }
  AllocationGuard(const AllocationGuard&) = delete;
  AllocationGuard& operator=(const AllocationGuard&) = delete;

  /** Allocations on this thread since the guard was created. */
  long count() const noexcept { return allocation_guard_detail::counter() - start; }

  /** Allocations on this thread since the guard was created, malloc
      included where heapCounted(). */
  long heapCount() const noexcept {
    return allocation_guard_detail::heapCounted()
               ? allocation_guard_detail::heapCounter() - heapStart
               : count();
  }

  /** heapCount() sees malloc (glibc builds). */
  static bool heapCounted() noexcept { return allocation_guard_detail::heapCounted(); }

private:
  bool previous;
  long start;
  long heapStart;
};

}  // namespace fxtest
