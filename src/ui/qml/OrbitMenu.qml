import QtQuick
import QtQuick.Controls

Menu {
    id: root
    topPadding: 5
    bottomPadding: 5
    leftPadding: 5
    rightPadding: 5

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animFast; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: Theme.animFast; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animInstant }
    }

    background: Rectangle {
        implicitWidth: 212
        color: Theme.surfaceElevated
        radius: Theme.radiusLarge
        border.width: 1
        border.color: Theme.borderStrong
    }
}
