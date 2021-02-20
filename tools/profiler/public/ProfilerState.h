/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This header contains most functions that give information about the Profiler:
// Whether it is active or not, paused, the selected features, and some generic
// process and thread information.
// It is safe to include unconditionally, but uses of structs and functions must
// be guarded by `#ifdef MOZ_GECKO_PROFILER`.

#ifndef ProfilerState_h
#define ProfilerState_h

#ifdef MOZ_GECKO_PROFILER

#  include "mozilla/Atomics.h"
#  include "mozilla/Maybe.h"

#  include <stdint.h>

//---------------------------------------------------------------------------
// Profiler features
//---------------------------------------------------------------------------

// Higher-order macro containing all the feature info in one place. Define
// |MACRO| appropriately to extract the relevant parts. Note that the number
// values are used internally only and so can be changed without consequence.
// Any changes to this list should also be applied to the feature list in
// toolkit/components/extensions/schemas/geckoProfiler.json.
#  define PROFILER_FOR_EACH_FEATURE(MACRO)                                     \
    MACRO(0, "java", Java, "Profile Java code, Android only")                  \
                                                                               \
    MACRO(1, "js", JS,                                                         \
          "Get the JS engine to expose the JS stack to the profiler")          \
                                                                               \
    /* The DevTools profiler doesn't want the native addresses. */             \
    MACRO(2, "leaf", Leaf, "Include the C++ leaf node if not stackwalking")    \
                                                                               \
    MACRO(3, "mainthreadio", MainThreadIO, "Add main thread file I/O")         \
                                                                               \
    MACRO(4, "fileio", FileIO,                                                 \
          "Add file I/O from all profiled threads, implies mainthreadio")      \
                                                                               \
    MACRO(5, "fileioall", FileIOAll,                                           \
          "Add file I/O from all threads, implies fileio")                     \
                                                                               \
    MACRO(6, "noiostacks", NoIOStacks,                                         \
          "File I/O markers do not capture stacks, to reduce overhead")        \
                                                                               \
    MACRO(7, "screenshots", Screenshots,                                       \
          "Take a snapshot of the window on every composition")                \
                                                                               \
    MACRO(8, "seqstyle", SequentialStyle,                                      \
          "Disable parallel traversal in styling")                             \
                                                                               \
    MACRO(9, "stackwalk", StackWalk,                                           \
          "Walk the C++ stack, not available on all platforms")                \
                                                                               \
    MACRO(10, "tasktracer", TaskTracer,                                        \
          "Start profiling with feature TaskTracer")                           \
                                                                               \
    MACRO(11, "threads", Threads, "Profile the registered secondary threads")  \
                                                                               \
    MACRO(12, "jstracer", JSTracer, "Enable tracing of the JavaScript engine") \
                                                                               \
    MACRO(13, "jsallocations", JSAllocations,                                  \
          "Have the JavaScript engine track allocations")                      \
                                                                               \
    MACRO(14, "nostacksampling", NoStackSampling,                              \
          "Disable all stack sampling: Cancels \"js\", \"leaf\", "             \
          "\"stackwalk\" and labels")                                          \
                                                                               \
    MACRO(15, "preferencereads", PreferenceReads,                              \
          "Track when preferences are read")                                   \
                                                                               \
    MACRO(16, "nativeallocations", NativeAllocations,                          \
          "Collect the stacks from a smaller subset of all native "            \
          "allocations, biasing towards collecting larger allocations")        \
                                                                               \
    MACRO(17, "ipcmessages", IPCMessages,                                      \
          "Have the IPC layer track cross-process messages")                   \
                                                                               \
    MACRO(18, "audiocallbacktracing", AudioCallbackTracing,                    \
          "Audio callback tracing")                                            \
                                                                               \
    MACRO(19, "cpu", CPUUtilization, "CPU utilization")

