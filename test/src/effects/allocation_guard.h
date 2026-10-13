#pragma once
// A test-only check that code under test does not allocate: while an
// AllocationGuard is alive, every call to the global operator new on the
// creating thread is counted (the replacement lives in
// allocation_guard_tests.cpp, linked into the same test binary). Other threads
// are not counted. Calls to malloc that do not go through operator new (a
// juce::HeapBlock, an AudioBuffer) are not seen: the guard catches the
// std::vector, std::string, std::function and make_unique kind of allocation.
//
//   AllocationGuard guard;
//   kind.render(input);
//   EXPECT_EQ(guard.count(), 0);

namespace fxtest {

namespace allocation_guard_detail {
// Defined in allocation_guard_tests.cpp.
bool& armed() noexcept;
long& counter() noexcept;
}  // namespace allocation_guard_detail

class AllocationGuard {
public:
  AllocationGuard() noexcept : previous(allocation_guard_detail::armed()) {
    start = allocation_guard_detail::counter();
    allocation_guard_detail::armed() = true;
  }
  ~AllocationGuard() { allocation_guard_detail::armed() = previous; }
  AllocationGuard(const AllocationGuard&) = delete;
  AllocationGuard& operator=(const AllocationGuard&) = delete;

  /** Allocations on this thread since the guard was created. */
  long count() const noexcept { return allocation_guard_detail::counter() - start; }

private:
  bool previous;
  long start;
};

}  // namespace fxtest
