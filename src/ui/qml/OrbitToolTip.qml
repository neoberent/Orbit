import QtQuick
import QtQuick.Controls

ToolTip {
    id: root
    delay: 500
    padding: 0
    topPadding: 5
    bottomPadding: 5
    leftPadding: 8
    rightPadding: 8

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animFast } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animInstant } }

    contentItem: Text {
        text: root.text
        color: Theme.textPrimary
        font.pixelSize: Theme.fontCaption
        font.weight: Font.Medium
    }

    background: Rectangle {
        color: Theme.surfaceElevated
        radius: Theme.radiusSmall
        border.width: 1
        border.color: Theme.borderStrong
    }
}
