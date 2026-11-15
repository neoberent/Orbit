import QtQuick
import QtQuick.Effects

Item {
    id: root
    height: 5
    clip: true

    property real value: 0.0
    property color trackColor: Theme.borderSoft
    property color fillColor: Theme.accent
    property bool indeterminate: false
    // [start, done, start, done, ...] as fractions of the whole: one pair per connection of a
    // segmented download. With two or more, each part fills from its own start.
    property var segments: []
    readonly property int segmentCount: segments ? Math.floor(segments.length / 2) : 0
    readonly property bool segmented: segmentCount >= 2 && !indeterminate

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
        visible: !root.indeterminate && !root.segmented
        color: root.fillColor
        width: Math.max(height, parent.width * Math.max(0, Math.min(1, root.value)))

        Behavior on width {
            NumberAnimation { duration: Theme.animSlow; easing.type: Theme.easeEmphasized }
        }
    }

    Item {
        id: segmentLayer
        anchors.fill: parent
        visible: false
        layer.enabled: root.segmented

        Repeater {
            model: root.segmented ? root.segmentCount : 0

            Rectangle {
                required property int index
                readonly property real start: root.segments[index * 2] || 0
                readonly property real done: root.segments[index * 2 + 1] || 0
                // A one-pixel gap where each connection's part begins shows the track
                // through, so the parts stay distinguishable once they meet.
                readonly property int gap: start > 0.002 ? 1 : 0
                x: Math.round(start * segmentLayer.width) + gap
                width: Math.max(0, Math.min(segmentLayer.width - x, done * segmentLayer.width - gap))
                height: segmentLayer.height
                color: root.fillColor
                Behavior on width { NumberAnimation { duration: Theme.animSlow; easing.type: Theme.easeStandard } }
            }
        }
    }

    // The parts are drawn through the track's rounded shape, so the bar keeps its round ends.
    Rectangle {
        id: segmentMask
        anchors.fill: parent
        radius: height / 2
        visible: false
        layer.enabled: root.segmented
    }

    MultiEffect {
        anchors.fill: parent
        visible: root.segmented
        source: segmentLayer
        maskEnabled: true
        maskSource: segmentMask
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
