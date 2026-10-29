import QtQuick

Rectangle {
    id: root

    property string fileName: ""
    property bool isTorrent: false
    property bool isMedia: false
    property bool muted: false
    property real size: 34

    readonly property string kind: Theme.fileKind(fileName, isTorrent, isMedia)
    readonly property string extension: isTorrent || isMedia ? "" : Theme.fileExtension(fileName).toUpperCase()
    readonly property color tint: muted ? Theme.kindGeneric : Theme.kindColor(kind)

    width: size
    height: size
    radius: Math.round(size * 0.24)
    color: Theme.withAlpha(tint, muted ? 0.07 : 0.12)
    border.width: 1
    border.color: Theme.withAlpha(tint, muted ? 0.12 : 0.22)
    Behavior on color { ColorAnimation { duration: Theme.animNormal } }

    Text {
        anchors.centerIn: parent
        visible: root.extension.length > 0
        text: root.extension
        color: root.tint
        font.pixelSize: Math.round(root.size * (root.extension.length > 3 ? 0.24 : 0.28))
        font.weight: Font.Bold
        font.letterSpacing: 0.3
    }

    Icon {
        anchors.centerIn: parent
        visible: root.extension.length === 0
        name: root.isTorrent ? "torrent" : (root.isMedia ? "media" : "downloading")
        iconSize: Math.round(root.size * 0.46)
        color: root.tint
    }
}