struct ProfilerFeature {
#  define DECLARE(n_, str_, Name_, desc_)                     \
    static constexpr uint32_t Name_ = (1u << n_);             \
    static constexpr bool Has##Name_(uint32_t aFeatures) {    \
      return aFeatures & Name_;                               \
    }                                                         \
    static constexpr void Set##Name_(uint32_t& aFeatures) {   \
      aFeatures |= Name_;                                     \
    }                                                         \
    static constexpr void Clear##Name_(uint32_t& aFeatures) { \
      aFeatures &= ~Name_;                                    \
    }

  // Define a bitfield constant, a getter, and two setters for each feature.
  PROFILER_FOR_EACH_FEATURE(DECLARE)

#  undef DECLARE
};

namespace mozilla {
namespace profiler {
namespace detail {

// RacyFeatures is only defined in this header file so that its methods can
// be inlined into profiler_is_active(). Please do not use anything from the
// detail namespace outside the profiler.

// Within the profiler's code, the preferred way to check profiler activeness
// and features is via ActivePS(). However, that requires locking gPSMutex.
// There are some hot operations where absolute precision isn't required, so we
// duplicate the activeness/feature state in a lock-free manner in this class.
class RacyFeatures {
 public:
  static void SetActive(uint32_t aFeatures) {
    sActiveAndFeatures = Active | aFeatures;
  }

  static void SetInactive() { sActiveAndFeatures = 0; }

  static void SetPaused() { sActiveAndFeatures |= Paused; }

  static void SetUnpaused() { sActiveAndFeatures &= ~Paused; }

  static void SetSamplingPaused() { sActiveAndFeatures |= SamplingPaused; }

  static void SetSamplingUnpaused() { sActiveAndFeatures &= ~SamplingPaused; }

  static mozilla::Maybe<uint32_t> FeaturesIfActive() {
    if (uint32_t af = sActiveAndFeatures; af & Active) {
      // Active, remove the Active&Paused bits to get all features.
      return Some(af & ~(Active | Paused | SamplingPaused));
    }
    return Nothing();
  }

  static mozilla::Maybe<uint32_t> FeaturesIfActiveAndUnpaused() {
    if (uint32_t af = sActiveAndFeatures; (af & (Active | Paused)) == Active) {
      // Active but not fully paused, remove the Active and sampling-paused bits
      // to get all features.
      return Some(af & ~(Active | SamplingPaused));
    }
    return Nothing();
  }

  static bool IsActive() { return uint32_t(sActiveAndFeatures) & Active; }

  static bool IsActiveWithFeature(uint32_t aFeature) {
    uint32_t af = sActiveAndFeatures;  // copy it first
    return (af & Active) && (af & aFeature);
  }

  // True if profiler is active, and not fully paused.
  // Note that periodic sampling *could* be paused!
  static bool IsActiveAndUnpaused() {
    uint32_t af = sActiveAndFeatures;  // copy it first
    return (af & Active) && !(af & Paused);
  }

  // True if profiler is active, and sampling is not paused (though generic
  // `SetPaused()` or specific `SetSamplingPaused()`).
  static bool IsActiveAndSamplingUnpaused() {
    uint32_t af = sActiveAndFeatures;  // copy it first
    return (af & Active) && !(af & (Paused | SamplingPaused));
  }

 private:
  static constexpr uint32_t Active = 1u << 31;
  static constexpr uint32_t Paused = 1u << 30;
  static constexpr uint32_t SamplingPaused = 1u << 29;

// Ensure Active/Paused don't overlap with any of the feature bits.
#  define NO_OVERLAP(n_, str_, Name_, desc_)                \
    static_assert(ProfilerFeature::Name_ != SamplingPaused, \
                  "bad feature value");

  PROFILER_FOR_EACH_FEATURE(NO_OVERLAP);

#  undef NO_OVERLAP

  // We combine the active bit with the feature bits so they can be read or
  // written in a single atomic operation. Accesses to this atomic are not
  // recorded by web replay as they may occur at non-deterministic points.
  static mozilla::Atomic<uint32_t, mozilla::MemoryOrdering::Relaxed>
      sActiveAndFeatures;
};

bool IsThreadBeingProfiled();
bool IsThreadRegistered();

}  // namespace detail
}  // namespace profiler
}  // namespace mozilla

//---------------------------------------------------------------------------
// Get information from the profiler
//---------------------------------------------------------------------------

