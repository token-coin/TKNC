// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_SYNC_H
#define TKN_SYNC_H

// This header declares threading primitives compatible with Clang
// Thread Safety Analysis and provides appropriate annotation macros.
#include <threadsafety.h> // IWYU pragma: export
#include <util/macros.h>

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>


// Actual implementation:

#ifdef DEBUG_LOCKORDER
template <typename MutexType>
void EnterCritical(const char* pszName, const char* pszFile, int nLine, MutexType* cs, bool fTry = false);
void LeaveCritical();
void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line);
template <typename MutexType>
void AssertLockHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) EXCLUSIVE_LOCKS_REQUIRED(cs);
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs);
void DeleteLock(void* cs);
bool LockStackEmpty();

// If true, abort() on potential lock-order deadlock bug instead of logging+throwing logic_error. Defaults to true; set false in DEBUG_LOCKORDER unit tests.
extern bool g_debug_lockorder_abort;
#else
template <typename MutexType>
inline void EnterCritical(const char* pszName, const char* pszFile, int nLine, MutexType* cs, bool fTry = false) {}
inline void LeaveCritical() {}
inline void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line) {}
template <typename MutexType>
inline void AssertLockHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) EXCLUSIVE_LOCKS_REQUIRED(cs) {}
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs) {}
inline void DeleteLock(void* cs) {}
inline bool LockStackEmpty() { return true; }
#endif

// Called when a mutex fails to lock immediately (held by another thread or spurious); responsible for locking the lock before returning.
#ifdef DEBUG_LOCKCONTENTION

template <typename LockType>
void ContendedLock(std::string_view name, std::string_view file, int nLine, LockType& lock);
#endif

// Template mixin that adds -Wthread-safety locking annotations and lock order checking to a subset of the mutex API.
template <typename PARENT>
class LOCKABLE AnnotatedMixin : public PARENT
{
public:
 ~AnnotatedMixin() {
 DeleteLock((void*)this);
 }

 void lock() EXCLUSIVE_LOCK_FUNCTION()
 {
 PARENT::lock();
 }

 void unlock() UNLOCK_FUNCTION()
 {
 PARENT::unlock();
 }

 bool try_lock() EXCLUSIVE_TRYLOCK_FUNCTION(true)
 {
 return PARENT::try_lock();
 }

 using unique_lock = std::unique_lock<PARENT>;
#ifdef __clang__
 // For negative capabilities in Clang Thread Safety Analysis: EXCLUSIVE_LOCKS_REQUIRED with ! operator indicates a mutex should not be held.
 const AnnotatedMixin& operator!() const { return *this; }
#endif // __clang__
};

// Wrapped mutex: supports recursive locking, but no waiting. TODO: We should move away from using the recursive lock by default.
using RecursiveMutex = AnnotatedMixin<std::recursive_mutex>;

// Wrapped mutex: supports waiting but not recursive locking.
using Mutex = AnnotatedMixin<std::mutex>;

class GlobalMutex : public Mutex { };

#define AssertLockHeld(cs) AssertLockHeldInternal(#cs, __FILE__, __LINE__, &cs)

