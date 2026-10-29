import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property string iconName: "all"
    property string title: "Nothing here"
    property string subtitle: ""
    property bool showAction: false
    property bool showGuide: false
    property bool dragActive: false
    property bool isMediaContext: false
    property string actionLabel: "Add Download"
    signal actionRequested()

    readonly property string displayIcon: dragActive ? "arrowDown" : iconName
    readonly property string displayTitle: dragActive ? "Drop to add" : title
    readonly property string displaySubtitle: dragActive ? "Release to queue this file or link" : subtitle

    readonly property var ways: isMediaContext
        ? [
            { icon: "link", title: "Paste a video link", detail: "Any page yt-dlp understands" },
            { icon: "media", title: "Pick a quality", detail: "Best, 1080p, 720p or 480p" },
            { icon: "arrowDown", title: "Audio only", detail: "Save as M4A, Opus or MP3" }
          ]
        : [
            { icon: "link", title: "Paste a link", detail: "Direct HTTP(S) downloads" },
            { icon: "torrent", title: "Drop a .torrent", detail: "Drag it onto this window" },
            { icon: "magnet", title: "Open a magnet", detail: "Paste one or click a link" }
          ]

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusXl
        color: Theme.withAlpha(Theme.accent, root.dragActive ? 0.05 : 0.0)
        border.width: 1.5
        border.color: Theme.withAlpha(Theme.accent, root.dragActive ? 0.5 : 0.0)
        Behavior on color { ColorAnimation { duration: Theme.animNormal } }
        Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
    }

    ColumnLayout {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -Theme.spaceLg
        spacing: 0

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 52
            Layout.preferredHeight: 52
            radius: Theme.radiusXl
            color: root.dragActive ? Theme.withAlpha(Theme.accent, 0.12) : Theme.surface
            border.width: 1
            border.color: root.dragActive ? Theme.withAlpha(Theme.accent, 0.45) : Theme.borderStrong
            Behavior on color { ColorAnimation { duration: Theme.animNormal } }
            Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }

            Icon {
                anchors.centerIn: parent
                name: root.displayIcon
                iconSize: 22
                color: root.dragActive ? Theme.accent : Theme.textSecondary
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceLg
            text: root.displayTitle
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.trackingTight
        }

        Text {
            visible: root.displaySubtitle.length > 0
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            Layout.preferredWidth: 380
            text: root.displaySubtitle
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        RowLayout {
            visible: root.showAction && !root.dragActive
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceXl - 4
            spacing: 10

            ModernButton {
                text: root.actionLabel
                iconName: "plus"
                variant: "primary"
                onClicked: root.actionRequested()
            }
            KeyCap { text: "Ctrl N" }
        }

        Row {
            visible: root.showGuide && !root.dragActive
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceXxl + 4
            spacing: 10

            Repeater {
                model: root.ways

                Rectangle {
                    required property var modelData
                    width: 188
                    height: 88
                    radius: Theme.radiusLarge
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 4

                        Icon {
                            name: modelData.icon
                            iconSize: 15
                            color: Theme.textSecondary
                        }
                        Item { Layout.fillHeight: true }
                        Text {
                            text: modelData.title
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.detail
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        RowLayout {
            visible: root.showGuide && !root.dragActive
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceLg
            spacing: 6

            KeyCap { text: "Ctrl F" }
            Text { text: "Search"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            Item { Layout.preferredWidth: 10 }
            KeyCap { text: "Space" }
            Text { text: "Pause / resume"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            Item { Layout.preferredWidth: 10 }
            KeyCap { text: "Enter" }
            Text { text: "Open"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            Item { Layout.preferredWidth: 10 }
            KeyCap { text: "Ctrl A" }
            Text { text: "Select all"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
        }
    }
}
