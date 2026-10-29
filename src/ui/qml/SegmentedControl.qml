import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property var options: []
    property var value
    signal activated(var value)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 30
    radius: Theme.radiusMedium
    color: Theme.surfaceInset
    border.width: 1
    border.color: Theme.border

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: root.options

            Rectangle {
                id: segment
                required property var modelData
                readonly property bool active: root.value === modelData.value
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.checked: active
                Accessible.onPressAction: root.activated(modelData.value)
                width: segLabel.implicitWidth + 24
                height: root.height - 6
                radius: Theme.radiusSmall
                color: active ? Theme.surfaceElevated : (segMouse.containsMouse ? Theme.withAlpha(Theme.textPrimary, 0.04) : "transparent")
                border.width: active ? 1 : 0
                border.color: Theme.borderStrong
                Behavior on color { ColorAnimation { duration: Theme.animFast } }

                Text {
                    id: segLabel
                    anchors.centerIn: parent
                    text: segment.modelData.label
                    color: segment.active ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    font.weight: segment.active ? Font.DemiBold : Font.Medium
                }

                MouseArea {
                    id: segMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activated(segment.modelData.value)
                }
            }
        }
    }
}
