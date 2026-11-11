// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/// How much was done: the period's figures (the total and how it compares, sessions, breaks,
/// the streak), one chart, where the time went, and the task notes.
QQC2.Page {
    id: page

    title: i18n("Statistics")
    background: null

    /// Laid out with real sizes (read by the smoke test).
    readonly property bool contentLaidOut: column.width > 0 && chart.width > 0

    // The model last caught up with the clock up to a few minutes ago: show "now" as it is.
    Component.onCompleted: StatsModel.checkDateChange()

    readonly property var periods: [
        { id: "day", text: i18n("Day"), value: StatsModel.Period.Day },
        { id: "week", text: i18n("Week"), value: StatsModel.Period.Week },
        { id: "month", text: i18n("Month"), value: StatsModel.Period.Month },
        { id: "year", text: i18n("Year"), value: StatsModel.Period.Year }
    ]
    readonly property string periodId: periods.find(p => p.value === StatsModel.period)?.id ?? "week"

    readonly property var breakdowns: [
        { id: "timer", text: i18n("Timer"), value: StatsModel.BreakdownMode.ByTimer },
        { id: "category", text: i18n("Category"), value: StatsModel.BreakdownMode.ByCategory },
        { id: "task", text: i18n("Task"), value: StatsModel.BreakdownMode.ByTask }
    ]
    readonly property string breakdownId: breakdowns.find(b => b.value === StatsModel.breakdownMode)?.id ?? "timer"


    /// "+35% vs last week", or nothing to compare with.
    readonly property string comparison: {
        const before = StatsModel.previousWorkSeconds;
        if (before <= 0) {
            return "";
        }
        const change = Math.round((StatsModel.workSeconds - before) * 100 / before);
        const sign = change > 0 ? "+" : "";
        switch (StatsModel.period) {
        case StatsModel.Period.Day:
            return i18nc("change against the day before", "%1%2% vs the day before", sign, change);
        case StatsModel.Period.Week:
            return i18nc("change against the week before", "%1%2% vs the week before", sign, change);
        case StatsModel.Period.Month:
            return i18nc("change against the month before", "%1%2% vs the month before", sign, change);
        default:
            return i18nc("change against the year before", "%1%2% vs the year before", sign, change);
        }
    }
    /// One cell of the figures strip: a number, its label, and a quieter line under them.
    /// `index` places it in the strip, for the lines between the cells.
    component Figure: Item {
        id: figure
        property int index
        property string value
        /// A word after the value, in the same size ("days").
        property string unit
        property string label
        property string note
        readonly property int columns: figures.columns
        Layout.fillWidth: true
        // The first cell is a little wider, for the total and its comparison.
        Layout.preferredWidth: !page.compact && index === 0 ? 7 : 5
        implicitHeight: figureColumn.implicitHeight + Theme.spaceLg * 2

        // The lines between the cells.
        Rectangle {
            visible: figure.index % figure.columns > 0
            width: Theme.hairline
            height: parent.height
            color: Theme.border
        }
        Rectangle {
            visible: figure.index >= figure.columns
            width: parent.width
            height: Theme.hairline
            color: Theme.border
        }

        ColumnLayout {
            id: figureColumn
            anchors.fill: parent
            anchors.topMargin: Theme.spaceLg
            anchors.bottomMargin: Theme.spaceLg
            anchors.leftMargin: Theme.spaceLg + Theme.hairline * 2
            anchors.rightMargin: Theme.spaceLg + Theme.hairline * 2
            spacing: Theme.hairline * 2

            Text {
                text: figure.unit.length > 0 ? figure.value + " " + figure.unit : figure.value
                color: Theme.foreground
                font.pixelSize: Theme.fontHeading
                font.weight: Font.Medium
                font.letterSpacing: -Theme.hairline / 2
                font.features: { "tnum": 1 }
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Text {
                text: figure.label
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // Faded rather than hidden: its line stays, so nothing below moves when a period
            // has a note and the next has none.
            Text {
                opacity: figure.note.length > 0 ? 1 : 0
                text: figure.note.length > 0 ? figure.note : " "
                color: Theme.subtle
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }

    /// A bordered surface with a quiet title, and maybe something at the right of it.
    component Panel: Rectangle {
        id: panel
        default property alias content: panelColumn.data
        property string title
        property alias headerExtra: panelExtra.data
        /// Panels side by side give their headers the same height, so their rows line up.
        property real headerHeight: 0
        implicitHeight: panelColumn.implicitHeight + Theme.spaceLg * 2
        radius: Theme.radiusCard
        color: Theme.card
        border.width: Theme.hairline
        border.color: Theme.border

        ColumnLayout {
            id: panelColumn
            anchors.fill: parent
            anchors.topMargin: Theme.spaceLg
            anchors.bottomMargin: Theme.spaceLg
            anchors.leftMargin: Theme.spaceLg + Theme.hairline * 2
            anchors.rightMargin: Theme.spaceLg + Theme.hairline * 2
            spacing: Theme.spaceMd + Theme.hairline * 2

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumHeight: panel.headerHeight
                spacing: Theme.spaceMd
                Text {
                    text: panel.title
                    color: Theme.foreground
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                RowLayout {
                    id: panelExtra
                    spacing: Theme.spaceXs
                }
            }
        }
    }

    /// A narrow window: smaller margins, the figures two by two, the panels one under the other.
    readonly property bool compact: width < Theme.compactBreakpoint
    readonly property real paddingX: compact ? Theme.pagePaddingCompact : Theme.pagePaddingX
    readonly property real paddingY: compact ? Theme.pagePaddingCompact : Theme.pagePaddingY

    Flickable {
        id: flickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + page.paddingY * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        ColumnLayout {
            id: column
            x: (flickable.width - width) / 2
            y: page.paddingY
            width: Math.min(flickable.width - page.paddingX * 2, Theme.statsMaxWidth)
            spacing: page.compact ? Theme.spaceLg : Theme.spaceXl

            // Which stretch of time: back, the period, forward; and how long a stretch.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                SButton {
                    kind: "ghost"
                    iconOnly: true
                    iconName: "chevron-left"
                    text: i18n("Previous")
                    implicitHeight: Theme.iconButton
                    onClicked: StatsModel.previousPeriod()
                }
                // Its own width at most (the spacer takes the rest), cut short when the row is
                // too narrow for it and the period buttons.
                Text {
                    text: StatsModel.title
                    color: Theme.foreground
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: implicitWidth
                }
                SButton {
                    kind: "ghost"
                    iconOnly: true
                    iconName: "chevron-right"
                    text: i18n("Next")
                    implicitHeight: Theme.iconButton
                    enabled: StatsModel.canGoNext
                    onClicked: StatsModel.nextPeriod()
                }
                SButton {
                    visible: !StatsModel.isCurrent
                    kind: "ghost"
                    text: i18n("Today")
                    implicitHeight: Theme.iconButton
                    onClicked: StatsModel.today()
                }
                Item {
                    Layout.fillWidth: true
                }
                Segmented {
                    model: page.periods
                    current: page.periodId
                    onActivated: id => StatsModel.period = page.periods.find(p => p.id === id).value
                }
            }

            // The total and how it compares, the sessions, the breaks and the streak, in one strip
            // (two by two in a narrow window).
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: figures.implicitHeight
                radius: Theme.radiusCard
                color: Theme.card
                border.width: Theme.hairline
                border.color: Theme.border

                GridLayout {
                    id: figures
                    anchors.left: parent.left
                    anchors.right: parent.right
                    columns: page.compact ? 2 : 4
                    columnSpacing: 0
                    rowSpacing: 0

                    Figure {
                        index: 0
                        value: StatsModel.formatDuration(StatsModel.workSeconds)
                        label: i18n("Focused")
                        note: page.comparison
                    }
                    Figure {
                        index: 1
                        value: String(StatsModel.completedWorkSessions)
                        label: i18n("Sessions")
                        note: StatsModel.interruptedWorkSessions > 0
                              ? i18np("+%1 interrupted", "+%1 interrupted", StatsModel.interruptedWorkSessions) : ""
                    }
                    Figure {
                        index: 2
                        value: StatsModel.formatDuration(StatsModel.breakSeconds)
                        label: i18n("Breaks")
                    }
                    // Days in a row, whatever the period shown.
                    Figure {
                        index: 3
                        value: String(StatsModel.currentStreak)
                        unit: i18np("day", "days", StatsModel.currentStreak)
                        label: i18n("Streak")
                        note: i18n("Best %1", StatsModel.bestStreak)
                    }
                }
            }

            // One chart: work per hour, day or month, in quiet bars; the chosen one in the accent
            // colour, named with its focus time at the right of the title.
            Panel {
                Layout.fillWidth: true
                title: i18n("Activity")
                headerExtra: Text {
                    objectName: "chartSelection"
                    opacity: chart.selectedBucket ? 1 : 0
                    text: chart.selectedBucket
                          ? i18nc("the chosen bar of the chart: its hour, day or month · its focus time", "%1 · %2",
                                  chart.selectedBucket.name, StatsModel.formatDuration(chart.selectedBucket.workSeconds))
                          : " "
                    color: Theme.dim
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.Medium
                    font.features: { "tnum": 1 }
                }

                Item {
                    id: chart
                    objectName: "activityChart"
                    Layout.fillWidth: true
                    implicitHeight: page.compact ? Theme.chartHeightCompact : Theme.chartHeight

                    readonly property var buckets: StatsModel.buckets
                    // The tallest bar, with a little room above.
                    readonly property real scaleTop: Math.max(1, buckets.reduce((m, b) => Math.max(m, b.workSeconds), 0) * 1.05)
                    readonly property real labelHeight: Theme.spaceXl
                    readonly property real plotHeight: height - labelHeight - Theme.spaceSm
                    readonly property real slot: buckets.length > 0 ? width / buckets.length : width
                    readonly property real barWidth: Math.max(Theme.hairline * 2, Math.min(Theme.chartBarMaxWidth, slot * 0.7))
                    // Labels thin out when there are many bars (a month, a day's hours).
                    readonly property int labelEvery: slot < Theme.space3xl ? Math.ceil(Theme.space3xl / Math.max(1, slot)) : 1

                    /// The bar chosen by a click or the arrow keys, or -1 for the default one.
                    property int chosenIndex: -1
                    /// The bar of "now" when the period is under way, else the last one with work.
                    readonly property int defaultIndex: {
                        const current = buckets.findIndex(b => b.isCurrent);
                        if (current >= 0) {
                            return current;
                        }
                        for (let i = buckets.length - 1; i >= 0; --i) {
                            if (buckets[i].workSeconds > 0) {
                                return i;
                            }
                        }
                        return -1;
                    }
                    readonly property int selectedIndex: chosenIndex >= 0 && chosenIndex < buckets.length ? chosenIndex : defaultIndex
                    readonly property var selectedBucket: selectedIndex >= 0 ? buckets[selectedIndex] : null

                    function choose(index: int): void {
                        if (buckets.length > 0) {
                            chosenIndex = Math.max(0, Math.min(buckets.length - 1, index));
                        }
                    }

                    // Another period starts with its default bar again.
                    Connections {
                        target: StatsModel
                        function onPeriodChanged() {
                            chart.chosenIndex = -1;
                        }
                    }

                    activeFocusOnTab: buckets.length > 0
                    Keys.onLeftPressed: choose(selectedIndex - 1)
                    Keys.onRightPressed: choose(selectedIndex + 1)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Home) {
                            choose(0);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_End) {
                            choose(buckets.length - 1);
                            event.accepted = true;
                        }
                    }
                    Accessible.role: Accessible.Chart
                    Accessible.name: i18n("Activity")
                    Accessible.description: chart.selectedBucket ? chart.selectedBucket.tooltip : ""

                    Repeater {
                        model: chart.buckets

                        Item {
                            id: bucket
                            required property var modelData
                            required property int index
                            readonly property bool selected: index === chart.selectedIndex
                            x: index * chart.slot
                            width: chart.slot
                            height: chart.height

                            readonly property real workHeight: chart.plotHeight * modelData.workSeconds / chart.scaleTop

                            // The tooltip's text, for screen readers too.
                            Accessible.role: Accessible.StaticText
                            Accessible.name: modelData.tooltip
                            Accessible.selectable: true
                            Accessible.selected: selected

                            HoverHandler {
                                id: bucketHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: chart.choose(bucket.index)
                            }
                            QQC2.ToolTip.visible: bucketHover.hovered && (modelData.workSeconds > 0 || modelData.breakSeconds > 0)
                            QQC2.ToolTip.text: modelData.tooltip
                            QQC2.ToolTip.delay: Theme.durationShort

                            Rectangle {
                                id: bar
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: chart.barWidth
                                y: chart.plotHeight - height
                                height: Math.max(bucket.modelData.workSeconds > 0 ? Theme.spaceXs : Theme.hairline * 3, bucket.workHeight)
                                radius: Math.min(width / 2, Theme.radiusXs)
                                bottomLeftRadius: Math.min(width / 2, Theme.hairline * 2)
                                bottomRightRadius: bottomLeftRadius
                                opacity: bucketHover.hovered && !bucket.selected ? 0.8 : 1
                                color: bucket.selected ? Theme.accent : bucket.modelData.workSeconds > 0 ? Theme.subtle : Theme.track
                                Behavior on height { NumberAnimation { duration: Theme.durationLong; easing.type: Easing.OutCubic } }
                                Behavior on color { ColorAnimation { duration: Theme.durationShort } }
                            }
                            // The keyboard's place.
                            Rectangle {
                                visible: bucket.selected && chart.activeFocus
                                anchors.centerIn: bar
                                width: bar.width + Theme.spaceXs * 2
                                height: bar.height + Theme.spaceXs * 2
                                radius: Theme.radiusSm
                                color: "transparent"
                                border.width: Theme.hairline * 2
                                border.color: Theme.focusRing
                            }

                            Text {
                                visible: bucket.index % chart.labelEvery === 0
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: chart.plotHeight + Theme.spaceSm
                                text: bucket.modelData.label
                                color: bucket.selected ? Theme.foreground : Theme.muted
                                font.pixelSize: Theme.fontSmall
                                font.weight: bucket.selected ? Font.Medium : Font.Normal
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: -chart.labelHeight / 2
                        visible: !StatsModel.hasData
                        text: i18n("No focus time in this period yet.")
                        color: Theme.muted
                        font.pixelSize: Theme.fontBody
                    }
                }
            }

            // Side by side, one under the other in a narrow window. Each lists a fixed number of
            // rows, so the page does not move when a period has more or fewer entries.
            GridLayout {
                Layout.fillWidth: true
                columns: page.compact ? 1 : 2
                columnSpacing: Theme.spaceLg
                rowSpacing: Theme.spaceLg

                // Where the time went, by timer, category or task.
                Panel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 6
                    title: i18n("Where the time went")
                    headerHeight: Theme.controlHeightSm
                    headerExtra: Segmented {
                        model: page.breakdowns
                        current: page.breakdownId
                        onActivated: id => StatsModel.breakdownMode = page.breakdowns.find(b => b.id === id).value
                    }

                    Item {
                        id: breakdownList
                        Layout.fillWidth: true
                        implicitHeight: Theme.statsListRows * Theme.statsListRowHeight
                        // The largest entry fills its bar; the others are measured against it.
                        readonly property var entries: StatsModel.breakdown.slice(0, Theme.statsListRows)
                        readonly property real largest: Math.max(1, entries.reduce((m, e) => Math.max(m, e.workSeconds), 0))

                        Text {
                            visible: breakdownList.entries.length === 0
                            height: Theme.statsListRowHeight
                            verticalAlignment: Text.AlignVCenter
                            text: i18n("Nothing recorded in this period.")
                            color: Theme.muted
                            font.pixelSize: Theme.fontBody
                        }

                        Column {
                            width: parent.width

                            Repeater {
                                model: breakdownList.entries

                                RowLayout {
                                    id: entry
                                    required property var modelData
                                    width: parent.width
                                    height: Theme.statsListRowHeight
                                    spacing: Theme.spaceMd + Theme.hairline * 2

                                    Text {
                                        text: entry.modelData.name
                                        color: Theme.foreground
                                        font.pixelSize: Theme.fontBody
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                    }
                                    Rectangle {
                                        Layout.preferredWidth: Theme.breakdownBarWidth
                                        implicitHeight: Theme.breakdownBarHeight
                                        radius: height / 2
                                        color: Theme.track
                                        Rectangle {
                                            width: parent.width * Math.min(1, entry.modelData.workSeconds / breakdownList.largest)
                                            height: parent.height
                                            radius: parent.radius
                                            color: Theme.dim
                                        }
                                    }
                                    Text {
                                        text: entry.modelData.formattedDuration
                                        color: Theme.muted
                                        font.pixelSize: Theme.fontCaption
                                        font.features: { "tnum": 1 }
                                        horizontalAlignment: Text.AlignRight
                                        Layout.preferredWidth: Theme.breakdownValueWidth
                                    }
                                }
                            }
                        }
                    }
                }

                // The task notes of the period, newest first: when, and what.
                Panel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 5
                    headerHeight: Theme.controlHeightSm
                    title: i18n("Notes")

                    Item {
                        id: notesList
                        objectName: "notesList"
                        Layout.fillWidth: true
                        implicitHeight: Theme.statsListRows * Theme.statsListRowHeight
                        readonly property var notes: StatsModel.notes.slice(0, Theme.statsListRows)
                        // The column of times and days is as wide as its longest label.
                        readonly property real labelWidth: Math.ceil(labelMetrics.advanceWidth)

                        TextMetrics {
                            id: labelMetrics
                            font.pixelSize: Theme.fontBody
                            font.features: { "tnum": 1 }
                            text: notesList.notes.reduce((longest, n) => n.label.length > longest.length ? n.label : longest, "")
                        }

                        Text {
                            visible: notesList.notes.length === 0
                            height: Theme.statsListRowHeight
                            verticalAlignment: Text.AlignVCenter
                            text: i18n("No notes in this period.")
                            color: Theme.muted
                            font.pixelSize: Theme.fontBody
                        }

                        Column {
                            width: parent.width

                            Repeater {
                                model: notesList.notes

                                RowLayout {
                                    id: noteRow
                                    required property var modelData
                                    width: parent.width
                                    height: Theme.statsListRowHeight
                                    spacing: Theme.spaceMd + Theme.hairline * 2
                                    Accessible.role: Accessible.StaticText
                                    Accessible.name: modelData.note
                                    Accessible.description: modelData.when

                                    HoverHandler {
                                        id: noteHover
                                    }
                                    QQC2.ToolTip.visible: noteHover.hovered
                                    QQC2.ToolTip.text: modelData.when + "\n" + modelData.note
                                    QQC2.ToolTip.delay: Theme.toolTipDelay

                                    Text {
                                        text: noteRow.modelData.label
                                        color: Theme.subtle
                                        font.pixelSize: Theme.fontBody
                                        font.features: { "tnum": 1 }
                                        Layout.preferredWidth: notesList.labelWidth
                                    }
                                    Text {
                                        text: noteRow.modelData.note
                                        color: Theme.dim
                                        font.pixelSize: Theme.fontBody
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Every session of the period, newest first, each of which can be deleted (one
            // started by mistake). A fixed height that scrolls, so the page does not move.
            Panel {
                Layout.fillWidth: true
                title: i18n("Sessions")
                headerExtra: Text {
                    opacity: StatsModel.sessions.length > 0 ? 1 : 0
                    text: StatsModel.sessions.length > 0
                          ? i18np("%1 session", "%1 sessions", StatsModel.sessions.length) : " "
                    color: Theme.subtle
                    font.pixelSize: Theme.fontCaption
                    font.features: { "tnum": 1 }
                }

                ListView {
                    id: sessionsList
                    objectName: "sessionsList"
                    Layout.fillWidth: true
                    implicitHeight: Theme.statsSessionRows * Theme.statsSessionRowHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: StatsModel.sessions
                    QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
                    // The column of times and days is as wide as its longest label.
                    readonly property real labelWidth: Math.ceil(sessionLabelMetrics.advanceWidth)

                    TextMetrics {
                        id: sessionLabelMetrics
                        font.pixelSize: Theme.fontBody
                        font.features: { "tnum": 1 }
                        text: StatsModel.sessions.reduce((longest, s) => s.label.length > longest.length ? s.label : longest, "")
                    }

                    Text {
                        visible: sessionsList.count === 0
                        height: Theme.statsSessionRowHeight
                        verticalAlignment: Text.AlignVCenter
                        text: i18n("No sessions in this period.")
                        color: Theme.muted
                        font.pixelSize: Theme.fontBody
                    }

                    delegate: RowLayout {
                        id: sessionRow
                        required property var modelData
                        width: ListView.view.width - (sessionsList.contentHeight > sessionsList.height ? Theme.spaceMd : 0)
                        height: Theme.statsSessionRowHeight
                        spacing: Theme.spaceMd + Theme.hairline * 2
                        readonly property bool isWork: modelData.kind === "work"
                        readonly property string details: modelData.note.length > 0
                                                          ? i18nc("a session: its timer · its task note", "%1 · %2",
                                                                  modelData.timer, modelData.note)
                                                          : modelData.timer
                        Accessible.role: Accessible.StaticText
                        Accessible.name: i18nc("a session in the list: kind, duration, timer and note", "%1, %2: %3",
                                               modelData.kindName, modelData.duration, details)
                        Accessible.description: modelData.when

                        HoverHandler {
                            id: sessionHover
                        }

                        Text {
                            text: sessionRow.modelData.label
                            color: Theme.subtle
                            font.pixelSize: Theme.fontBody
                            font.features: { "tnum": 1 }
                            Layout.preferredWidth: sessionsList.labelWidth
                        }
                        // Focus in the accent colour, breaks quieter.
                        Rectangle {
                            implicitWidth: Theme.spaceSm
                            implicitHeight: Theme.spaceSm
                            radius: width / 2
                            color: sessionRow.isWork ? Theme.accent : Theme.subtle
                        }
                        Text {
                            text: sessionRow.modelData.kindName
                            color: Theme.foreground
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            text: sessionRow.details
                            color: Theme.dim
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }
                        Text {
                            text: sessionRow.modelData.completed
                                  ? sessionRow.modelData.duration
                                  : i18nc("an interrupted session: its duration", "%1, interrupted", sessionRow.modelData.duration)
                            color: Theme.muted
                            font.pixelSize: Theme.fontCaption
                            font.features: { "tnum": 1 }
                            horizontalAlignment: Text.AlignRight
                        }
                        // Shown on hover or keyboard focus, but always taking its place.
                        SButton {
                            id: deleteButton
                            kind: "ghost"
                            iconOnly: true
                            iconName: "trash"
                            text: i18n("Delete session")
                            implicitHeight: Theme.iconButton
                            opacity: sessionHover.hovered || hovered || visualFocus ? 1 : 0
                            onClicked: page.askDeleteSession(sessionRow.modelData)
                        }
                    }
                }
            }
        }
    }

    /// The session the dialog asks about.
    property var sessionToDelete: null

    /// A focus session that was finished went to the calendar, if one is set up: the dialog asks
    /// whether its event goes too.
    readonly property bool sessionInCalendar: sessionToDelete !== null && sessionToDelete.kind === "work"
                                              && sessionToDelete.completed && !!CalendarSync
                                              && CalendarSync.enabled && CalendarSync.calendarUrl.length > 0

    function askDeleteSession(session: var): void {
        sessionToDelete = session;
        alsoFromCalendar.checked = true;
        deleteSessionDialog.open();
    }

    // What deleting a session did, in a toast (the settings page shows its own).
    Connections {
        target: DataManager
        enabled: page.visible
        function onOperationFailed(message) {
            applicationWindow().showPassiveNotification(message);
        }
        function onOperationSucceeded(message) {
            applicationWindow().showPassiveNotification(message);
        }
    }

    ConfirmDialog {
        id: deleteSessionDialog
        title: i18n("Delete this session?")
        text: page.sessionToDelete
              ? i18nc("@info deleting a session: %1 kind, %2 duration, %3 date and time",
                      "%1 of %2, started %3. It no longer counts in the statistics. This cannot be undone.",
                      page.sessionToDelete.kindName, page.sessionToDelete.duration, page.sessionToDelete.when)
              : ""
        confirmText: i18n("Delete")
        confirmKind: "danger"
        onConfirmed: {
            deleteSessionDialog.close();
            if (page.sessionToDelete) {
                DataManager.deleteSession(page.sessionToDelete.sessionId, page.sessionInCalendar && alsoFromCalendar.checked);
            }
            page.sessionToDelete = null;
        }

        RowLayout {
            visible: page.sessionInCalendar
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            Text {
                id: alsoFromCalendarLabel
                text: CalendarSync && CalendarSync.calendarName.length > 0
                      ? i18nc("@option:check %1 calendar name", "Also delete its event from the calendar “%1”", CalendarSync.calendarName)
                      : i18n("Also delete its event from the calendar")
                color: Theme.foreground
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            SSwitch {
                id: alsoFromCalendar
                checked: true
                Accessible.name: alsoFromCalendarLabel.text
            }
        }
    }
}
