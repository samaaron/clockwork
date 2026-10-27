/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

CriticalSection::CriticalSection() noexcept
{
    pthread_mutexattr_t atts;
    pthread_mutexattr_init (&atts);
    pthread_mutexattr_settype (&atts, PTHREAD_MUTEX_RECURSIVE);
   #if ! JUCE_ANDROID
    pthread_mutexattr_setprotocol (&atts, PTHREAD_PRIO_INHERIT);
   #endif
    pthread_mutex_init (&lock, &atts);
    pthread_mutexattr_destroy (&atts);
}

CriticalSection::~CriticalSection() noexcept        { pthread_mutex_destroy (&lock); }
void CriticalSection::enter() const noexcept        { pthread_mutex_lock (&lock); }
bool CriticalSection::tryEnter() const noexcept     { return pthread_mutex_trylock (&lock) == 0; }
void CriticalSection::exit() const noexcept         { pthread_mutex_unlock (&lock); }

//==============================================================================
void JUCE_CALLTYPE Thread::sleep (int millisecs)
{
    struct timespec time;
    time.tv_sec = millisecs / 1000;
    time.tv_nsec = (millisecs % 1000) * 1000000;
    nanosleep (&time, nullptr);
}

//==============================================================================
// The unix siginterrupt function is deprecated - this does the same job.
int juce_siginterrupt ([[maybe_unused]] int sig, [[maybe_unused]] int flag)
{
   #if JUCE_WASM
    return 0;
   #else
    #if JUCE_ANDROID
     using juce_sigactionflags_type = unsigned long;
    #else
     using juce_sigactionflags_type = int;
    #endif

    struct ::sigaction act;
    (void) ::sigaction (sig, nullptr, &act);

    if (flag != 0)
        act.sa_flags &= static_cast<juce_sigactionflags_type> (~SA_RESTART);
    else
        act.sa_flags |= static_cast<juce_sigactionflags_type> (SA_RESTART);

    return ::sigaction (sig, &act, nullptr);
   #endif
}

class PosixThreadAttribute
{
public:
    explicit PosixThreadAttribute (size_t stackSize)
    {
        if (valid && stackSize != 0)
            pthread_attr_setstacksize (&attr, stackSize);
    }

    ~PosixThreadAttribute()
    {
        if (valid)
            pthread_attr_destroy (&attr);
    }

    auto* get() { return valid ? &attr : nullptr; }

private:
    pthread_attr_t attr;
    bool valid { pthread_attr_init (&attr) == 0 };
};

class PosixSchedulerPriority
{
public:
    static PosixSchedulerPriority findCurrentSchedulerAndPriority()
    {
        int scheduler{};
        sched_param param{};
        pthread_getschedparam (pthread_self(), &scheduler, &param);
        return { scheduler, param.sched_priority };
    }

    static PosixSchedulerPriority getNativeSchedulerAndPriority (const Optional<Thread::RealtimeOptions>& rt,
                                                                 [[maybe_unused]] Thread::Priority prio)
    {
        const auto isRealtime = rt.hasValue();

        const auto priority = [&]
        {
            if (isRealtime)
            {
                const auto min = jmax (0, sched_get_priority_min (SCHED_RR));
                const auto max = jmax (1, sched_get_priority_max (SCHED_RR));

                return jmap (rt->getPriority(), 0, 10, min, max);
            }

            // We only use this helper if we're on an old macos/ios platform that might
            // still respect legacy pthread priorities for SCHED_OTHER.
            #if JUCE_MAC || JUCE_IOS
             const auto min = jmax (0, sched_get_priority_min (SCHED_OTHER));
             const auto max = jmax (0, sched_get_priority_max (SCHED_OTHER));

             const auto p = [prio]
             {
                 switch (prio)
                 {
                     case Thread::Priority::highest:    return 4;
                     case Thread::Priority::high:       return 3;
                     case Thread::Priority::normal:     return 2;
                     case Thread::Priority::low:        return 1;
                     case Thread::Priority::background: return 0;
                 }

                 return 3;
             }();

             if (min != 0 && max != 0)
                 return jmap (p, 0, 4, min, max);
            #endif

            return 0;
        }();

        #if JUCE_MAC || JUCE_IOS || JUCE_BSD
         const auto scheduler = SCHED_OTHER;
        #elif JUCE_LINUX
         const auto backgroundSched = prio == Thread::Priority::background ? SCHED_IDLE
                                                                           : SCHED_OTHER;
         const auto scheduler = isRealtime ? SCHED_RR : backgroundSched;
        #else
         const auto scheduler = 0;
        #endif

         return { scheduler, priority };
    }

    void apply ([[maybe_unused]] PosixThreadAttribute& attr) const
    {
        #if JUCE_LINUX || JUCE_BSD
         const struct sched_param param { getPriority() };

         pthread_attr_setinheritsched (attr.get(), PTHREAD_EXPLICIT_SCHED);
         pthread_attr_setschedpolicy (attr.get(), getScheduler());
         pthread_attr_setschedparam (attr.get(), &param);
        #endif
    }

