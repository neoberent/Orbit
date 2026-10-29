import QtQuick

Row {
    id: root

    property int days: 0x7f
    signal changed(int days)

    readonly property var labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

    spacing: 3

    Repeater {
        model: 7

        Rectangle {
            id: chip
            required property int index
            readonly property bool on: (root.days & (1 << index)) !== 0
            width: 38
            height: 26
            radius: Theme.radiusSmall
            color: on ? Theme.withAlpha(Theme.accent, 0.16) : (chipMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceInset)
            border.width: 1
            border.color: on ? Theme.withAlpha(Theme.accent, 0.45) : Theme.border

            Accessible.role: Accessible.CheckBox
            Accessible.name: root.labels[index]
            Accessible.checked: on
            Accessible.onToggleAction: root.changed(root.days ^ (1 << index))
            Accessible.onPressAction: root.changed(root.days ^ (1 << index))

            Text {
                anchors.centerIn: parent
                text: root.labels[chip.index]
                color: chip.on ? Theme.textPrimary : Theme.textTertiary
                font.pixelSize: Theme.fontMicro
                font.weight: chip.on ? Font.DemiBold : Font.Medium
            }
            MouseArea {
                id: chipMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.changed(root.days ^ (1 << chip.index))
            }
        }
    }
}
