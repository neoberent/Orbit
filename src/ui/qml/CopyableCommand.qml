import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    property string command: ""
    property bool copied: false

    Layout.fillWidth: true
    Layout.preferredHeight: 32
    radius: Theme.radiusSmall
    color: Theme.background
    border.width: 1
    border.color: Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: root.command
            color: Theme.textPrimary
            font.family: "Cascadia Mono, Consolas, monospace"
            font.pixelSize: Theme.fontMicro
            elide: Text.ElideRight
        }

        IconButton {
            iconName: root.copied ? "check" : "copy"
            iconColor: root.copied ? Theme.success : Theme.textSecondary
            tooltip: root.copied ? "Copied" : "Copy"
            onClicked: {
                copyField.text = root.command
                copyField.selectAll()
                copyField.copy()
                root.copied = true
                copiedResetTimer.restart()
            }
        }
    }

    TextEdit {
        id: copyField
        visible: false
    }

    Timer {
        id: copiedResetTimer
        interval: 1500
        onTriggered: root.copied = false
    }
}
