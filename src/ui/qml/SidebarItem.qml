import QtQuick
import QtQuick.Layouts

Item {
    id: root
    implicitHeight: 32

    property string iconName: ""
    property string label: ""
    property int count: -1
    property bool selected: false
    property bool hovered: false
    property bool pressed: false

    signal clicked()

    Accessible.role: Accessible.PageTab
    Accessible.name: label
    Accessible.selected: selected
    Accessible.onPressAction: clicked()

    Rectangle {
        width: 3
        height: root.selected ? 16 : 8
        radius: 1.5
        color: Theme.accent
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        opacity: root.selected ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
        Behavior on height { NumberAnimation { duration: Theme.animNormal; easing.type: Theme.easeEmphasized } }
    }

    Rectangle {
        id: bg
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        radius: Theme.radiusSmall
        color: root.selected ? Theme.withAlpha(Theme.accent, 0.12)
               : root.pressed ? Theme.surfacePressed
               : root.hovered ? Theme.surfaceHover : "transparent"
        border.width: 1
        border.color: root.selected ? Theme.withAlpha(Theme.accent, 0.18) : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.animFast } }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 19
        anchors.rightMargin: 13
        spacing: 10

        Icon {
            name: root.iconName
            iconSize: 14.5
            color: root.selected ? Theme.accent : (root.hovered ? Theme.textPrimary : Theme.textSecondary)
            Behavior on color { ColorAnimation { duration: Theme.animFast } }
        }

        Text {
            Layout.fillWidth: true
            text: root.label
            color: root.selected ? Theme.textPrimary : (root.hovered ? Theme.textPrimary : Theme.textSecondary)
            font.pixelSize: Theme.fontBody
            font.weight: root.selected ? Font.DemiBold : Font.Medium
            font.letterSpacing: Theme.trackingTight
            elide: Text.ElideRight
            Behavior on color { ColorAnimation { duration: Theme.animFast } }
        }

        Rectangle {
            visible: root.count >= 0
            Layout.preferredWidth: Math.max(19, countText.implicitWidth + 10)
            Layout.preferredHeight: 17
            radius: Theme.radiusPill
            color: root.selected ? Theme.withAlpha(Theme.accent, 0.2) : Theme.withAlpha(Theme.textPrimary, 0.06)
            opacity: root.count > 0 ? 1.0 : 0.5
            Behavior on color { ColorAnimation { duration: Theme.animFast } }

            Text {
                id: countText
                anchors.centerIn: parent
                text: root.count
                color: root.selected ? Theme.accent : Theme.textSecondary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.trackingTight
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onEntered: root.hovered = true
        onExited: root.hovered = false
        onPressed: root.pressed = true
        onReleased: root.pressed = false
        onClicked: root.clicked()
    }
}
