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

using AppFocusChangeCallback = void (*)();
AppFocusChangeCallback appFocusChangeCallback = nullptr;

using CheckEventBlockedByModalComps = bool (*)(NSEvent*);
CheckEventBlockedByModalComps isEventBlockedByModalComps = nullptr;

using MenuTrackingChangedCallback = void (*)(bool);
MenuTrackingChangedCallback menuTrackingChangedCallback = nullptr;

//==============================================================================
struct AppDelegateClass final : public ObjCClass<NSObject>
{
    AppDelegateClass()  : ObjCClass ("JUCEAppDelegate_")
    {
        addMethod (@selector (applicationDidBecomeActive:), [] (id /*self*/, SEL, NSNotification*)  { focusChanged(); });
        addMethod (@selector (applicationDidResignActive:), [] (id /*self*/, SEL, NSNotification*)  { focusChanged(); });
        addMethod (@selector (applicationWillUnhide:),      [] (id /*self*/, SEL, NSNotification*)  { focusChanged(); });

        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wundeclared-selector")
        addMethod (@selector (mainMenuTrackingBegan:), [] (id /*self*/, SEL, NSNotification*)
        {
            NullCheckedInvocation::invoke (menuTrackingChangedCallback, true);
        });

        addMethod (@selector (mainMenuTrackingEnded:), [] (id /*self*/, SEL, NSNotification*)
        {
            NullCheckedInvocation::invoke (menuTrackingChangedCallback, false);
        });

        // (used as a way of running a dummy thread)
        addMethod (@selector (dummyMethod), [] (id /*self*/, SEL) {});
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE

       #if JUCE_PUSH_NOTIFICATIONS
        //==============================================================================
        addIvar<NSObject<NSApplicationDelegate, NSUserNotificationCenterDelegate>*> ("pushNotificationsDelegate");

        addMethod (@selector (applicationDidFinishLaunching:), [] (id self, SEL, NSNotification* notification)
        {
            if (notification.userInfo != nil)
            {
                JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
                // NSUserNotification is deprecated from macOS 11, but there doesn't seem to be a
                // replacement for NSApplicationLaunchUserNotificationKey returning a non-deprecated type
                NSUserNotification* userNotification = notification.userInfo[NSApplicationLaunchUserNotificationKey];
                JUCE_END_IGNORE_WARNINGS_GCC_LIKE

                JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wnullable-to-nonnull-conversion")
                if (userNotification != nil && userNotification.userInfo != nil)
                    [self application: [NSApplication sharedApplication] didReceiveRemoteNotification: userNotification.userInfo];
                JUCE_END_IGNORE_WARNINGS_GCC_LIKE
            }
        });

        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wundeclared-selector")
        addMethod (@selector (setPushNotificationsDelegate:), [] (id self, SEL, NSObject<NSApplicationDelegate, NSUserNotificationCenterDelegate>* delegate)
        {
            object_setInstanceVariable (self, "pushNotificationsDelegate", delegate);
        });
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE

        addMethod (@selector (application:didRegisterForRemoteNotificationsWithDeviceToken:), [] (id self, SEL, NSApplication* application, NSData* deviceToken)
        {
            auto* delegate = getPushNotificationsDelegate (self);

            SEL selector = @selector (application:didRegisterForRemoteNotificationsWithDeviceToken:);

            if (delegate != nil && [delegate respondsToSelector: selector])
            {
                NSInvocation* invocation = [NSInvocation invocationWithMethodSignature: [delegate methodSignatureForSelector: selector]];
                [invocation setSelector: selector];
                [invocation setTarget: delegate];
                [invocation setArgument: &application atIndex:2];
                [invocation setArgument: &deviceToken atIndex:3];

                [invocation invoke];
            }
        });

        addMethod (@selector (application:didFailToRegisterForRemoteNotificationsWithError:), [] (id self, SEL, NSApplication* application, NSError* error)
        {
            auto* delegate = getPushNotificationsDelegate (self);

            SEL selector =  @selector (application:didFailToRegisterForRemoteNotificationsWithError:);

            if (delegate != nil && [delegate respondsToSelector: selector])
            {
                NSInvocation* invocation = [NSInvocation invocationWithMethodSignature: [delegate methodSignatureForSelector: selector]];
                [invocation setSelector: selector];
                [invocation setTarget: delegate];
                [invocation setArgument: &application atIndex:2];
                [invocation setArgument: &error       atIndex:3];

                [invocation invoke];
            }
        });

        addMethod (@selector (application:didReceiveRemoteNotification:), [] (id self, SEL, NSApplication* application, NSDictionary* userInfo)
        {
            auto* delegate = getPushNotificationsDelegate (self);

            SEL selector =  @selector (application:didReceiveRemoteNotification:);

            if (delegate != nil && [delegate respondsToSelector: selector])
            {
                NSInvocation* invocation = [NSInvocation invocationWithMethodSignature: [delegate methodSignatureForSelector: selector]];
                [invocation setSelector: selector];
                [invocation setTarget: delegate];
                [invocation setArgument: &application atIndex:2];
                [invocation setArgument: &userInfo    atIndex:3];

                [invocation invoke];
            }
        });
       #endif

        registerClass();
    }

private:
    static void focusChanged()
    {
        if (appFocusChangeCallback != nullptr)
            (*appFocusChangeCallback)();
    }