    constexpr int getScheduler() const { return scheduler; }
    constexpr int  getPriority() const { return priority; }

private:
    constexpr PosixSchedulerPriority (int schedulerIn, int priorityIn)
        : scheduler (schedulerIn), priority (priorityIn) {}

    int scheduler;
    int priority;
};

static void* makeThreadHandle (PosixThreadAttribute& attr, void* userData, void* (*threadEntryProc) (void*))
{
    pthread_t handle = {};

    const auto status = pthread_create (&handle, attr.get(), threadEntryProc, userData);

    if (status != 0)
        return nullptr;

    pthread_detach (handle);
    return (void*) handle;
}

void Thread::closeThreadHandle()
{
    threadId = {};
    threadHandle = nullptr;
}

void JUCE_CALLTYPE Thread::setCurrentThreadName (const String& name)
{
   #if JUCE_IOS || JUCE_MAC
    JUCE_AUTORELEASEPOOL
    {
        [[NSThread currentThread] setName: juceStringToNS (name)];
    }
   #elif JUCE_LINUX || JUCE_BSD || JUCE_ANDROID
    #if (JUCE_BSD \
          || (JUCE_LINUX && (__GLIBC__ * 1000 + __GLIBC_MINOR__) >= 2012) \
          || (JUCE_ANDROID && __ANDROID_API__ >= 9))
     pthread_setname_np (pthread_self(), name.toRawUTF8());
    #else
     prctl (PR_SET_NAME, name.toRawUTF8(), 0, 0, 0);
    #endif
   #endif
}

Thread::ThreadID JUCE_CALLTYPE Thread::getCurrentThreadId()
{
    return (ThreadID) pthread_self();
}

void JUCE_CALLTYPE Thread::yield()
{
    sched_yield();
}

//==============================================================================
/* Remove this macro if you're having problems compiling the cpu affinity
   calls (the API for these has changed about quite a bit in various Linux
   versions, and a lot of distros seem to ship with obsolete versions)
*/
#if defined (CPU_ISSET) && ! defined (SUPPORT_AFFINITIES)
 #define SUPPORT_AFFINITIES 1
#endif

void JUCE_CALLTYPE Thread::setCurrentThreadAffinityMask ([[maybe_unused]] uint32 affinityMask)
{
   #if SUPPORT_AFFINITIES
    cpu_set_t affinity;
    CPU_ZERO (&affinity);

    for (int i = 0; i < 32; ++i)
    {
        if ((affinityMask & (uint32) (1 << i)) != 0)
        {
            // GCC 12 on FreeBSD complains about CPU_SET irrespective of
            // the type of the first argument
            JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wsign-conversion")
            CPU_SET ((size_t) i, &affinity);
            JUCE_END_IGNORE_WARNINGS_GCC_LIKE
        }
    }

   #if (! JUCE_ANDROID) && ((! (JUCE_LINUX || JUCE_BSD)) || ((__GLIBC__ * 1000 + __GLIBC_MINOR__) >= 2004))
    pthread_setaffinity_np (pthread_self(), sizeof (cpu_set_t), &affinity);
   #elif JUCE_ANDROID
    sched_setaffinity (gettid(), sizeof (cpu_set_t), &affinity);
   #else
    // NB: this call isn't really correct because it sets the affinity of the process,
    // (getpid) not the thread (not gettid). But it's included here as a fallback for
    // people who are using ridiculously old versions of glibc
    sched_setaffinity (getpid(), sizeof (cpu_set_t), &affinity);
   #endif

    sched_yield();

   #else
    // affinities aren't supported because either the appropriate header files weren't found,
    // or the SUPPORT_AFFINITIES macro was turned off
    jassertfalse;
   #endif
}

//==============================================================================
#if ! JUCE_WASM
bool DynamicLibrary::open (const String& name)
{
    close();
    handle = dlopen (name.isEmpty() ? nullptr : name.toUTF8().getAddress(), RTLD_LOCAL | RTLD_NOW);
    return handle != nullptr;
}

void DynamicLibrary::close()
{
    if (handle != nullptr)
    {
        dlclose (handle);
        handle = nullptr;
    }
}

void* DynamicLibrary::getFunction (const String& functionName) noexcept
{
    return handle != nullptr ? dlsym (handle, functionName.toUTF8()) : nullptr;
}

//==============================================================================
#if JUCE_LINUX || JUCE_ANDROID
static String readPosixConfigFileValue (const char* file, const char* key)
{
    StringArray lines;

    if (auto* f = fopen (file, "r"))
    {
        char line[4096];

        while (fgets (line, sizeof (line), f) != nullptr)
            lines.add (String (CharPointer_UTF8 (line)).trimEnd());

        fclose (f);
    }

    for (int i = lines.size(); --i >= 0;) // (NB - it's important that this runs in reverse order)
        if (lines[i].upToFirstOccurrenceOf (":", false, false).trim().equalsIgnoreCase (key))
            return lines[i].fromFirstOccurrenceOf (":", false, false).trim();

    return {};
}
#endif


#endif

} // namespace juce
