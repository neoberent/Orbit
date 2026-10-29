import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    property var links: []
    signal downloadRequested(var links)
    signal reviewRequested(string link)

    function offer(newLinks) {
        links = newLinks
        const pos = trayController.cornerPosition(width, height)
        x = pos.x
        y = pos.y
        showNormal()
        raise()
        autoHide.restart()
        panel.opacity = 1
    }

    function dismiss() {
        autoHide.stop()
        hide()
    }

    width: 400
    height: panel.implicitHeight
    color: "transparent"
    transientParent: null
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    title: "Orbit"

    Timer {
        id: autoHide
        interval: 15000
        onTriggered: if (panelHover.hovered) restart(); else root.dismiss()
    }

    Rectangle {
        id: panel
        anchors.fill: parent
        implicitHeight: content.implicitHeight + 28
        radius: Theme.radiusLarge
        color: Theme.surfaceElevated
        border.width: 1
        border.color: Theme.borderStrong

        HoverHandler { id: panelHover }

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: Theme.radiusSmall
                    color: Theme.withAlpha(Theme.accent, 0.14)
                    Icon { anchors.centerIn: parent; name: "clipboard"; iconSize: 14; color: Theme.accent }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        text: root.links.length === 1 ? "Download this link?" : "Download " + root.links.length + " copied links?"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: "Copied to the clipboard"
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontMicro
                    }
                }
                IconButton { iconName: "close"; tooltip: "Dismiss"; onClicked: root.dismiss() }
            }

            Repeater {
                model: root.links.slice(0, 3)
                Text {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    font.family: Theme.monoFamily
                    elide: Text.ElideMiddle
                }
            }
            Text {
                visible: root.links.length > 3
                text: "and " + (root.links.length - 3) + " more"
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                ModernButton {
                    visible: root.links.length === 1
                    text: "Options…"
                    variant: "secondary"
                    onClicked: {
                        const link = root.links[0]
                        root.dismiss()
                        root.reviewRequested(link)
                    }
                }
                ModernButton {
                    text: root.links.length === 1 ? "Download" : "Download All"
                    variant: "primary"
                    iconName: "arrowDown"
                    onClicked: {
                        const list = root.links
                        root.dismiss()
                        root.downloadRequested(list)
                    }
                }
            }
        }
    }
}