    static String quotedIfContainsSpaces (NSString* file)
    {
        String s (nsStringToJuce (file));
        s = s.unquoted().replace ("\"", "\\\"");

        if (s.containsChar (' '))
            s = s.quoted();

        return s;
    }

    //==============================================================================
   #if JUCE_PUSH_NOTIFICATIONS
    static NSObject<NSApplicationDelegate, NSUserNotificationCenterDelegate>* getPushNotificationsDelegate (id self)
    {
        return getIvar<NSObject<NSApplicationDelegate, NSUserNotificationCenterDelegate>*> (self, "pushNotificationsDelegate");
    }
   #endif
};

// This is declared at file scope, so that it's guaranteed to be
// constructed before and destructed after `appDelegate` (below)
static AppDelegateClass appDelegateClass;

//==============================================================================
struct AppDelegate
{
public:
    AppDelegate()
    {
        delegate = [appDelegateClass.createInstance() init];

        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wundeclared-selector")
        [center addObserver: delegate selector: @selector (mainMenuTrackingBegan:)
                       name: NSMenuDidBeginTrackingNotification object: nil];
        [center addObserver: delegate selector: @selector (mainMenuTrackingEnded:)
                       name: NSMenuDidEndTrackingNotification object: nil];
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE

        [center addObserver: delegate selector: @selector (applicationDidResignActive:)
                       name: NSApplicationDidResignActiveNotification object: NSApp];

        [center addObserver: delegate selector: @selector (applicationDidBecomeActive:)
                       name: NSApplicationDidBecomeActiveNotification object: NSApp];

        [center addObserver: delegate selector: @selector (applicationWillUnhide:)
                       name: NSApplicationWillUnhideNotification object: NSApp];
    }

    ~AppDelegate()
    {
        [[NSRunLoop currentRunLoop] cancelPerformSelectorsWithTarget: delegate];
        [[NSNotificationCenter defaultCenter] removeObserver: delegate];

        [delegate release];
    }

    MessageQueue messageQueue;
    id delegate;
};

//==============================================================================
void MessageManager::runDispatchLoop()
{
    if (quitMessagePosted.get() == 0) // check that the quit message wasn't already posted..
    {
        JUCE_AUTORELEASEPOOL
        {
            // must only be called by the message thread!
            jassert (isThisTheMessageThread());

            [NSApp run];
        }
    }
}

static void shutdownNSApp()
{
    [NSApp stop: nil];
    [NSEvent startPeriodicEventsAfterDelay: 0  withPeriod: 0.1];
}

void MessageManager::stopDispatchLoop()
{
    if (isThisTheMessageThread())
    {
        quitMessagePosted = true;
        shutdownNSApp();
    }
    else
    {
        struct QuitCallback final : public CallbackMessage
        {
            QuitCallback() {}
            void messageCallback() override    { MessageManager::getInstance()->stopDispatchLoop(); }
        };

        (new QuitCallback())->post();
    }
}

#if JUCE_MODAL_LOOPS_PERMITTED
bool MessageManager::runDispatchLoopUntil (int millisecondsToRunFor)
{
    jassert (millisecondsToRunFor >= 0);
    jassert (isThisTheMessageThread()); // must only be called by the message thread

    auto endTime = Time::currentTimeMillis() + millisecondsToRunFor;

    while (quitMessagePosted.get() == 0)
    {
        JUCE_AUTORELEASEPOOL
        {
            auto msRemaining = endTime - Time::currentTimeMillis();

            if (msRemaining <= 0)
                break;

            CFRunLoopRunInMode (kCFRunLoopDefaultMode, jmin (1.0, msRemaining * 0.001), true);

            if (NSEvent* e = [NSApp nextEventMatchingMask: NSEventMaskAny
                                                untilDate: [NSDate dateWithTimeIntervalSinceNow: 0.001]
                                                   inMode: NSDefaultRunLoopMode
                                                  dequeue: YES])
                if (isEventBlockedByModalComps == nullptr || ! (*isEventBlockedByModalComps) (e))
                    [NSApp sendEvent: e];
        }
    }

    return quitMessagePosted.get() == 0;
}
#endif

//==============================================================================
void initialiseNSApplication();
void initialiseNSApplication()
{
    JUCE_AUTORELEASEPOOL
    {
        [NSApplication sharedApplication];
    }
}

static std::unique_ptr<AppDelegate> appDelegate;

void MessageManager::doPlatformSpecificInitialisation()
{
    if (appDelegate == nil)
        appDelegate.reset (new AppDelegate());
}

void MessageManager::doPlatformSpecificShutdown()
{
    appDelegate = nullptr;
}

bool MessageManager::postMessageToSystemQueue (MessageBase* message)
{
    jassert (appDelegate != nil);
    appDelegate->messageQueue.post (message);
    return true;
}

} // namespace juce
