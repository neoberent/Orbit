import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: Theme.topBar

    property alias searchText: searchField.text
    property string pageTitle: "All Downloads"
    property string addButtonLabel: "Add Download"
    property int itemCount: 0
    property int selectedCount: 0
    readonly property var summary: downloadModel.summary
    readonly property string subtitle: {
        const parts = [itemCount + (itemCount === 1 ? " item" : " items")]
        if (selectedCount > 1) parts.push(selectedCount + " selected")
        if (summary.active > 0) parts.push(summary.active + " active")
        if (summary.downloading) parts.push(summary.downloadRateText)
        return parts.join("  ·  ")
    }

    signal addRequested()

    function focusSearch() {
        searchField.forceActiveFocus()
        searchField.selectAll()
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 14
        spacing: 10

        ColumnLayout {
            spacing: 1
            Layout.rightMargin: 4

            Text {
                text: root.pageTitle
                color: Theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
                font.letterSpacing: -0.25
            }
            Text {
                text: root.subtitle
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
            }
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            Layout.preferredHeight: 32
            Layout.preferredWidth: batchRow.implicitWidth + 6
            radius: Theme.radiusMedium
            color: Theme.surfaceInset
            border.width: 1
            border.color: Theme.border

            Row {
                id: batchRow
                anchors.centerIn: parent
                spacing: 1

                IconButton {
                    implicitWidth: 28
                    implicitHeight: 26
                    iconName: "play"
                    tooltip: "Resume all"
                    enabled: root.summary.paused > 0
                    onClicked: downloadModel.resumeAll()
                }
                IconButton {
                    implicitWidth: 28
                    implicitHeight: 26
                    iconName: "pause"
                    tooltip: "Pause all"
                    enabled: root.summary.active + root.summary.seeding > 0
                    onClicked: downloadModel.pauseAll()
                }
                Rectangle { width: 1; height: 14; anchors.verticalCenter: parent.verticalCenter; color: Theme.border }
                IconButton {
                    implicitWidth: 28
                    implicitHeight: 26
                    iconName: "sweep"
                    tooltip: "Clear finished"
                    enabled: root.summary.finished > 0
                    onClicked: downloadModel.clearFinished()
                }
            }
        }

        Rectangle {
            id: searchBox
            Layout.preferredWidth: searchField.activeFocus || searchField.text.length > 0 ? 280 : 230
            Layout.preferredHeight: 32
            radius: Theme.radiusMedium
            color: searchField.activeFocus ? Theme.surfaceElevated : Theme.surface
            border.width: 1
            border.color: searchField.activeFocus ? Theme.withAlpha(Theme.accent, 0.7) : (searchHover.hovered ? Theme.borderStrong : Theme.border)
            Behavior on Layout.preferredWidth { NumberAnimation { duration: Theme.animNormal; easing.type: Theme.easeStandard } }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: parent.radius + 3
                color: "transparent"
                border.width: 3
                border.color: Theme.withAlpha(Theme.accent, 0.12)
                opacity: searchField.activeFocus ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
            }
            Behavior on color { ColorAnimation { duration: Theme.animFast } }
            Behavior on border.color { ColorAnimation { duration: Theme.animFast } }

            MouseArea {
                id: searchHover
                property bool hovered: false
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onEntered: hovered = true
                onExited: hovered = false
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 8
                spacing: 7

                Icon {
                    name: "search"
                    iconSize: 13
                    color: searchField.activeFocus ? Theme.accent : Theme.textSecondary
                    Behavior on color { ColorAnimation { duration: Theme.animFast } }
                }

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: "Search downloads"
                    placeholderTextColor: Theme.textTertiary
                    Keys.onEscapePressed: { text = ""; focus = false }
                    selectByMouse: true
                    font.pixelSize: Theme.fontBody
                    color: Theme.textPrimary
                    verticalAlignment: TextInput.AlignVCenter
                    background: Item {}
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                }

                Rectangle {
                    visible: searchField.text.length === 0 && !searchField.activeFocus
                    Layout.preferredHeight: 18
                    Layout.preferredWidth: keyHint.implicitWidth + 10
                    radius: Theme.radiusTiny
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.border

                    Text {
                        id: keyHint
                        anchors.centerIn: parent
                        text: "Ctrl F"
                        color: Theme.textTertiary
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }
                }

                Item {
                    visible: searchField.text.length > 0
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: clearHover.hovered ? Theme.surfaceHover : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.animFast } }
                    }
                    Icon { anchors.centerIn: parent; name: "close"; iconSize: 9; color: clearHover.hovered ? Theme.textPrimary : Theme.textTertiary }
                    MouseArea {
                        id: clearHover
                        property bool hovered: false
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: hovered = true
                        onExited: hovered = false
                        onClicked: searchField.text = ""
                    }
                }
            }
        }

        ModernButton {
            Layout.leftMargin: 4
            text: root.addButtonLabel
            iconName: "plus"
            variant: "primary"
            onClicked: root.addRequested()
        }
    }
}
