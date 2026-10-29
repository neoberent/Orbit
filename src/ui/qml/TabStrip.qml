import QtQuick
import QtQuick.Layouts

Row {
    id: root

    property var tabs: []
    property int currentIndex: 0
    signal selected(int index)

    spacing: 2

    Repeater {
        model: root.tabs

        Item {
            id: tab
            required property var modelData
            required property int index
            readonly property bool active: root.currentIndex === index
            width: tabLabel.implicitWidth + 20
            height: 28

            Accessible.role: Accessible.PageTab
            Accessible.name: modelData
            Accessible.selected: active
            Accessible.onPressAction: root.selected(index)

            Rectangle {
                anchors.fill: parent
                anchors.bottomMargin: 2
                radius: Theme.radiusSmall
                color: tabMouse.containsMouse && !tab.active ? Theme.surfaceHover : "transparent"
            }
            Text {
                id: tabLabel
                anchors.centerIn: parent
                text: tab.modelData
                color: tab.active ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                font.weight: tab.active ? Font.DemiBold : Font.Medium
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 12
                height: 2
                radius: 1
                color: Theme.accent
                visible: tab.active
            }
            MouseArea {
                id: tabMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.selected(tab.index)
            }
        }
    }
}
