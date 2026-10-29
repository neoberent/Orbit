import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string message: ""
    property bool success: true

    function show(text, ok) {
        message = text
        success = ok === undefined ? true : ok
        hideTimer.restart()
        visible = true
        opacity = 1
    }

    visible: false
    opacity: 0
    z: 50
    implicitWidth: Math.min(row.implicitWidth + 28, parent ? parent.width - 40 : 560)
    implicitHeight: 36
    radius: Theme.radiusMedium
    color: Theme.surfaceElevated
    border.width: 1
    border.color: root.success ? Theme.borderStrong : Theme.withAlpha(Theme.danger, 0.45)

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.animNormal
            onRunningChanged: if (!running && root.opacity === 0) root.visible = false
        }
    }

    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    Timer {
        id: hideTimer
        interval: 4200
        onTriggered: if (!toastHover.hovered) root.opacity = 0; else restart()
    }

    HoverHandler { id: toastHover }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 14
        spacing: 9

        Icon {
            name: root.success ? "check" : "alert"
            iconSize: 13
            color: root.success ? Theme.success : Theme.danger
        }
        Text {
            Layout.fillWidth: true
            text: root.message
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideMiddle
        }
    }
}
