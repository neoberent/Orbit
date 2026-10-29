import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 420
    anchors.centerIn: parent
    padding: 0

    property string heading: ""
    property string message: ""
    property string confirmLabel: "Confirm"
    property bool destructive: false
    property var onConfirm: null

    function ask(headingText, messageText, label, isDestructive, callback) {
        heading = headingText
        message = messageText
        confirmLabel = label
        destructive = isDestructive
        onConfirm = callback
        open()
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
    }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }

    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 16
            spacing: 14

            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(root.destructive ? Theme.danger : Theme.accent, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(root.destructive ? Theme.danger : Theme.accent, 0.24)
                Icon {
                    anchors.centerIn: parent
                    name: root.destructive ? "trash" : "alert"
                    iconSize: 15
                    color: root.destructive ? Theme.danger : Theme.accent
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.heading
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading + 1
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    text: root.message
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                    lineHeight: 1.15
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton { text: "Cancel"; variant: "ghost"; onClicked: root.close() }
            ModernButton {
                id: confirmButton
                text: root.confirmLabel
                variant: root.destructive ? "dangerSolid" : "primary"
                onClicked: {
                    const cb = root.onConfirm
                    root.close()
                    if (cb) cb()
                }
            }
        }
    }

    onOpened: confirmButton.forceActiveFocus()
}
