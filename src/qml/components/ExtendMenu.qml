// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import stream.sugo.app

/// More time, over the composer's button: 2, 5 or 10 minutes for the phase under way, or for the
/// one that has just ended (TimerEngine.extend()).
SMenu {
    id: extendMenu
    objectName: "extendMenu"

    side: "top"
    align: "end"

    /// What the minutes go to: the phase under way, or the one that has just ended.
    readonly property string heading: TimerEngine.active ? i18n("Add time")
                                    : TimerEngine.finishedPhase === TimerEngine.Phase.Work ? i18n("Keep working")
                                    : i18n("Make the break longer")

    SMenuHeader {
        text: extendMenu.heading
    }
    SMenuItem {
        objectName: "extendEntry-2"
        text: i18np("%1 minute", "%1 minutes", 2)
        onTriggered: TimerEngine.extend(2 * 60)
    }
    SMenuItem {
        objectName: "extendEntry-5"
        text: i18np("%1 minute", "%1 minutes", 5)
        onTriggered: TimerEngine.extend(5 * 60)
    }
    SMenuItem {
        objectName: "extendEntry-10"
        text: i18np("%1 minute", "%1 minutes", 10)
        onTriggered: TimerEngine.extend(10 * 60)
    }
}
