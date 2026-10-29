import QtQuick
import QtQuick.Controls

Switch {
    id: control
    text: ""
    padding: 0
    implicitWidth: 36
    implicitHeight: 21

    property string accessibleLabel: ""
    Accessible.name: accessibleLabel

    contentItem: Item {}

    indicator: Rectangle {
        x: 0
        y: 0
        width: control.width
        height: control.height
        radius: height / 2
        color: control.checked ? Theme.accent : (control.hovered ? Theme.surfaceHover : "transparent")
        border.width: control.checked ? 0 : 1
        border.color: control.hovered ? Theme.borderStrong : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.animFast } }

        Rectangle {
            width: 15
            height: 15
            radius: 7.5
            y: 3
            x: control.checked ? parent.width - width - 3 : 3
            color: control.checked ? "#ffffff" : Theme.textSecondary
            Behavior on x { NumberAnimation { duration: Theme.animNormal; easing.type: Theme.easeStandard } }
            Behavior on color { ColorAnimation { duration: Theme.animFast } }
        }
    }
}
