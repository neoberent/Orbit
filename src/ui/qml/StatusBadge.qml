import QtQuick
import QtQuick.Layouts

// A download's state as a small icon (or dot) and a word. Only what needs attention gets
// colour in its text: a running transfer, a failure, a choice to make. Finished, paused
// and waiting rows stay quiet, so a long list doesn't turn into a wall of coloured pills.
Item {
    id: root

    property string label: ""
    property color tone: Theme.textSecondary

    readonly property bool live: label === "Downloading" || label === "Connecting" || label === "Seeding" || label === "Verifying"
    readonly property bool iconic: ["Completed", "Paused", "Error", "Failed", "Queued", "Cancelled", "Scheduled", "Corrupt", "File exists"].indexOf(label) >= 0
    readonly property bool emphasized: live || ["Failed", "Error", "Corrupt", "File exists", "Missing"].indexOf(label) >= 0

    implicitWidth: row.implicitWidth
    implicitHeight: 20

    RowLayout {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Item {
            Layout.preferredWidth: 10
            Layout.preferredHeight: 10
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
                    NumberAnimation { from: 1.0; to: 0.35; duration: 900; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.35; to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                }
            }

            Icon {
                anchors.centerIn: parent
                visible: root.iconic
                name: root.label === "Completed" ? "check" : (root.label === "Paused" ? "pause" : (root.label === "Queued" || root.label === "Scheduled" ? "clock" : (root.label === "Cancelled" ? "close" : "alert")))
                iconSize: 10
                color: root.tone
            }
        }

        Text {
            text: root.label
            color: root.emphasized ? root.tone : Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            font.weight: root.emphasized ? Font.DemiBold : Font.Medium
            Behavior on color { ColorAnimation { duration: Theme.animNormal } }
        }
    }
}
