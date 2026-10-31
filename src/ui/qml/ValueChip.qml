import QtQuick

Rectangle {
    property string text
    implicitWidth: Math.max(32, chipText.implicitWidth + 16)
    implicitHeight: 22
    radius: Theme.radiusSmall
    color: Theme.surfaceInset
    border.width: 1
    border.color: Theme.border
    Text {
        id: chipText
        anchors.centerIn: parent
        text: parent.text
        color: Theme.textPrimary
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        font.features: { "tnum": 1 }
    }
}
