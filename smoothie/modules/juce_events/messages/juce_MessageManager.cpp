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

MessageManager::MessageManager() noexcept
  : messageThreadId (Thread::getCurrentThreadId())
{
    JUCE_VERSION_ID

}

MessageManager::~MessageManager() noexcept
{
    doPlatformSpecificShutdown();

    jassert (instance == this);
    instance = nullptr;  // do this last in case this instance is still needed by doPlatformSpecificShutdown()
}

MessageManager* MessageManager::instance = nullptr;

MessageManager* MessageManager::getInstance()
{
    if (instance == nullptr)
    {
        instance = new MessageManager();
        doPlatformSpecificInitialisation();
    }

    return instance;
}

MessageManager* MessageManager::getInstanceWithoutCreating() noexcept
{
    return instance;
}

void MessageManager::deleteInstance()
{
    deleteAndZero (instance);
}

//==============================================================================
bool MessageManager::MessageBase::post()
{
    auto* mm = MessageManager::instance;

    if (mm == nullptr || mm->quitMessagePosted.get() != 0 || ! postMessageToSystemQueue (this))
    {
        Ptr deleter (this); // (this will delete messages that were just created with a 0 ref count)
        return false;
    }

    return true;
}

//==============================================================================
#if ! (JUCE_MAC || JUCE_IOS || JUCE_ANDROID)
// implemented in platform-specific code (juce_Messaging_linux.cpp and juce_Messaging_windows.cpp)
namespace detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
} // namespace detail

class MessageManager::QuitMessage final : public MessageManager::MessageBase
{
public:
    QuitMessage() {}

    void messageCallback() override
    {
        if (auto* mm = MessageManager::instance)
            mm->quitMessageReceived = true;
    }

    JUCE_DECLARE_NON_COPYABLE (QuitMessage)
};

void MessageManager::runDispatchLoop()
{
    jassert (isThisTheMessageThread()); // must only be called by the message thread

    while (quitMessageReceived.get() == 0)
    {
        {
            if (! detail::dispatchNextMessageOnSystemQueue (false))
                Thread::sleep (1);
        }
    }
}

void MessageManager::stopDispatchLoop()
{
    (new QuitMessage())->post();
    quitMessagePosted = true;
}

#if JUCE_MODAL_LOOPS_PERMITTED
bool MessageManager::runDispatchLoopUntil (int millisecondsToRunFor)
{
    jassert (isThisTheMessageThread()); // must only be called by the message thread

    auto endTime = Time::currentTimeMillis() + millisecondsToRunFor;

    while (quitMessageReceived.get() == 0)
    {
        {
            if (! detail::dispatchNextMessageOnSystemQueue (millisecondsToRunFor >= 0))
                Thread::sleep (1);
        }

        if (millisecondsToRunFor >= 0 && Time::currentTimeMillis() >= endTime)
            break;
    }

    return quitMessageReceived.get() == 0;
}
#endif

#endif

//==============================================================================
class AsyncFunctionCallback final : public MessageManager::MessageBase
{
public:
    AsyncFunctionCallback (MessageCallbackFunction* const f, void* const param)
        : func (f), parameter (param)
    {}

    void messageCallback() override
    {
        result = (*func) (parameter);
        finished.signal();
    }

    WaitableEvent finished;
    std::atomic<void*> result { nullptr };

private:
    MessageCallbackFunction* const func;
    void* const parameter;

    JUCE_DECLARE_NON_COPYABLE (AsyncFunctionCallback)
};

void* MessageManager::callFunctionOnMessageThread (MessageCallbackFunction* func, void* parameter)
{
    if (isThisTheMessageThread())
        return func (parameter);

    const ReferenceCountedObjectPtr<AsyncFunctionCallback> message (new AsyncFunctionCallback (func, parameter));

    if (message->post())
    {
        message->finished.wait();
        return message->result.load();
    }

    jassertfalse; // the OS message queue failed to send the message!
    return nullptr;
}

bool MessageManager::callAsync (std::function<void()> fn)
{
    struct AsyncCallInvoker final : public MessageBase
    {
        AsyncCallInvoker (std::function<void()> f) : callback (std::move (f)) {}
        void messageCallback() override  { callback(); }
        std::function<void()> callback;
    };

    return (new AsyncCallInvoker (std::move (fn)))->post();
}

//==============================================================================
bool MessageManager::isThisTheMessageThread() const noexcept
{
    const std::lock_guard<std::mutex> lock { messageThreadIdMutex };

    return Thread::getCurrentThreadId() == messageThreadId;
}

void MessageManager::setCurrentThreadAsMessageThread()
{
    auto thisThread = Thread::getCurrentThreadId();

    const std::lock_guard<std::mutex> lock { messageThreadIdMutex };

    if (std::exchange (messageThreadId, thisThread) != thisThread)
    {
       #if JUCE_WINDOWS
        // This is needed on windows to make sure the message window is created by this thread
        doPlatformSpecificShutdown();
        doPlatformSpecificInitialisation();
       #endif
    }
}

bool MessageManager::existsAndIsCurrentThread() noexcept
{
    if (auto i = getInstanceWithoutCreating())
        return i->isThisTheMessageThread();

    return false;
}

//==============================================================================
JUCE_API void JUCE_CALLTYPE initialiseJuce_GUI()
{
    JUCE_AUTORELEASEPOOL
    {
        MessageManager::getInstance();
    }
}

JUCE_API void JUCE_CALLTYPE shutdownJuce_GUI()
{
    JUCE_AUTORELEASEPOOL
    {
        DeletedAtShutdown::deleteAll();
        MessageManager::deleteInstance();
    }
}

} // namespace juce
