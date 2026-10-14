#pragma once
// The thread that builds the cab.model blocks' impulse responses off the audio
// thread.
//
// One thread serves every cab block of the process. Each block registers as a
// client while it exists; the thread visits every client about every 10 ms
// (sooner when woken) and calls serviceBuild(), in which the client compares
// its parameters with the ones its live response was built from and, when they
// differ, builds a new response and loads it into its idle convolver. The
// audio thread is never involved beyond picking up a finished convolver (see
// CabModel.cpp), so a block changes its sound with no allocation, lock or
// signal on the audio thread.
//
// The thread exists while at least one client does: the first client starts
// it, the last one to go stops it.
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cabfx {

class CabIrClient {
public:
  virtual ~CabIrClient() = default;
  /** On the worker thread: build whatever the client needs. May take a few
      milliseconds; never called for a client after removeClient returned. */
  virtual void serviceBuild() = 0;
};

class CabIrWorker {
public:
  /** The shared worker, started if no client holds it. Not for the audio
      thread. */
  static std::shared_ptr<CabIrWorker> acquire();

  ~CabIrWorker();
  CabIrWorker(const CabIrWorker&) = delete;
  CabIrWorker& operator=(const CabIrWorker&) = delete;

  void addClient(CabIrClient* client);
  /** Waits while the client is being serviced, so the caller may destroy it
      afterwards. */
  void removeClient(CabIrClient* client);
  /** Visit the clients now rather than at the next tick. Not for the audio
      thread (it takes a mutex). */
  void wake();

private:
  CabIrWorker();
  void run();

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<CabIrClient*> clients;
  bool woken{false};
  bool quit{false};
  std::thread thread;
};

}  // namespace cabfx
