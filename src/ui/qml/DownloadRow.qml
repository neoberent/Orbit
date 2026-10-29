import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property Item table

    width: ListView.view ? ListView.view.width : 0
    height: Theme.rowHeight

    Accessible.role: Accessible.ListItem
    Accessible.name: name
    Accessible.description: badgeLabel + (showBar && stateLabel !== "Queued" ? ", " + percentText : "")
    Accessible.selected: selected
    Accessible.onPressAction: if (table) table.handlePress(index, taskId, 0, Qt.LeftButton)

    readonly property bool selected: table !== null && table.selectionRevision >= 0 && table.selection[taskId] === true
    readonly property bool hovered: hover.hovered
    readonly property bool finished: stateLabel === "Completed" || stateLabel === "Cancelled"
    readonly property bool failed: stateLabel === "Error"
    readonly property bool showBar: !finished && stateLabel !== "Seeding" && !(failed && progress <= 0)
    readonly property color tone: Theme.stateColor(stateLabel)
    readonly property string kind: Theme.fileKind(name, isTorrent, isMedia)
    readonly property string extension: Theme.fileExtension(name).toUpperCase()
    readonly property string host: Theme.sourceHost(source, isTorrent)
    readonly property string percentText: Theme.percentText(progress)
    readonly property bool sizeKnown: totalText !== "Unknown"
    readonly property bool scheduled: stateLabel === "Paused" && scheduledText.length > 0
    readonly property bool conflicted: fileConflictPath.length > 0
    readonly property string badgeLabel: stateLabel === "Error" ? (checksumState === "mismatch" ? "Corrupt" : "Failed")
                                         : conflicted ? "File exists"
                                         : scheduled ? "Scheduled"
                                         : checksumState === "verifying" ? "Verifying" : stateLabel

    readonly property string secondaryText: {
        const parts = []
        parts.push(extension.length > 0 && !isTorrent && !isMedia ? extension + " " + Theme.kindLabel(kind).toLowerCase() : Theme.kindLabel(kind))
        if (host.length > 0) parts.push(host)
        return parts.join("  ·  ")
    }

    readonly property string progressDetail: {
        switch (stateLabel) {
        case "Queued": return "Waiting for a free slot"
        case "Connecting": return "Connecting to source…"
        case "Error":
            if (checksumState === "mismatch") return "Downloaded, but the checksum didn't match"
            return sizeKnown ? "Stopped at " + downloadedText + " of " + totalText : "Stopped at " + downloadedText
        case "Paused":
            if (conflicted) return "Waiting for your choice"
            if (scheduled) return "Starts " + scheduledText
        default: return sizeKnown ? downloadedText + " of " + totalText : downloadedText
        }
    }

    function primaryAction() {
        if (conflicted && table) { table.fileConflictRequested(taskId); return }
        if (failed || stateLabel === "Cancelled") downloadModel.resumeItem(taskId)
        else if (isPaused) downloadModel.resumeItem(taskId)
        else if (isActive) downloadModel.pauseItem(taskId)
    }

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.rowSelected : (root.hovered ? Theme.rowHover : "transparent")
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 2
        color: Theme.accent
        opacity: root.selected ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 16
        height: 1
        color: Theme.borderSoft
    }

    HoverHandler { id: hover }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (mouse) => {
            if (root.table) root.table.handlePress(index, taskId, mouse.modifiers, mouse.button)
        }
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton && root.table) root.table.openPrimary(taskId)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: root.table ? root.table.rightPad : 12
        spacing: 0

        FileTile {
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: 12
            fileName: model.name
            isTorrent: model.isTorrent
            isMedia: model.isMedia
            muted: model.stateLabel === "Cancelled"
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: 16
            spacing: 3

            Text {
                Layout.fillWidth: true
                text: name
                color: stateLabel === "Cancelled" ? Theme.textSecondary : Theme.textPrimary
                font.pixelSize: 13
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.trackingTight
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 5

                Icon {
                    visible: root.failed
                    name: "alert"
                    iconSize: 11
                    color: Theme.danger
                }
                Text {
                    Layout.fillWidth: true
                    text: root.failed && errorMessage.length > 0 ? errorMessage : root.secondaryText
                    color: root.failed ? Theme.danger : Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }

            ProgressIndicator {
                visible: root.table && root.table.compact && root.showBar
                Layout.fillWidth: true
                Layout.topMargin: 2
                height: 3
                value: progress
                fillColor: stateLabel === "Downloading" ? root.tone : Theme.textTertiary
                indeterminate: stateLabel === "Connecting"
            }
        }

        Item {
            visible: root.table && root.table.wProgress > 0
            Layout.preferredWidth: root.table ? root.table.wProgress : 0
            Layout.fillHeight: true

            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                ProgressIndicator {
                    visible: root.showBar
                    Layout.fillWidth: true
                    height: 5
                    value: progress
                    trackColor: Theme.withAlpha(Theme.textPrimary, 0.06)
                    fillColor: stateLabel === "Downloading" ? Theme.accent
                               : root.failed ? Theme.withAlpha(Theme.danger, 0.7)
                               : Theme.withAlpha(Theme.textSecondary, 0.55)
                    indeterminate: stateLabel === "Connecting"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        Layout.fillWidth: true
                        text: root.finished || stateLabel === "Seeding"
                              ? (stateLabel === "Cancelled" ? "Cancelled at " + root.percentText : (root.sizeKnown ? totalText : downloadedText))
                              : root.progressDetail
                        color: root.finished || stateLabel === "Seeding" ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: root.finished || stateLabel === "Seeding" ? Theme.fontBody : Theme.fontCaption
                        font.weight: root.finished || stateLabel === "Seeding" ? Font.Medium : Font.Normal
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: root.showBar && stateLabel !== "Queued" && stateLabel !== "Connecting"
                        text: root.percentText
                        color: stateLabel === "Downloading" ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.DemiBold
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }

        ColumnLayout {
            visible: root.table && root.table.wSpeed > 0
            Layout.preferredWidth: root.table ? root.table.wSpeed : 0
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            Text {
                text: stateLabel === "Seeding" ? (uploadSpeedText !== "--" ? "↑ " + uploadSpeedText : "—")
                      : (speedText !== "--" && stateLabel === "Downloading" ? speedText : "—")
                color: text === "—" ? Theme.textDisabled : Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
            }
            Text {
                visible: isTorrent && (isActive || isPaused)
                Layout.maximumWidth: root.table ? root.table.wSpeed - 10 : 90
                text: peers + (peers === 1 ? " peer" : " peers")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontMicro
                elide: Text.ElideRight
            }
        }

        ColumnLayout {
            visible: root.table && root.table.wTime > 0
            Layout.preferredWidth: root.table ? root.table.wTime : 0
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            readonly property bool showEta: stateLabel === "Downloading" && etaText !== "--"
            readonly property bool showDate: (stateLabel === "Completed" || stateLabel === "Seeding") && dateText.length > 0

            Text {
                text: parent.showEta ? etaText : (parent.showDate ? dateText : "—")
                color: text === "—" ? Theme.textDisabled : Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
                elide: Text.ElideRight
                Layout.maximumWidth: root.table ? root.table.wTime - 12 : 80
            }
            Text {
                visible: parent.showEta || parent.showDate
                text: parent.showEta ? "remaining" : "finished"
                color: Theme.textTertiary
                font.pixelSize: Theme.fontMicro
            }
        }

        Item {
            Layout.preferredWidth: root.table ? root.table.wStatus : 110
            Layout.fillHeight: true

            StatusBadge {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                label: root.badgeLabel
                tone: root.conflicted ? Theme.warning : root.scheduled ? Theme.accent : root.tone
            }
        }

        Row {
            Layout.preferredWidth: root.table ? root.table.wActions : 90
            Layout.alignment: Qt.AlignVCenter
            layoutDirection: Qt.RightToLeft
            spacing: 2
            opacity: root.hovered || root.selected ? 1.0 : 0.45
            Behavior on opacity { NumberAnimation { duration: Theme.animNormal; easing.type: Theme.easeStandard } }

            IconButton {
                iconName: root.finished || root.failed ? "trash" : "close"
                danger: true
                tooltip: root.finished || root.failed ? "Remove from list" : "Cancel"
                onClicked: {
                    if (root.finished || root.failed) downloadModel.removeItem(taskId)
                    else downloadModel.cancelItem(taskId)
                }
            }
            IconButton {
                iconName: "folder"
                tooltip: "Open containing folder"
                onClicked: downloadModel.openContainingFolder(taskId)
            }
            IconButton {
                visible: stateLabel !== "Completed"
                iconName: root.failed || stateLabel === "Cancelled" ? "retry" : (isPaused ? "play" : "pause")
                enabled: isActive || isPaused || root.failed || stateLabel === "Cancelled"
                tooltip: root.failed || stateLabel === "Cancelled" ? "Retry" : (isPaused ? "Resume" : "Pause")
                onClicked: root.primaryAction()
            }
        }
    }
}
