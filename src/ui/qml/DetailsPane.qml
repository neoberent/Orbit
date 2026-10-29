import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property Item table: null
    property var speedHistory: null

    readonly property int selectionCount: table ? table.selectionCount : 0
    readonly property bool multi: selectionCount > 1
    readonly property var item: table && selectionCount === 1 && downloadModel.revision >= 0 ? downloadModel.get(table.focusId) : null
    readonly property var group: {
        if (!table || !multi || downloadModel.revision < 0) return []
        return table.selectedIds().map(id => downloadModel.get(id)).filter(it => it.taskId !== undefined)
    }

    readonly property bool valid: item !== null && item.taskId !== undefined
    readonly property string stateName: valid ? item.stateLabel : ""
    readonly property bool finished: stateName === "Completed" || stateName === "Cancelled"
    readonly property bool failed: stateName === "Error"
    readonly property bool conflicted: valid && item.fileConflictPath.length > 0
    // Signed or time-limited links typically fail with one of these once they expire.
    readonly property bool linkLooksExpired: failed && valid && item.canRefreshLink && /HTTP (401|403|404|410|498)\b/.test(item.errorMessage)
    readonly property string kind: valid ? Theme.fileKind(item.name, item.isTorrent, item.isMedia) : "file"
    readonly property string extension: valid ? Theme.fileExtension(item.name).toUpperCase() : ""
    readonly property string percentText: valid ? Theme.percentText(item.progress) : ""

    readonly property var samples: speedHistory && speedHistory.revision >= 0
                                   ? (multi ? speedHistory.combined(table.selectedIds()) : (valid ? speedHistory.forId(item.taskId) : []))
                                   : []
    readonly property real average: {
        let sum = 0, n = 0
        for (let i = 0; i < samples.length; ++i) { if (samples[i] > 0) { sum += samples[i]; n++ } }
        return n > 0 ? sum / n : 0
    }

    function dash(value) { return value === undefined || value === "" || value === "--" || value === "Unknown" ? "—" : value }

    property int torrentTab: 0
    readonly property bool torrentView: !multi && valid && item.isTorrent
    readonly property bool showTorrentPage: torrentView && torrentTab > 0
    property var pieceCells: []
    property int pieceCount: 0

    onTorrentViewChanged: if (!torrentView) torrentTab = 0

    function refreshPieces() {
        if (!torrentView || torrentTab !== 0) { pieceCells = []; return }
        pieceCells = downloadModel.torrentPieceMap(item.taskId, 240)
        pieceCount = downloadModel.torrentPieceCount(item.taskId)
    }

    Timer {
        interval: 2000
        repeat: true
        triggeredOnStart: true
        running: root.visible && root.torrentView && root.torrentTab === 0
        onTriggered: root.refreshPieces()
    }

    function checksumText(state) {
        switch (state) {
        case "pending": return "Checked when done"
        case "verifying": return "Verifying…"
        case "match": return "Verified"
        case "mismatch": return "Mismatch"
        case "failed": return "Couldn't verify"
        default: return ""
        }
    }

    readonly property var stats: {
        if (multi) {
            let total = 0, done = 0, rate = 0, active = 0, paused = 0, finishedCount = 0, failedCount = 0
            for (const it of group) {
                total += it.totalBytes
                done += it.downloadedBytes
                rate += it.speedBytes
                if (it.isActive) active++
                else if (it.isPaused) paused++
                else if (it.stateLabel === "Error") failedCount++
                else if (it.stateLabel === "Completed") finishedCount++
            }
            const list = [
                { label: "Total size", value: total > 0 ? Theme.formatBytes(total) : "—" },
                { label: "Downloaded", value: Theme.formatBytes(done) + (total > 0 ? "  (" + Theme.percentText(done / total) + ")" : "") },
                { label: "Combined speed", value: rate > 0 ? Theme.formatRate(rate) : "—" }
            ]
            const parts = []
            if (active) parts.push(active + " active")
            if (paused) parts.push(paused + " paused")
            if (finishedCount) parts.push(finishedCount + " completed")
            if (failedCount) parts.push(failedCount + " failed")
            list.push({ label: "Status", value: parts.length ? parts.join(", ") : "Idle" })
            return list
        }
        if (!valid) return []
        const list = [
            { label: "Size", value: dash(item.totalText) },
            { label: "Downloaded", value: item.downloadedText + (item.totalText !== "Unknown" && !finished ? "  (" + percentText + ")" : "") },
            { label: "Speed", value: stateName === "Downloading" ? dash(item.speedText) : "—" },
            { label: "Time left", value: stateName === "Downloading" ? dash(item.etaText) : "—" }
        ]
        if (item.isTorrent) {
            list.push({ label: "Upload", value: dash(item.uploadSpeedText) })
            list.push({ label: "Peers", value: item.peers + " peers · " + item.seeds + " seeds" })
        } else if (!item.isMedia && item.isActive) {
            list.push({ label: "Connections", value: String(item.connections) })
        }
        if (item.speedLimitKBps > 0) list.push({ label: "Speed limit", value: item.speedLimitKBps + " KB/s" })
        if (item.isTorrent && item.uploadLimitKBps > 0) list.push({ label: "Upload limit", value: item.uploadLimitKBps + " KB/s" })
        if (item.scheduledText && item.scheduledText.length > 0) list.push({ label: "Starts", value: item.scheduledText })
        if (item.checksumState && item.checksumState.length > 0) list.push({ label: "Checksum", value: checksumText(item.checksumState), tone: item.checksumState })
        if (item.hasBrowserSession) list.push({ label: "Sign-in", value: "Browser session" })
        if (item.addedText && item.addedText.length > 0) list.push({ label: "Added", value: item.addedText })
        if (item.dateText && item.dateText.length > 0) list.push({ label: "Finished", value: item.dateText })
        return list
    }

    color: Theme.panel
    implicitHeight: Math.max(content.implicitHeight, graphColumn.visible ? 150 : 0) + 28

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    TextEdit { id: clipboardHelper; visible: false }

    component FieldLabel: Text {
        color: Theme.textTertiary
        font.pixelSize: Theme.fontMicro
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.trackingWider
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 12
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        spacing: 20

        ColumnLayout {
            id: content
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                FileTile {
                    visible: !root.multi
                    size: 38
                    fileName: root.valid ? root.item.name : ""
                    isTorrent: root.valid && root.item.isTorrent
                    isMedia: root.valid && root.item.isMedia
                    muted: root.stateName === "Cancelled"
                }

                Rectangle {
                    visible: root.multi
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    radius: 9
                    color: Theme.withAlpha(Theme.accent, 0.12)
                    border.width: 1
                    border.color: Theme.withAlpha(Theme.accent, 0.24)
                    Text {
                        anchors.centerIn: parent
                        text: root.selectionCount
                        color: Theme.accent
                        font.pixelSize: 14
                        font.weight: Font.Bold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        Layout.fillWidth: true
                        text: root.multi ? root.selectionCount + " downloads selected" : (root.valid ? root.item.name : "")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontHeading
                        font.weight: Font.DemiBold
                        font.letterSpacing: Theme.trackingTight
                        elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.multi
                              ? "Ctrl+click to add or remove items, Shift+click to select a range"
                              : root.conflicted
                                ? "A file with this name already exists in " + root.item.destinationDir
                              : root.failed && root.item.errorMessage.length > 0
                                ? root.item.errorMessage + (root.linkLooksExpired ? " - the link may have expired" : "")
                                : (root.extension.length > 0 && root.valid && !root.item.isTorrent && !root.item.isMedia
                                   ? root.extension + " " + Theme.kindLabel(root.kind).toLowerCase()
                                   : Theme.kindLabel(root.kind))
                                  + (root.valid ? "  ·  " + Theme.sourceHost(root.item.source, root.item.isTorrent) : "")
                        color: root.failed && !root.multi ? Theme.danger : Theme.textTertiary
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                }

                ModernButton {
                    visible: !root.multi && (root.linkLooksExpired || root.conflicted)
                    text: root.conflicted ? "Choose…" : "Refresh Link…"
                    variant: "secondary"
                    iconName: root.conflicted ? "alert" : "link"
                    onClicked: {
                        if (root.conflicted) root.table.fileConflictRequested(root.item.taskId)
                        else root.table.openRefreshLink(root.item)
                    }
                }
                StatusBadge {
                    visible: !root.multi
                    label: root.failed ? (root.item.checksumState === "mismatch" ? "Corrupt" : "Failed")
                           : root.conflicted ? "File exists"
                           : root.stateName === "Paused" && root.item.scheduledText.length > 0 ? "Scheduled" : root.stateName
                    tone: root.conflicted ? Theme.warning : Theme.stateColor(root.stateName)
                }

                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.border; Layout.leftMargin: 4; Layout.rightMargin: 2 }

                IconButton {
                    visible: root.multi || root.stateName !== "Completed"
                    iconName: root.multi ? "play"
                              : root.failed || root.stateName === "Cancelled" ? "retry" : (root.valid && root.item.isPaused ? "play" : "pause")
                    enabled: root.multi ? root.group.some(it => it.isPaused || it.stateLabel === "Error" || it.stateLabel === "Cancelled")
                                        : (root.valid && (root.item.isActive || root.item.isPaused || root.failed || root.stateName === "Cancelled"))
                    tooltip: root.multi ? "Resume selected"
                             : root.failed || root.stateName === "Cancelled" ? "Retry" : (root.valid && root.item.isPaused ? "Resume" : "Pause")
                    onClicked: {
                        if (root.multi) { root.table.resumeSelected(); root.table.retrySelected(); return }
                        if (root.conflicted) { root.table.fileConflictRequested(root.item.taskId); return }
                        if (root.failed || root.stateName === "Cancelled" || root.item.isPaused) downloadModel.resumeItem(root.item.taskId)
                        else downloadModel.pauseItem(root.item.taskId)
                    }
                }
                IconButton {
                    visible: root.multi
                    iconName: "pause"
                    tooltip: "Pause selected"
                    enabled: root.group.some(it => it.isActive)
                    onClicked: root.table.pauseSelected()
                }
                IconButton {
                    visible: !root.multi && root.stateName === "Completed"
                    iconName: "open"
                    tooltip: "Open file"
                    onClicked: downloadModel.openFile(root.item.taskId)
                }
                IconButton {
                    visible: !root.multi
                    iconName: "edit"
                    tooltip: "Rename"
                    onClicked: root.table.openRename(root.item)
                }
                IconButton {
                    visible: !root.multi && root.valid && root.item.canRefreshLink && !root.linkLooksExpired
                    iconName: "link"
                    tooltip: "Refresh link"
                    onClicked: root.table.openRefreshLink(root.item)
                }
                IconButton {
                    visible: !root.multi
                    iconName: "sliders"
                    tooltip: "Download options"
                    onClicked: root.table.openOptions(root.item)
                }
                IconButton {
                    visible: !root.multi
                    iconName: "folder"
                    tooltip: "Open containing folder"
                    onClicked: downloadModel.openContainingFolder(root.item.taskId)
                }
                IconButton {
                    visible: root.multi
                    iconName: "trash"
                    danger: true
                    tooltip: "Remove selected from list"
                    enabled: root.group.some(it => !it.isActive)
                    onClicked: root.table.removeSelected()
                }
                IconButton {
                    iconName: "close"
                    tooltip: "Close details"
                    onClicked: root.table.clearSelection()
                }
            }

            TabStrip {
                visible: root.torrentView
                tabs: ["Overview", "Files", "Peers", "Trackers"]
                currentIndex: root.torrentTab
                onSelected: (index) => root.torrentTab = index
            }

            TorrentDetailsView {
                visible: root.showTorrentPage
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                taskId: root.torrentView ? root.item.taskId : -1
                page: root.torrentTab === 1 ? "files" : root.torrentTab === 2 ? "peers" : "trackers"
            }

            ProgressIndicator {
                visible: !root.showTorrentPage && !root.multi && !root.finished && root.stateName !== "Seeding" && !(root.failed && root.valid && root.item.progress <= 0)
                Layout.fillWidth: true
                height: 4
                value: root.valid ? root.item.progress : 0
                trackColor: Theme.withAlpha(Theme.textPrimary, 0.06)
                fillColor: root.stateName === "Downloading" ? Theme.accent : (root.failed ? Theme.withAlpha(Theme.danger, 0.7) : Theme.withAlpha(Theme.textSecondary, 0.55))
                indeterminate: root.stateName === "Connecting"
            }

            Flow {
                visible: !root.showTorrentPage
                Layout.fillWidth: true
                spacing: 0

                Repeater {
                    model: root.stats

                    Item {
                        id: statCell
                        required property var modelData
                        required property int index
                        readonly property bool startsLine: x <= 0
                        width: Math.max(104, cell.implicitWidth + 28)
                        height: cell.implicitHeight + 6

                        Rectangle {
                            visible: !statCell.startsLine
                            x: -1
                            width: 1
                            height: cell.implicitHeight
                            color: Theme.border
                        }

                        ColumnLayout {
                            id: cell
                            x: statCell.startsLine ? 0 : 14
                            spacing: 3
                            FieldLabel { text: modelData.label.toUpperCase() }
                            Text {
                                text: modelData.value
                                color: modelData.value === "—" ? Theme.textDisabled
                                       : modelData.tone === "match" ? Theme.success
                                       : modelData.tone === "mismatch" || modelData.tone === "failed" ? Theme.danger
                                       : Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.Medium
                                font.features: { "tnum": 1 }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                visible: root.torrentView && !root.showTorrentPage && root.pieceCells.length > 0
                Layout.fillWidth: true
                spacing: 4
                FieldLabel { text: "PIECES · " + root.pieceCount }
                PieceMap {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 10
                    cells: root.pieceCells
                    fillColor: root.stateName === "Seeding" ? Theme.seeding : Theme.accent
                }
            }

            GridLayout {
                visible: !root.multi && !root.showTorrentPage
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 12
                rowSpacing: 4

                FieldLabel { text: "SOURCE"; Layout.preferredWidth: 64 }
                Text {
                    Layout.fillWidth: true
                    text: root.valid ? root.item.source : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    font.family: Theme.monoFamily
                    elide: Text.ElideMiddle
                }
                IconButton {
                    implicitWidth: 24
                    implicitHeight: 24
                    iconName: "copy"
                    tooltip: "Copy source link"
                    enabled: root.valid && root.item.source.length > 0
                    onClicked: {
                        clipboardHelper.text = root.item.source
                        clipboardHelper.selectAll()
                        clipboardHelper.copy()
                    }
                }

                FieldLabel { text: "SAVED TO"; Layout.preferredWidth: 64 }
                Text {
                    Layout.fillWidth: true
                    text: root.valid ? (root.item.destination.length > 0 ? root.item.destination : "—") : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    font.family: Theme.monoFamily
                    elide: Text.ElideMiddle
                }
                IconButton {
                    implicitWidth: 24
                    implicitHeight: 24
                    iconName: "copy"
                    tooltip: "Copy file path"
                    enabled: root.valid && root.item.destination.length > 0
                    onClicked: {
                        clipboardHelper.text = root.item.destination
                        clipboardHelper.selectAll()
                        clipboardHelper.copy()
                    }
                }
            }
        }

        ColumnLayout {
            id: graphColumn
            visible: root.width >= 880
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                FieldLabel { text: "ACTIVITY · LAST 2 MIN"; Layout.fillWidth: true }
                Text {
                    text: graph.peak > 0 ? "Peak " + Theme.formatRate(graph.peak) : "No activity"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontMicro
                    font.features: { "tnum": 1 }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 80
                radius: Theme.radiusMedium
                color: Theme.surfaceInset
                border.width: 1
                border.color: Theme.border
                clip: true

                SpeedGraph {
                    id: graph
                    anchors.fill: parent
                    anchors.margins: 1
                    samples: root.samples
                    capacity: root.speedHistory ? root.speedHistory.capacity : 120
                    lineColor: root.valid && root.stateName === "Seeding" ? Theme.seeding : Theme.accent
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 6
                    visible: graph.peak > 0
                    text: Theme.formatRate(graph.scaleMax)
                    color: Theme.textDisabled
                    font.pixelSize: 10
                    font.features: { "tnum": 1 }
                }
            }

            Text {
                text: root.average > 0 ? "Average " + Theme.formatRate(root.average) + " while active" : " "
                color: Theme.textTertiary
                font.pixelSize: Theme.fontMicro
                font.features: { "tnum": 1 }
            }
        }
    }
}
