// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick
import stream.sugo.app

/**
 * Every colour and size of the interface. The QML elsewhere uses only these (tests/check_qml_colours.sh
 * and tests/check_qml_sizes.sh fail on literals outside this file).
 *
 * The look: neutral and nearly monochrome, 1 px borders, little decoration. The brand colours
 * (peach → red → magenta, green for breaks) appear only where the time is: the ring, the bars,
 * the status. Dark or light: the "Color scheme" setting, or the desktop's choice for "System"
 * (dark when the desktop does not say).
 */
QtObject {
    id: theme

    readonly property bool dark: AppSettings.colorScheme === "dark"
                                 || (AppSettings.colorScheme !== "light" && Qt.styleHints.colorScheme !== Qt.ColorScheme.Light)

    // --- Surfaces ----------------------------------------------------------------------------
    readonly property color background: dark ? "#0e0e10" : "#fbfbfb"
    readonly property color card: dark ? "#161618" : "#ffffff"
    readonly property color raised: dark ? "#1c1c1f" : "#ffffff"
    readonly property color border: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color borderStrong: dark ? Qt.rgba(1, 1, 1, 0.15) : Qt.rgba(0, 0, 0, 0.15)
    readonly property color hover: dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.04)
    readonly property color pressed: dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color selected: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color track: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.07)
    readonly property color scrim: Qt.rgba(0, 0, 0, dark ? 0.6 : 0.3)
    /// Under what floats over the page: the composer, menus, sheets.
    readonly property color shadow: Qt.rgba(0, 0, 0, dark ? 0.55 : 0.14)

    // --- Text --------------------------------------------------------------------------------
    readonly property color foreground: dark ? "#ececee" : "#18181b"
    readonly property color muted: dark ? "#8e8e97" : "#6b6b74"
    /// Text a little quieter than the foreground: menu rows, the bars of a breakdown.
    readonly property color dim: theme.alpha(foreground, 0.75)
    readonly property color subtle: dark ? "#5d5d66" : "#a1a1aa"

    /// The main action: the text colour as a fill.
    readonly property color primary: dark ? "#ececee" : "#18181b"
    readonly property color primaryText: dark ? "#0e0e10" : "#fbfbfb"
    readonly property color focusRing: dark ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(0, 0, 0, 0.45)

    // --- Brand and states --------------------------------------------------------------------
    readonly property color brandPeach: "#ffa36b"
    readonly property color brand: "#ff3d3d"
    readonly property color brandMagenta: "#c2185b"
    /// Text and knobs on the brand colour.
    readonly property color brandText: "#ffffff"
    readonly property color breakLight: "#7ee0a1"
    readonly property color breakColor: "#22c55e"
    readonly property color breakDeep: "#0f9d6b"
    readonly property color positive: dark ? "#4ade80" : "#16a34a"
    readonly property color warning: dark ? "#fbbf24" : "#d97706"
    readonly property color danger: dark ? "#f87171" : "#dc2626"
    readonly property color dangerFill: dark ? Qt.rgba(0.97, 0.44, 0.44, 0.12) : Qt.rgba(0.86, 0.15, 0.15, 0.08)
    readonly property color warningFill: dark ? Qt.rgba(0.98, 0.75, 0.14, 0.1) : Qt.rgba(0.85, 0.47, 0.02, 0.08)

    // --- Accent ------------------------------------------------------------------------------
    /// The shades of the accent colours live in TrayIconStyle.accentShades, with the colours of
    /// the running-timer badge, so the interface and the badge cannot drift apart.
    /// The chosen accent, for work and the main controls.
    readonly property color accent: TrayIconStyle.accentShadeColor(AppSettings.accentColor, dark)
    /// Text and icons on the accent.
    readonly property color accentText: TrayIconStyle.accentTextOnColor(AppSettings.accentColor, dark)
    /// Breaks: the break green, or blue when the accent is itself green (lime, green, teal), so
    /// work and break never look alike.
    readonly property color breakAccent: ["lime", "green", "teal"].includes(AppSettings.accentColor)
                                         ? (dark ? "#60a5fa" : "#2563eb") : breakColor

    // --- Window buttons of other systems ---------------------------------------------------
    readonly property color windowsClose: "#c42b1c"
    readonly property color macClose: "#ff5f57"
    readonly property color macMinimize: "#febc2e"
    readonly property color macMaximize: "#28c840"

    // --- The application icon, drawn live by LogoIntro (same colours as its SVG) ---------------
    readonly property color logoTileTop: "#33363d"
    readonly property color logoTileBottom: "#16181c"
    readonly property color logoEdge: Qt.rgba(1, 1, 1, 0.12)
    readonly property color logoLeafLight: "#8ee88a"
    readonly property color logoLeafDeep: "#1f9d55"
    /// The soft light of the brand behind the logo (welcome tour), strongest in the middle.
    readonly property color glowInner: theme.alpha(brand, dark ? 0.32 : 0.18)
    readonly property color glowOuter: dark ? theme.alpha(brandMagenta, 0.12) : theme.alpha(brandPeach, 0.08)

    // --- Spacing, shapes, sizes ---------------------------------------------------------------
    readonly property int hairline: 1
    readonly property int spaceXs: 4
    readonly property int spaceSm: 6
    readonly property int spaceMd: 8
    readonly property int spaceLg: 12
    readonly property int spaceXl: 16
    readonly property int space2xl: 24
    readonly property int space3xl: 32
    readonly property int space4xl: 48

    readonly property int radiusSm: 6
    readonly property int radius: 8
    /// Larger surfaces: popups, segmented controls, cards inside cards.
    readonly property int radiusMd: 10
    readonly property int radiusLg: 12

    readonly property int iconSm: 14
    readonly property int icon: 16
    readonly property int iconMd: 20
    readonly property int iconLg: 32
    readonly property int iconXl: 40
    /// The application icon in the welcome tour.
    readonly property int logoHero: 128

    readonly property int controlHeight: 30
    readonly property int controlHeightLg: 40
    readonly property int navHeight: 34
    readonly property int contentMaxWidth: 760
    readonly property int windowWidth: 980
    readonly property int windowHeight: 680
    readonly property int windowMinWidth: 380
    readonly property int windowMinHeight: 520

    // The redesign's sizes, one block per part of the window (each part adds its own here).
    readonly property int radiusXs: 4
    readonly property int radiusXl: 16
    readonly property int controlHeightSm: 28
    readonly property int iconButton: 28
    readonly property int roundButton: 34
    readonly property int switchWidth: 30
    readonly property int switchHeight: 18
    readonly property int switchKnob: 12
    readonly property int sliderWidth: 140
    readonly property int sliderWidthCompact: 110
    readonly property int sliderTrack: 4
    readonly property int sliderKnob: 14
    readonly property int selectMinWidth: 120
    readonly property int selectMinWidthWide: 150
    readonly property int fieldWidthWide: 220
    readonly property int numberFieldWidth: 76
    readonly property int swatchSize: 14
    readonly property int phaseDot: 6
    /// The dot on the title bar's update icon: something is waiting in Settings › About.
    readonly property int badgeDot: 6

    // --- Title bar
    /// On macOS the bar takes the height of the system's title bar instead (TitleBar.qml).
    readonly property int titleBarHeight: 44
    readonly property int titleBarPaddingLeft: 14
    readonly property int titleBarPaddingRight: 8
    /// Room for the traffic lights of macOS at the left of the title bar.
    readonly property int titleBarMacInset: 84
    readonly property int tabHeight: 28
    readonly property int tabPaddingX: 12
    readonly property int tabPaddingXCompact: 8
    readonly property int logoSmall: 20
    /// Below this window width the title bar and the pages use their compact layout.
    readonly property int compactBreakpoint: 560

    // --- Menus
    /// A menu opens this far from its button, and stays this far from the window's edges.
    readonly property int menuOffset: 4
    readonly property int menuEdgeMargin: 8
    readonly property int menuPadding: 5
    readonly property int menuRowHeight: 28
    readonly property int menuRowHeightTwoLine: 42
    readonly property int menuHeaderHeight: 22
    readonly property int menuMinWidth: 180
    readonly property int menuMaxWidth: 340
    readonly property int menuAutoStartWidth: 250
    readonly property int durationMenu: 150
    /// A menu grows from this scale to full size while it opens.
    readonly property real menuScaleFrom: 0.96
    readonly property int paletteWidth: 500
    readonly property int paletteTop: 64
    readonly property int paletteFieldHeight: 42
    readonly property int sheetWidth: 420
    readonly property int sheetPadding: 18

    // --- Focus page
    readonly property int clockSize: 128
    readonly property int clockSizeCompact: 88
    readonly property int cycleTrackWidth: 420
    readonly property int cycleTrackHeight: 6
    // The cycle in the composer: taller than the timer page's track, to resize its phases by their edges.
    readonly property int cyclePreviewHeight: 12
    readonly property int cycleTrackGap: 3
    readonly property int composerWidth: 520
    readonly property int composerPadding: 10
    readonly property int dockBottomMargin: 16
    readonly property int dockSpacing: 10

    // --- Statistics
    readonly property int pagePaddingX: 28
    readonly property int pagePaddingY: 22
    readonly property int pagePaddingCompact: 16
    readonly property int chartHeight: 110
    readonly property int chartHeightCompact: 90
    readonly property int chartBarMaxWidth: 34
    readonly property int breakdownBarHeight: 4
    readonly property int breakdownBarWidth: 80
    readonly property int breakdownValueWidth: 56
    /// The lists of the statistics (where the time went, the notes): this many rows of this height.
    readonly property int statsListRows: 5
    readonly property int statsListRowHeight: 26
    /// The statistics fill the window up to this width, then stay centred.
    readonly property int statsMaxWidth: 1040

    // --- Settings
    readonly property int settingsNavWidth: 178
    readonly property int settingsNavItemHeight: 30
    /// Below this window width the list of categories becomes a menu.
    readonly property int settingsNavBreakpoint: 640
    readonly property int settingsBodyPaddingX: 26
    readonly property int settingsBodyPaddingY: 20
    /// The page's padding in a narrow window.
    readonly property int settingsBodyPaddingCompactX: 16
    readonly property int settingsBodyPaddingCompactY: 14
    /// The title of a category, at the top of its page.
    readonly property int fontSettingsTitle: 15
    /// A row puts its controls under its text when the text would have less room than this.
    readonly property int settingsRowTextMinWidth: 150
    readonly property int settingsRowMinHeight: 46
    readonly property int settingsRowPaddingX: 14
    readonly property int settingsRowPaddingY: 10
    readonly property int aboutLogo: 56

    // --- Welcome
    readonly property int logoWelcome: 88
    readonly property int welcomeTextWidth: 400
    readonly property int welcomeGroupWidth: 440
    readonly property int welcomeFooterHeight: 56
    readonly property int welcomeFooterPaddingX: 14
    readonly property int fontWelcomeTitle: 22

    // --- Type --------------------------------------------------------------------------------
    /// The system's fixed-width font ("monospace" is a fontconfig alias, unknown to macOS).
    readonly property string monoFamily: DesktopStyle.monoFamily
    readonly property int fontMicro: 10
    readonly property int fontSmall: 11
    readonly property int fontCaption: 12
    readonly property int fontBody: 13
    readonly property int fontTitle: 14
    /// A title a step above the body's.
    readonly property int fontTitleLg: 16
    /// The title of a section inside a page.
    readonly property int fontSection: 17
    readonly property int fontHeading: 20
    /// The figures of the statistics.
    readonly property int fontFigure: 22
    readonly property int fontDisplay: 26
    /// The name of the application in the welcome tour.
    readonly property int fontHero: 36
    readonly property real letterSpacingCaps: 1.2
    readonly property real letterSpacingTight: -0.8

    // --- Motion ------------------------------------------------------------------------------
    readonly property int durationShort: 120
    readonly property int durationLong: 260
    /// Between one element and the next when a group comes in one after the other.
    readonly property int durationStagger: 60
    readonly property int toolTipDelay: 600

    /// The colour of an accent id in the current theme (the dots of the colour menu); red for
    /// an unknown id.
    function accentSwatch(id: string): color {
        return TrayIconStyle.accentShadeColor(id, dark);
    }

    /// `c` with its alpha multiplied by `a` (for washes of the brand and state colours).
    function alpha(c: color, a: real): color {
        return Qt.rgba(c.r, c.g, c.b, c.a * a);
    }
}
