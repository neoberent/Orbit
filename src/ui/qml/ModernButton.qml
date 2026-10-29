import QtQuick
import QtQuick.Controls

Button {
    id: control

    property string variant: "primary"
    property string iconName: ""

    implicitHeight: 32
    leftPadding: iconName.length > 0 ? 11 : 14
    rightPadding: 14
    topPadding: 0
    bottomPadding: 0

    readonly property color labelColor: variant === "primary" || variant === "dangerSolid" ? Theme.accentInk
                                         : variant === "danger" ? Theme.danger
                                         : Theme.textPrimary

    contentItem: Row {
        spacing: 7
        Icon {
            visible: control.iconName.length > 0
            name: control.iconName
            iconSize: 13
            color: control.labelColor
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: control.text
            color: control.labelColor
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.trackingTight
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        id: bg
        radius: Theme.radiusSmall
        color: control.variant === "primary"
               ? (control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accent))
               : control.variant === "dangerSolid"
                 ? (control.down ? Qt.darker(Theme.danger, 1.15) : (control.hovered ? Qt.lighter(Theme.danger, 1.08) : Theme.danger))
               : control.variant === "secondary"
                 ? (control.down ? Theme.surfacePressed : (control.hovered ? Theme.surfaceHover : Theme.surface))
                 : control.variant === "danger"
                   ? (control.hovered ? Theme.withAlpha(Theme.danger, 0.12) : "transparent")
                   : (control.down ? Theme.surfacePressed : (control.hovered ? Theme.surfaceHover : "transparent"))
        border.width: control.variant === "secondary" ? 1 : 0
        border.color: Theme.border
        Behavior on color { ColorAnimation { duration: Theme.animFast } }

        Rectangle {
            visible: control.variant === "primary" || control.variant === "dangerSolid"
            anchors.fill: parent
            anchors.margins: 0.5
            radius: parent.radius - 0.5
            color: "transparent"
            border.width: 1
            border.color: Theme.withAlpha("#ffffff", 0.08)
        }
    }

    opacity: enabled ? 1.0 : 0.42
    Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
}
