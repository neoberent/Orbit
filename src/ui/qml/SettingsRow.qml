import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property string label: ""
    property string description: ""
    property bool divider: true
    property bool dimmed: false
    default property alias control: controlSlot.data
    property alias extra: extraSlot.data

    Layout.fillWidth: true
    implicitHeight: Math.max(52, body.implicitHeight + 24)

    Component.onCompleted: {
        const slots = [controlSlot, extraSlot]
        for (const slot of slots) {
            for (let i = 0; i < slot.children.length; ++i) {
                const child = slot.children[i]
                if (child.accessibleLabel !== undefined && child.accessibleLabel.length === 0) child.accessibleLabel = root.label
            }
        }
    }
    opacity: dimmed ? 0.5 : 1.0
    Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

    Rectangle {
        visible: root.divider
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        height: 1
        color: Theme.border
    }

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: root.label
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    visible: root.description.length > 0
                    Layout.fillWidth: true
                    text: root.description
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            RowLayout {
                id: controlSlot
                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                spacing: 8
            }
        }

        ColumnLayout {
            id: extraSlot
            visible: children.length > 0
            Layout.fillWidth: true
            spacing: 8
        }
    }
}
