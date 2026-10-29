import QtQuick

Item {
    id: root
    height: 5
    clip: true

    property real value: 0.0
    property color trackColor: Theme.borderSoft
    property color fillColor: Theme.accent
    property bool indeterminate: false

    Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: root.trackColor
    }

    Rectangle {
        id: fill
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        radius: height / 2
        visible: !root.indeterminate
        color: root.fillColor
        width: Math.max(height, parent.width * Math.max(0, Math.min(1, root.value)))

        Behavior on width {
            NumberAnimation { duration: Theme.animSlow; easing.type: Theme.easeEmphasized }
        }
    }

    Rectangle {
        id: shimmer
        visible: root.indeterminate
        width: parent.width * 0.3
        height: parent.height
        radius: height / 2

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.withAlpha(root.fillColor, 0.0) }
                GradientStop { position: 0.5; color: root.fillColor }
                GradientStop { position: 1.0; color: Theme.withAlpha(root.fillColor, 0.0) }
            }
        }

        SequentialAnimation on x {
            running: root.indeterminate
            loops: Animation.Infinite
            NumberAnimation { from: -shimmer.width; to: root.width; duration: 1200; easing.type: Easing.InOutQuad }
        }
    }
}
