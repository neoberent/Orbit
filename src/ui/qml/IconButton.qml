import QtQuick
import QtQuick.Controls

Item {
    id: root
    implicitWidth: 28
    implicitHeight: 28

    property string iconName: ""
    property color iconColor: Theme.textSecondary
    property color hoverColor: Theme.textPrimary
    property bool danger: false
    property string tooltip: ""
    property bool hovered: false
    property bool pressed: false

    signal clicked()

    opacity: enabled ? 1.0 : 0.35

    Accessible.role: Accessible.Button
    Accessible.name: tooltip.length > 0 ? tooltip : iconName
    Accessible.onPressAction: if (enabled) clicked()

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSmall
        color: !root.enabled ? "transparent"
               : root.pressed ? (root.danger ? Theme.withAlpha(Theme.danger, 0.16) : Theme.surfacePressed)
               : root.hovered ? (root.danger ? Theme.withAlpha(Theme.danger, 0.1) : Theme.surfaceHover)
               : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    Icon {
        anchors.centerIn: parent
        name: root.iconName
        iconSize: 14
        color: root.hovered && root.enabled ? (root.danger ? Theme.danger : root.hoverColor) : root.iconColor
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onEntered: root.hovered = true
        onExited: root.hovered = false
        onPressed: root.pressed = true
        onReleased: root.pressed = false
        onClicked: root.clicked()
    }

    scale: root.pressed ? 0.92 : 1.0
    Behavior on scale { NumberAnimation { duration: Theme.animInstant; easing.type: Theme.easeStandard } }

    OrbitToolTip {
        visible: root.hovered && root.enabled && root.tooltip.length > 0
        text: root.tooltip
    }
}
