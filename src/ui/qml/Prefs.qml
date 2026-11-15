pragma Singleton
import QtQuick
import QtCore

Settings {
    category: "ui"

    property int windowWidth: 1240
    property int windowHeight: 760
    property int windowX: -1
    property int windowY: -1

    property string sortKey: "added"
    property bool sortDescending: false

    property bool notifyOnFinish: true

    // "dark", "light" or "system" (follow the desktop). Empty until first set, when it
    // takes over the older dark-theme switch.
    property string themeMode: ""
    // A key from Theme.accentPalette, or "system" for the desktop's accent colour.
    property string accent: "indigo"
    // "comfortable" or "compact" rows in the download list.
    property string density: "comfortable"
    // Lets the desktop show through the window's background (Linux).
    property bool translucent: false

    property bool mediaSubtitles: false
    property string mediaSubtitleLanguages: "en"
    property bool mediaEmbedSubtitles: true
}
