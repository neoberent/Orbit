import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property alias text: field.text
    property alias field: field
    property alias placeholderText: field.placeholderText
    property alias validator: field.validator
    property alias echoMode: field.echoMode
    property alias readOnly: field.readOnly
    property string iconName: ""
    property string suffix: ""
    property bool monospace: false
    property string accessibleLabel: ""

    signal editingFinished()

    implicitHeight: 34
    implicitWidth: 200
    radius: Theme.radiusMedium
    color: Theme.surfaceInset
    border.width: 1
    border.color: field.activeFocus ? Theme.withAlpha(Theme.accent, 0.8)
                  : (hover.hovered ? Theme.borderStrong : Theme.border)
    Behavior on border.color { ColorAnimation { duration: Theme.animFast } }

    HoverHandler { id: hover }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8

        Icon {
            visible: root.iconName.length > 0
            name: root.iconName
            iconSize: 13
            color: Theme.textTertiary
        }

        TextField {
            id: field
            Layout.fillWidth: true
            Layout.fillHeight: true
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.textPrimary
            placeholderTextColor: Theme.textDisabled
            font.pixelSize: Theme.fontBody
            font.family: root.monospace ? Theme.monoFamily : Qt.application.font.family
            selectByMouse: true
            background: Item {}
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            onEditingFinished: root.editingFinished()
            Accessible.name: root.accessibleLabel.length > 0 ? root.accessibleLabel : root.placeholderText
        }

        Text {
            visible: root.suffix.length > 0
            text: root.suffix
            color: Theme.textTertiary
            font.pixelSize: Theme.fontCaption
        }
    }
}
