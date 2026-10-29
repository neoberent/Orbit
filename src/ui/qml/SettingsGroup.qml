import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root

    property string title: ""
    property string description: ""
    default property alias rows: card.data

    Layout.fillWidth: true
    spacing: 0

    Text {
        visible: root.title.length > 0
        text: root.title
        color: Theme.textPrimary
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.trackingTight
    }

    Text {
        visible: root.description.length > 0
        Layout.fillWidth: true
        Layout.topMargin: 3
        text: root.description
        color: Theme.textTertiary
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: root.title.length > 0 ? 8 : 0
        Layout.preferredHeight: card.implicitHeight
        radius: Theme.radiusLarge
        color: Theme.panel
        border.width: 1
        border.color: Theme.border

        ColumnLayout {
            id: card
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0
        }
    }
}
