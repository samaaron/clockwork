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

extern HWND juce_messageWindowHandle;

#if JUCE_MODULE_AVAILABLE_juce_gui_extra
 LRESULT juce_offerEventToActiveXControl (::MSG&);
#endif

using CheckEventBlockedByModalComps = bool (*)(const MSG&);
CheckEventBlockedByModalComps isEventBlockedByModalComps = nullptr;

using SettingChangeCallbackFunc = void (*)(void);
SettingChangeCallbackFunc settingChangeCallback = nullptr;

//==============================================================================
class InternalMessageQueue
{
public:
    InternalMessageQueue()
    {
        messageWindow = std::make_unique<HiddenMessageWindow> (messageWindowName, (WNDPROC) messageWndProc);
        juce_messageWindowHandle = messageWindow->getHWND();
    }

    ~InternalMessageQueue()
    {
        juce_messageWindowHandle = nullptr;
        clearSingletonInstance();
    }

    JUCE_DECLARE_SINGLETON (InternalMessageQueue, false)

    void postMessage (MessageManager::MessageBase* message)
    {
        bool shouldTriggerMessageQueueDispatch = false;

        {
            const ScopedLock sl (lock);

            shouldTriggerMessageQueueDispatch = messageQueue.isEmpty();
            messageQueue.add (message);
        }

        if (! shouldTriggerMessageQueueDispatch)
            return;

        if (detail::RunningInUnity::state)
        {
            SendNotifyMessage (juce_messageWindowHandle, customMessageID, 0, 0);
            return;
        }

        PostMessage (juce_messageWindowHandle, customMessageID, 0, 0);
    }

    bool dispatchNextMessage (bool returnIfNoPendingMessages)
    {
        MSG m;

        if (returnIfNoPendingMessages && ! PeekMessage (&m, nullptr, 0, 0, PM_NOREMOVE))
            return false;

        if (GetMessage (&m, nullptr, 0, 0) >= 0)
        {
           #if JUCE_MODULE_AVAILABLE_juce_gui_extra
            if (juce_offerEventToActiveXControl (m) != S_FALSE)
                return true;
           #endif

            if (m.message == customMessageID && m.hwnd == juce_messageWindowHandle)
            {
                dispatchMessages();
            }
            else if (m.message == WM_QUIT)
            {
                // There is no application object to ask; the host owns quitting.
            }
            else if (isEventBlockedByModalComps == nullptr || ! isEventBlockedByModalComps (m))
            {
                if ((m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN)
                      && ! JuceWindowIdentifier::isJUCEWindow (m.hwnd))
                {
                    // if it's someone else's window being clicked on, and the focus is
                    // currently on a juce window, pass the kb focus over..
                    auto currentFocus = GetFocus();

                    if (currentFocus == nullptr || JuceWindowIdentifier::isJUCEWindow (currentFocus))
                        SetFocus (m.hwnd);
                }

                TranslateMessage (&m);
                DispatchMessage (&m);
            }
        }

        return true;
    }

private:
    //==============================================================================
    static LRESULT CALLBACK messageWndProc (HWND h, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (h == juce_messageWindowHandle)
        {
            if (message == customMessageID)
            {
                if (auto* queue = InternalMessageQueue::getInstanceWithoutCreating())
                    queue->dispatchMessages();

                return 0;
            }

            if (message == WM_SETTINGCHANGE)
                NullCheckedInvocation::invoke (settingChangeCallback);
        }

        return DefWindowProc (h, message, wParam, lParam);
    }

    static void dispatchMessage (MessageManager::MessageBase* message)
    {
        {
            message->messageCallback();
        }

        message->decReferenceCount();
    }

    void dispatchMessages()
    {
        ReferenceCountedArray<MessageManager::MessageBase> messagesToDispatch;

        {
            const ScopedLock sl (lock);

            if (messageQueue.isEmpty())
                return;

            messagesToDispatch.swapWith (messageQueue);
        }

        for (int i = 0; i < messagesToDispatch.size(); ++i)
        {
            auto message = messagesToDispatch.getUnchecked (i);
            message->incReferenceCount();
            dispatchMessage (message.get());
        }
    }

    //==============================================================================
    static constexpr unsigned int customMessageID = WM_USER + 123;
    static const TCHAR messageWindowName[];

    std::unique_ptr<HiddenMessageWindow> messageWindow;

    CriticalSection lock;
    ReferenceCountedArray<MessageManager::MessageBase> messageQueue;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InternalMessageQueue)
};

JUCE_IMPLEMENT_SINGLETON (InternalMessageQueue)

const TCHAR InternalMessageQueue::messageWindowName[] = _T("JUCEWindow");

//==============================================================================
namespace detail
{

bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages)
{
    if (auto* queue = InternalMessageQueue::getInstanceWithoutCreating())
        return queue->dispatchNextMessage (returnIfNoPendingMessages);

    return false;
}

} // namespace detail

bool MessageManager::postMessageToSystemQueue (MessageManager::MessageBase* const message)
{
    if (auto* queue = InternalMessageQueue::getInstanceWithoutCreating())
    {
        queue->postMessage (message);
        return true;
    }

    return false;
}

//==============================================================================
void MessageManager::doPlatformSpecificInitialisation()
{
    [[maybe_unused]] const auto result = OleInitialize (nullptr);
    InternalMessageQueue::getInstance();
}

void MessageManager::doPlatformSpecificShutdown()
{
    InternalMessageQueue::deleteInstance();
    OleUninitialize();
}

} // namespace juce
