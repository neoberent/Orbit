import QtQuick
import QtQuick.Effects

Rectangle {
    id: root

    property string fileName: ""
    property bool isTorrent: false
    property bool isMedia: false
    property bool muted: false
    property real size: 34
    // file:// URL of a preview (a video's thumbnail, or the picture itself); the tile falls
    // back to the file type when there's none or it can't be read.
    property string thumbnail: ""
    readonly property bool showsThumbnail: thumbnail.length > 0 && preview.status === Image.Ready

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

    Image {
        id: preview
        anchors.fill: parent
        anchors.margins: 1
        visible: false
        source: root.thumbnail
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: Math.ceil(root.size * 2)
        sourceSize.height: Math.ceil(root.size * 2)
        smooth: true
        mipmap: true
    }

    // The picture follows the tile's rounded corners through a mask of the same shape.
    Rectangle {
        id: thumbnailMask
        width: preview.width
        height: preview.height
        radius: Math.max(0, root.radius - 1)
        visible: false
        layer.enabled: true
    }

    MultiEffect {
        anchors.fill: preview
        source: preview
        maskEnabled: true
        maskSource: thumbnailMask
        visible: root.showsThumbnail
        opacity: root.muted ? 0.45 : 1
    }

    Text {
        anchors.centerIn: parent
        visible: root.extension.length > 0 && !root.showsThumbnail
        text: root.extension
        color: root.tint
        font.pixelSize: Math.max(8, Math.round(root.size * (root.extension.length > 3 ? 0.24 : 0.28)))
        font.weight: Font.Bold
        font.letterSpacing: 0.3
    }

    Icon {
        anchors.centerIn: parent
        visible: root.extension.length === 0 && !root.showsThumbnail
        name: root.isTorrent ? "torrent" : (root.isMedia ? "media" : "downloading")
        iconSize: Math.round(root.size * 0.46)
        color: root.tint
    }
}
