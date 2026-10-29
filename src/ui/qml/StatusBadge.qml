import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string label: ""
    property color tone: Theme.textSecondary

    readonly property bool live: label === "Downloading" || label === "Connecting" || label === "Seeding" || label === "Verifying"
    readonly property bool iconic: ["Completed", "Paused", "Error", "Failed", "Queued", "Cancelled", "Scheduled", "Corrupt", "File exists"].indexOf(label) >= 0

    implicitWidth: row.implicitWidth + 16
    implicitHeight: 20
    radius: Theme.radiusPill
    color: Theme.withAlpha(tone, 0.09)
    border.width: 1
    border.color: Theme.withAlpha(tone, 0.18)
    Behavior on color { ColorAnimation { duration: Theme.animNormal } }
    Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 5

        Item {
            Layout.preferredWidth: 9
            Layout.preferredHeight: 9
            Layout.alignment: Qt.AlignVCenter

            Rectangle {
                anchors.centerIn: parent
                visible: !root.iconic
                width: 6
                height: 6
                radius: 3
                color: root.tone
                Behavior on color { ColorAnimation { duration: Theme.animNormal } }

                property real pulse: 1.0
                opacity: root.live ? pulse : 1.0

                SequentialAnimation on pulse {
                    running: root.live
                    loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.35; duration: 800; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.35; to: 1.0; duration: 800; easing.type: Easing.InOutSine }
                }
            }

            Icon {
                anchors.centerIn: parent
                visible: root.iconic
                name: root.label === "Completed" ? "check" : (root.label === "Paused" ? "pause" : (root.label === "Queued" || root.label === "Scheduled" ? "clock" : (root.label === "Cancelled" ? "close" : "alert")))
                iconSize: 9
                color: root.tone
            }
        }

        Text {
            text: root.label
            color: root.tone
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.trackingWide
            Behavior on color { ColorAnimation { duration: Theme.animNormal } }
        }
    }
}
