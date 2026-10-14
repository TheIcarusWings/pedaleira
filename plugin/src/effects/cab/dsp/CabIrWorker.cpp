// The shared builder thread of the cab.model blocks; see CabIrWorker.h.
#include "CabIrWorker.h"

#include <algorithm>
#include <chrono>

namespace cabfx {

namespace {

// How often the clients are visited when nothing wakes the thread: short
// enough that a knob move is heard at once, long enough to cost nothing.
constexpr auto kTick = std::chrono::milliseconds(10);

std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::weak_ptr<CabIrWorker>& registry() {
  static std::weak_ptr<CabIrWorker> worker;
  return worker;
}

}  // namespace

std::shared_ptr<CabIrWorker> CabIrWorker::acquire() {
  const std::lock_guard<std::mutex> lock(registryMutex());
  if (auto existing = registry().lock())
    return existing;
  std::shared_ptr<CabIrWorker> created(new CabIrWorker());
  registry() = created;
  return created;
}

CabIrWorker::CabIrWorker() : thread([this] { run(); }) {}

CabIrWorker::~CabIrWorker() {
  {
    const std::lock_guard<std::mutex> lock(mutex);
    quit = true;
  }
  condition.notify_all();
  if (thread.joinable())
    thread.join();
}

void CabIrWorker::addClient(CabIrClient* client) {
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (std::find(clients.begin(), clients.end(), client) == clients.end())
      clients.push_back(client);
    woken = true;
  }
  condition.notify_all();
}

void CabIrWorker::removeClient(CabIrClient* client) {
  // The thread services clients with the mutex held, so taking it here waits
  // for a build in progress to finish.
  const std::lock_guard<std::mutex> lock(mutex);
  clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
}

void CabIrWorker::wake() {
  {
    const std::lock_guard<std::mutex> lock(mutex);
    woken = true;
  }
  condition.notify_all();
}

void CabIrWorker::run() {
  std::unique_lock<std::mutex> lock(mutex);
  while (!quit) {
    if (clients.empty())
      condition.wait(lock, [this] { return quit || woken; });
    else
      condition.wait_for(lock, kTick, [this] { return quit || woken; });
    woken = false;
    if (quit)
      break;
    for (CabIrClient* client : clients)
      client->serviceBuild();
  }
}

}  // namespace cabfx