// Is the profiler active? Note: the return value of this function can become
// immediately out-of-date. E.g. the profile might be active but then
// profiler_stop() is called immediately afterward. One common and reasonable
// pattern of usage is the following:
//
//   if (profiler_is_active()) {
//     ExpensiveData expensiveData = CreateExpensiveData();
//     PROFILER_OPERATION(expensiveData);
//   }
//
// where PROFILER_OPERATION is a no-op if the profiler is inactive. In this
// case the profiler_is_active() check is just an optimization -- it prevents
// us calling CreateExpensiveData() unnecessarily in most cases, but the
// expensive data will end up being created but not used if another thread
// stops the profiler between the CreateExpensiveData() and PROFILER_OPERATION
// calls.
inline bool profiler_is_active() {
  return mozilla::profiler::detail::RacyFeatures::IsActive();
}

// Same as profiler_is_active(), but with the same extra checks that determine
// if the profiler would currently store markers. So this should be used before
// doing some potentially-expensive work that's used in a marker. E.g.:
//
//   if (profiler_can_accept_markers()) {
//     ExpensiveMarkerPayload expensivePayload = CreateExpensivePayload();
//     BASE_PROFILER_ADD_MARKER_WITH_PAYLOAD(name, OTHER, expensivePayload);
//   }
inline bool profiler_can_accept_markers() {
  return mozilla::profiler::detail::RacyFeatures::IsActiveAndUnpaused();
}

// Is the profiler active, and is the current thread being profiled?
// (Same caveats and recommented usage as profiler_is_active().)
inline bool profiler_thread_is_being_profiled() {
  return profiler_is_active() &&
         mozilla::profiler::detail::IsThreadBeingProfiled();
}

// During profiling, if the current thread is registered, return true
// (regardless of whether it is actively being profiled).
// (Same caveats and recommented usage as profiler_is_active().)
inline bool profiler_is_active_and_thread_is_registered() {
  return profiler_is_active() &&
         mozilla::profiler::detail::IsThreadRegistered();
}

// Is the profiler active and paused? Returns false if the profiler is inactive.
bool profiler_is_paused();

// Is the profiler active and sampling is paused? Returns false if the profiler
// is inactive.
bool profiler_is_sampling_paused();

// Is the current thread sleeping?
bool profiler_thread_is_sleeping();

// Get all the features supported by the profiler that are accepted by
// profiler_start(). The result is the same whether the profiler is active or
// not.
uint32_t profiler_get_available_features();

// Returns the full feature set if the profiler is active.
// Note: the return value can become immediately out-of-date, much like the
// return value of profiler_is_active().
inline mozilla::Maybe<uint32_t> profiler_features_if_active() {
  return mozilla::profiler::detail::RacyFeatures::FeaturesIfActive();
}

// Returns the full feature set if the profiler is active and unpaused.
// Note: the return value can become immediately out-of-date, much like the
// return value of profiler_is_active().
inline mozilla::Maybe<uint32_t> profiler_features_if_active_and_unpaused() {
  return mozilla::profiler::detail::RacyFeatures::FeaturesIfActiveAndUnpaused();
}

// Check if a profiler feature (specified via the ProfilerFeature type) is
// active. Returns false if the profiler is inactive. Note: the return value
// can become immediately out-of-date, much like the return value of
// profiler_is_active().
bool profiler_feature_active(uint32_t aFeature);

// Get the current process's ID.
int profiler_current_process_id();

// Get the current thread's ID.
int profiler_current_thread_id();

// Statically initialized to 0, then set once from profiler_init(), which should
// be called from the main thread before any other use of the profiler.
extern int scProfilerMainThreadId;

inline int profiler_main_thread_id() { return scProfilerMainThreadId; }

inline bool profiler_is_main_thread() {
  return profiler_current_thread_id() == profiler_main_thread_id();
}

// Returns true if any of the profiler mutexes are currently locked *on the
// current thread*. This may be used by re-entrant code that may call profiler
// functions while the same of a different profiler mutex is locked, which could
// deadlock.
bool profiler_is_locked_on_current_thread();

#endif  // !MOZ_GECKO_PROFILER

#endif  // ProfilerState_h
