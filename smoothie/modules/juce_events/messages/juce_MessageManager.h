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

//==============================================================================
/** See MessageManager::callFunctionOnMessageThread() for use of this function type. */
using MessageCallbackFunction = void* (void* userData);


//==============================================================================
/**
    This class is in charge of the application's event-dispatch loop.

    @see Message, CallbackMessage, JUCEApplication, JUCEApplicationBase

    @tags{Events}
*/
class JUCE_API  MessageManager  final
{
public:
    //==============================================================================
    /** Returns the global instance of the MessageManager. */
    static MessageManager* getInstance();

    /** Returns the global instance of the MessageManager, or nullptr if it doesn't exist. */
    static MessageManager* getInstanceWithoutCreating() noexcept;

    /** Deletes the global MessageManager instance.
        Does nothing if no instance had been created.
    */
    static void deleteInstance();

    //==============================================================================
    /** Runs the event dispatch loop until a stop message is posted.

        This method is only intended to be run by the application's startup routine,
        as it blocks, and will only return after the stopDispatchLoop() method has been used.

        @see stopDispatchLoop
    */
    void runDispatchLoop();

    /** Sends a signal that the dispatch loop should terminate.

        After this is called, the runDispatchLoop() or runDispatchLoopUntil() methods
        will be interrupted and will return.

        @see runDispatchLoop
    */
    void stopDispatchLoop();

    /** Returns true if the stopDispatchLoop() method has been called.
    */
    bool hasStopMessageBeenSent() const noexcept        { return quitMessagePosted.get() != 0; }

   #if JUCE_MODAL_LOOPS_PERMITTED
    /** Synchronously dispatches messages until a given time has elapsed.

        Returns false if a quit message has been posted by a call to stopDispatchLoop(),
        otherwise returns true.
    */
    bool runDispatchLoopUntil (int millisecondsToRunFor);
   #endif

    //==============================================================================
    /** Asynchronously invokes a function or C++11 lambda on the message thread.

        @returns  true if the message was successfully posted to the message queue,
                  or false otherwise.
    */
    static bool callAsync (std::function<void()> functionToCall);

    /** Calls a function using the message-thread.

        This can be used by any thread to cause this function to be called-back
        by the message thread. If it's the message-thread that's calling this method,
        then the function will just be called; if another thread is calling, a message
        will be posted to the queue, and this method will block until that message
        is delivered, the function is called, and the result is returned.

        Be careful not to cause any deadlocks with this! It's easy to do - e.g. if the caller
        thread has a critical section locked, which an unrelated message callback then tries to lock
        before the message thread gets round to processing this callback.

        @param callback     the function to call - its signature must be @code
                            void* myCallbackFunction (void*) @endcode
        @param userData     a user-defined pointer that will be passed to the function that gets called
        @returns            the value that the callback function returns.
    */
    void* callFunctionOnMessageThread (MessageCallbackFunction* callback, void* userData);

    /** Returns true if the caller-thread is the message thread. */
    bool isThisTheMessageThread() const noexcept;

    /** Called to tell the manager that the current thread is the one that's running the dispatch loop.

        (Best to ignore this method unless you really know what you're doing..)
        @see getCurrentMessageThread
    */
    void setCurrentThreadAsMessageThread();

    /** Returns the ID of the current message thread, as set by setCurrentThreadAsMessageThread().

        (Best to ignore this method unless you really know what you're doing..)
        @see setCurrentThreadAsMessageThread
    */
    Thread::ThreadID getCurrentMessageThread() const noexcept            { return messageThreadId; }

    /** Returns true if there's an instance of the MessageManager, and if the current thread
        is running it.
    */
    static bool existsAndIsCurrentThread() noexcept;

    //==============================================================================
    /** Internal class used as the base class for all message objects.
        You shouldn't need to use this directly - see the CallbackMessage or Message
        classes instead.
    */
    class JUCE_API  MessageBase  : public ReferenceCountedObject
    {
    public:
        MessageBase() = default;
        ~MessageBase() override = default;

        virtual void messageCallback() = 0;
        bool post();

        using Ptr = ReferenceCountedObjectPtr<MessageBase>;

        JUCE_DECLARE_NON_COPYABLE (MessageBase)
    };

    //==============================================================================
   #ifndef DOXYGEN
    // Internal methods - do not use!
    void deliverBroadcastMessage (const String&);
    ~MessageManager() noexcept;
   #endif

private:
    //==============================================================================
    MessageManager() noexcept;

    static MessageManager* instance;

    friend class MessageBase;
    class QuitMessage;
    friend class QuitMessage;

    Atomic<int> quitMessagePosted { 0 }, quitMessageReceived { 0 };
    Thread::ThreadID messageThreadId;
    mutable std::mutex messageThreadIdMutex;

    static bool postMessageToSystemQueue (MessageBase*);
    static void* exitModalLoopCallback (void*);
    static void doPlatformSpecificInitialisation();
    static void doPlatformSpecificShutdown();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MessageManager)
};


/** This macro is used to catch unsafe use of functions which expect to only be called
    on the message thread.
    It will also fail if you try to use the function before the message manager has been
    created, which could happen if you accidentally invoke it during a static constructor.
*/
#define JUCE_ASSERT_MESSAGE_THREAD \
    jassert (juce::MessageManager::existsAndIsCurrentThread());

/** This macro is used to catch unsafe use of functions which expect to not be called
    outside the lifetime of the MessageManager.
*/
#define JUCE_ASSERT_MESSAGE_MANAGER_EXISTS \
    jassert (juce::MessageManager::getInstanceWithoutCreating() != nullptr);


} // namespace juce
