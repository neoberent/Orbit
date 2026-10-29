import QtQuick

Rectangle {
    id: root
    property string text: ""

    implicitWidth: capText.implicitWidth + 10
    implicitHeight: 18
    radius: Theme.radiusTiny
    color: Theme.withAlpha(Theme.textPrimary, 0.03)
    border.width: 1
    border.color: Theme.borderStrong

    Text {
        id: capText
        anchors.centerIn: parent
        text: root.text
        color: Theme.textSecondary
        font.pixelSize: 10
        font.weight: Font.DemiBold
    }
}
