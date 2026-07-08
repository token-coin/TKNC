// Copyright (c) 2015-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_SCHEDULER_H
#define TKN_SCHEDULER_H

#include <attributes.h>
#include <sync.h>
#include <util/task_runner.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <thread>
#include <utility>

// Simple class for background tasks run periodically or once "after a while". Usage: create CScheduler, scheduleFromNow(func, delta), run serviceQueue() in a thread. At shutdown call stop() then join/delete thread and scheduler.
class CScheduler
{
public:
    CScheduler();
    ~CScheduler();

    std::thread m_service_thread;

    typedef std::function<void()> Function;

    /** Call func at/after time t */
    void schedule(Function f, std::chrono::steady_clock::time_point t) EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

    /** Call f once after the delta has passed */
    void scheduleFromNow(Function f, std::chrono::milliseconds delta) EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex)
    {
        schedule(std::move(f), std::chrono::steady_clock::now() + delta);
    }

    // Repeat f until scheduler is stopped. First run after delta. Timing is not exact (rescheduled after each run finishes); not for accurate scheduling.
    void scheduleEvery(Function f, std::chrono::milliseconds delta) EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

    // Mock scheduler to fast forward in time; reschedules all taskQueue items delta_seconds sooner.
    void MockForward(std::chrono::seconds delta_seconds) EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

    // Services the queue 'forever'. Should be run in a thread.
    void serviceQueue() EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

    /** Tell any threads running serviceQueue to stop as soon as the current task is done */
    void stop() EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex)
    {
        WITH_LOCK(newTaskMutex, stopRequested = true);
        newTaskScheduled.notify_all();
        if (m_service_thread.joinable()) m_service_thread.join();
    }
    /** Tell any threads running serviceQueue to stop when there is no work left to be done */
    void StopWhenDrained() EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex)
    {
        WITH_LOCK(newTaskMutex, stopWhenEmpty = true);
        newTaskScheduled.notify_all();
        if (m_service_thread.joinable()) m_service_thread.join();
    }

    // Returns number of tasks waiting to be serviced, and first and last task times.
    size_t getQueueInfo(std::chrono::steady_clock::time_point& first,
                        std::chrono::steady_clock::time_point& last) const
        EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

    /** Returns true if there are threads actively running in serviceQueue() */
    bool AreThreadsServicingQueue() const EXCLUSIVE_LOCKS_REQUIRED(!newTaskMutex);

private:
    mutable Mutex newTaskMutex;
    std::condition_variable newTaskScheduled;
    std::multimap<std::chrono::steady_clock::time_point, Function> taskQueue GUARDED_BY(newTaskMutex);
    int nThreadsServicingQueue GUARDED_BY(newTaskMutex){0};
    bool stopRequested GUARDED_BY(newTaskMutex){false};
    bool stopWhenEmpty GUARDED_BY(newTaskMutex){false};
    bool shouldStop() const EXCLUSIVE_LOCKS_REQUIRED(newTaskMutex) { return stopRequested || (stopWhenEmpty && taskQueue.empty()); }
};

// Class for CScheduler clients scheduling multiple jobs to run serially. Jobs may run on different threads but never concurrently; memory is release-acquire consistent between callbacks (B() observes all effects of A() that ran before).
class SerialTaskRunner : public util::TaskRunnerInterface
{
private:
    CScheduler& m_scheduler;

    Mutex m_callbacks_mutex;

    // Scheduler may run in multiple threads; we maintain our own queue to ensure in-order callback execution.
    std::list<std::function<void()>> m_callbacks_pending GUARDED_BY(m_callbacks_mutex);
    bool m_are_callbacks_running GUARDED_BY(m_callbacks_mutex) = false;

    void MaybeScheduleProcessQueue() EXCLUSIVE_LOCKS_REQUIRED(!m_callbacks_mutex);
    void ProcessQueue() EXCLUSIVE_LOCKS_REQUIRED(!m_callbacks_mutex);

public:
    explicit SerialTaskRunner(CScheduler& scheduler LIFETIMEBOUND) : m_scheduler{scheduler} {}

    // Add a callback to be executed serially. Memory is release-acquire consistent between callbacks; behaves as if executed in order by a single thread.
    void insert(std::function<void()> func) override EXCLUSIVE_LOCKS_REQUIRED(!m_callbacks_mutex);

    // Process all remaining queue members on calling thread, blocking until empty. Must be called after CScheduler has no remaining processing threads.
    void flush() override EXCLUSIVE_LOCKS_REQUIRED(!m_callbacks_mutex);

    size_t size() override EXCLUSIVE_LOCKS_REQUIRED(!m_callbacks_mutex);
};

#endif // TKN_SCHEDULER_H
