import QtQuick

Rectangle {
    color: Theme.surfaceElevated
    radius: Theme.radiusXl
    border.color: Theme.borderStrong
    border.width: 1

    Rectangle {
        anchors.top: parent.top
        anchors.topMargin: 1
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - Theme.radiusXl * 2
        height: 1
        color: Theme.highlight
    }
}
