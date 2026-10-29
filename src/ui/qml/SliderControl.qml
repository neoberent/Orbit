import QtQuick
import QtQuick.Controls

Slider {
    id: control
    implicitHeight: 20

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.surfaceHover

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 15
        height: 15
        radius: 7.5
        color: "#ffffff"
        border.width: 1
        border.color: Theme.withAlpha("#000000", 0.1)
        scale: control.pressed ? 1.15 : (control.hovered ? 1.08 : 1.0)
        Behavior on scale { NumberAnimation { duration: Theme.animFast; easing.type: Theme.easeStandard } }
    }
}