inline void AssertLockNotHeldInline(const char* name, const char* file, int line, Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, RecursiveMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, GlobalMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
#define AssertLockNotHeld(cs) AssertLockNotHeldInline(#cs, __FILE__, __LINE__, &cs)

/** Wrapper around std::unique_lock style lock for MutexType. */
template <typename MutexType>
class SCOPED_LOCKABLE UniqueLock : public MutexType::unique_lock
{
private:
 using Base = typename MutexType::unique_lock;

 void Enter(const char* pszName, const char* pszFile, int nLine)
 {
 EnterCritical(pszName, pszFile, nLine, Base::mutex());
#ifdef DEBUG_LOCKCONTENTION
 if (!Base::try_lock()) {
 ContendedLock(pszName, pszFile, nLine, static_cast<Base&>(*this));
 }
#else
 Base::lock();
#endif
 }

 bool TryEnter(const char* pszName, const char* pszFile, int nLine)
 {
 EnterCritical(pszName, pszFile, nLine, Base::mutex(), true);
 if (Base::try_lock()) {
 return true;
 }
 LeaveCritical();
 return false;
 }

public:
 UniqueLock(MutexType& mutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(mutexIn) : Base(mutexIn, std::defer_lock)
 {
 if (fTry)
 TryEnter(pszName, pszFile, nLine);
 else
 Enter(pszName, pszFile, nLine);
 }

 UniqueLock(MutexType* pmutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(pmutexIn)
 {
 if (!pmutexIn) return;

 *static_cast<Base*>(this) = Base(*pmutexIn, std::defer_lock);
 if (fTry)
 TryEnter(pszName, pszFile, nLine);
 else
 Enter(pszName, pszFile, nLine);
 }

 ~UniqueLock() UNLOCK_FUNCTION()
 {
 if (Base::owns_lock())
 LeaveCritical();
 }

 operator bool()
 {
 return Base::owns_lock();
 }

protected:
 // needed for reverse_lock
 UniqueLock() = default;

public:
 // An RAII-style reverse lock. Unlocks on construction and locks on destruction.
 class SCOPED_LOCKABLE reverse_lock {
 public:
 explicit reverse_lock(UniqueLock& _lock, const MutexType& mutex, const char* _guardname, const char* _file, int _line) UNLOCK_FUNCTION(mutex) : lock(_lock), file(_file), line(_line) {
 // Ensure that mutex passed back for thread-safety analysis is indeed the original
 assert(std::addressof(mutex) == lock.mutex());

 CheckLastCritical((void*)lock.mutex(), lockname, _guardname, _file, _line);
 lock.unlock();
 LeaveCritical();
 lock.swap(templock);
 }

 ~reverse_lock() UNLOCK_FUNCTION() {
 templock.swap(lock);
 EnterCritical(lockname.c_str(), file.c_str(), line, lock.mutex());
 lock.lock();
 }

 private:
 reverse_lock(reverse_lock const&);
 reverse_lock& operator=(reverse_lock const&);

 UniqueLock& lock;
 UniqueLock templock;
 std::string lockname;
 const std::string file;
 const int line;
 };
 friend class reverse_lock;
};

// clang's thread-safety analyzer can't deal with mutex aliases, so the original mutex must be passed back to reverse_lock for thread-safety analysis (not actually used otherwise).
#define REVERSE_LOCK(g, cs) typename std::decay<decltype(g)>::type::reverse_lock UNIQUE_NAME(revlock)(g, cs, #cs, __FILE__, __LINE__)

// When locking a Mutex, require negative capability to ensure the lock is not already held.
inline Mutex& MaybeCheckNotHeld(Mutex& cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) LOCK_RETURNED(cs) { return cs; }
inline Mutex* MaybeCheckNotHeld(Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) LOCK_RETURNED(cs) { return cs; }

// When locking a GlobalMutex or RecursiveMutex, just check it is not locked in the surrounding scope.
template <typename MutexType>
inline MutexType& MaybeCheckNotHeld(MutexType& m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }
template <typename MutexType>
inline MutexType* MaybeCheckNotHeld(MutexType* m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }

#define LOCK(cs) UniqueLock UNIQUE_NAME(criticalblock)(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__)
#define LOCK2(cs1, cs2) \
 UniqueLock criticalblock1(MaybeCheckNotHeld(cs1), #cs1, __FILE__, __LINE__); \
 UniqueLock criticalblock2(MaybeCheckNotHeld(cs2), #cs2, __FILE__, __LINE__)
#define LOCK_ARGS(cs) MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__
#define TRY_LOCK(cs, name) UniqueLock name(LOCK_ARGS(cs), true)
#define WAIT_LOCK(cs, name) UniqueLock name(LOCK_ARGS(cs))

// Run code while locking a mutex. Return type deduction follows decltype(auto): returning a parenthesized local variable (e.g. `return (j);`) yields &int (a reference to a local), detectable at compile-time via -Wreturn-local-addr (gcc) / -Wreturn-stack-address (clang), both enabled by default.
#define WITH_LOCK(cs, code) (MaybeCheckNotHeld(cs), [&]() -> decltype(auto) { LOCK(cs); code; }())

#endif // TKN_SYNC_H
