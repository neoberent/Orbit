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

    property bool mediaSubtitles: false
    property string mediaSubtitleLanguages: "en"
    property bool mediaEmbedSubtitles: true
}
