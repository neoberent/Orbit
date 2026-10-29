import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

MenuItem {
    id: control

    property string iconName: ""
    property bool danger: false
    property bool iconHidden: false

    implicitHeight: 30
    leftPadding: 10
    rightPadding: 10

    readonly property color labelColor: !enabled ? Theme.textDisabled
                                        : (danger && highlighted ? Theme.danger : Theme.textPrimary)

    contentItem: RowLayout {
        spacing: 10
        Icon {
            visible: control.iconName.length > 0
            opacity: control.iconHidden ? 0 : 1
            name: control.iconName
            iconSize: 12.5
            color: !control.enabled ? Theme.textDisabled
                   : control.danger && control.highlighted ? Theme.danger
                   : control.highlighted ? Theme.textPrimary : Theme.textSecondary
        }
        Text {
            Layout.fillWidth: true
            text: control.text
            color: control.labelColor
            font.pixelSize: Theme.fontBody
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: !control.highlighted || !control.enabled ? "transparent"
               : control.danger ? Theme.withAlpha(Theme.danger, 0.1) : Theme.surfaceHover
        Behavior on color { ColorAnimation { duration: Theme.animInstant } }
    }
}
