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
    closePolicy: Popup.NoAutoClose

    property string action: "none"
    property int secondsLeft: 30

    function start(which) {
        action = which
        secondsLeft = 30
        open()
        countdown.restart()
    }

    function verb() {
        switch (action) {
        case "exit": return "Orbit will close"
        case "sleep": return "Your PC will go to sleep"
        case "hibernate": return "Your PC will hibernate"
        case "shutdown": return "Your PC will shut down"
        default: return ""
        }
    }

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        onTriggered: {
            root.secondsLeft--
            if (root.secondsLeft <= 0) {
                stop()
                root.close()
                powerManager.executeAction(root.action)
            }
        }
    }

    onClosed: countdown.stop()

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            spacing: 14
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.warning, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.warning, 0.24)
                Icon { anchors.centerIn: parent; name: "power"; iconSize: 15; color: Theme.warning }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: "All downloads are finished"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading + 1
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: root.verb() + " in " + root.secondsLeft + (root.secondsLeft === 1 ? " second." : " seconds.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                    font.features: { "tnum": 1 }
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
            ModernButton { id: cancelButton; text: "Cancel"; variant: "secondary"; onClicked: root.close() }
            ModernButton {
                text: powerManager.actionLabel(root.action) + " Now"
                variant: "primary"
                onClicked: {
                    root.close()
                    powerManager.executeAction(root.action)
                }
            }
        }
    }

    onOpened: cancelButton.forceActiveFocus()
}
