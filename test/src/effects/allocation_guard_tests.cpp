// The replacement global operator new behind AllocationGuard
// (allocation_guard.h), and a test that it counts what it should. Replacing
// operator new is a property of the whole test binary; it only counts while a
// guard is alive on the calling thread and otherwise forwards to malloc. On
// glibc malloc, calloc and realloc are wrapped the same way (they forward to
// glibc's own, so free and everything else stay glibc's).
#include "effects/allocation_guard.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <vector>

namespace fxtest::allocation_guard_detail {

bool& armed() noexcept {
  static thread_local bool value = false;
  return value;
}

long& counter() noexcept {
  static thread_local long value = 0;
  return value;
}

long& heapCounter() noexcept {
  static thread_local long value = 0;
  return value;
}

bool heapCounted() noexcept {
#if defined(__GLIBC__)
  return true;
#else
  return false;
#endif
}

}  // namespace fxtest::allocation_guard_detail

#if defined(__GLIBC__)
extern "C" {
void* __libc_malloc(std::size_t size);
void* __libc_calloc(std::size_t count, std::size_t size);
void* __libc_realloc(void* p, std::size_t size);

void* malloc(std::size_t size) noexcept {
  if (fxtest::allocation_guard_detail::armed())
    ++fxtest::allocation_guard_detail::heapCounter();
  return __libc_malloc(size);
}

void* calloc(std::size_t count, std::size_t size) noexcept {
  if (fxtest::allocation_guard_detail::armed())
    ++fxtest::allocation_guard_detail::heapCounter();
  return __libc_calloc(count, size);
}

void* realloc(void* p, std::size_t size) noexcept {
  if (fxtest::allocation_guard_detail::armed())
    ++fxtest::allocation_guard_detail::heapCounter();
  return __libc_realloc(p, size);
}
}
#endif

void* operator new(std::size_t size) {
  if (fxtest::allocation_guard_detail::armed())
    ++fxtest::allocation_guard_detail::counter();
  if (void* p = std::malloc(size == 0 ? 1 : size))
    return p;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  if (fxtest::allocation_guard_detail::armed())
    ++fxtest::allocation_guard_detail::counter();
  if (void* p = std::malloc(size == 0 ? 1 : size))
    return p;
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

TEST(AllocationGuardTest, CountsOperatorNewOnlyWhileArmed) {
  using fxtest::AllocationGuard;
  {
    std::vector<int> before(100);  // not counted: no guard yet
    EXPECT_EQ(before.size(), 100u);
  }
  AllocationGuard guard;
  EXPECT_EQ(guard.count(), 0);
  // Through a volatile pointer, so the compiler may not elide the pair.
  std::vector<int>* volatile grown = new std::vector<int>(1000);
  EXPECT_GE(guard.count(), 2) << "the vector and its storage";
  delete grown;
  const long afterVector = guard.count();
  int onStack[64] = {};
  onStack[3] = 1;
  EXPECT_EQ(onStack[3], 1);
  EXPECT_EQ(guard.count(), afterVector) << "stack use is not an allocation";
}

TEST(AllocationGuardTest, HeapCountSeesMallocWhereItCan) {
  using fxtest::AllocationGuard;
  AllocationGuard guard;
  // Through a volatile pointer, so the compiler may not elide the pair.
  void* volatile block = std::malloc(256);
  std::free(block);
  if (AllocationGuard::heapCounted())
    EXPECT_GE(guard.heapCount(), 1) << "a direct malloc";
  EXPECT_EQ(guard.count(), 0) << "malloc is not operator new";
}
