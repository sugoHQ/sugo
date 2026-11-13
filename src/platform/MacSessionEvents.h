// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>

/**
 * macOS only: reports screen lock/unlock and system sleep/wake, the backend of ScreenLockWatcher
 * on macOS. Plain C++ over CoreFoundation and IOKit:
 *
 * - lock: the distributed notifications "com.apple.screenIsLocked" / "com.apple.screenIsUnlocked"
 *   (posted by the login window; undocumented, but stable for many releases);
 * - sleep: IORegisterForSystemPower on the main run loop. "Will sleep" is reported before the
 *   change is allowed (IOAllowPowerChange), "has powered on" after waking.
 *
 * The callbacks run on the main thread (the main run loop, which Qt's event loop spins).
 * Everything is deregistered in the destructor.
 */
class MacSessionEvents
{
public:
    /// `lockChanged(true)` when the screen locks, `(false)` when it unlocks;
    /// `sleepChanged(true)` before the system sleeps, `(false)` after it woke up.
    MacSessionEvents(std::function<void(bool)> lockChanged, std::function<void(bool)> sleepChanged);
    ~MacSessionEvents();
    MacSessionEvents(const MacSessionEvents &) = delete;
    MacSessionEvents &operator=(const MacSessionEvents &) = delete;

private:
    struct Callbacks; // the C callbacks, defined in the .cpp file
    friend struct Callbacks;

    std::function<void(bool)> m_lockChanged;
    std::function<void(bool)> m_sleepChanged;
    std::uint32_t m_rootPort = 0;     // io_connect_t from IORegisterForSystemPower, 0 if none
    void *m_notifyPort = nullptr;      // IONotificationPortRef
    std::uint32_t m_notifier = 0;     // io_object_t
};
