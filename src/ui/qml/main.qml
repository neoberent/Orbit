import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: Math.max(minimumWidth, Prefs.windowWidth)
    height: Math.max(minimumHeight, Prefs.windowHeight)
    minimumWidth: 820
    minimumHeight: 520
    visible: false
    title: "Orbit Download Manager"
    color: Theme.canvas

    Binding { target: Theme; property: "dark"; value: appController.darkTheme }

    onClosing: (close) => {
        close.accepted = false
        trayController.hideToTray()
    }

    function rememberGeometry() {
        if (visibility === Window.Windowed) {
            Prefs.windowWidth = width
            Prefs.windowHeight = height
            Prefs.windowX = x
            Prefs.windowY = y
        }
    }

    onWidthChanged: geometryTimer.restart()
    onHeightChanged: geometryTimer.restart()
    onXChanged: geometryTimer.restart()
    onYChanged: geometryTimer.restart()
    onVisibilityChanged: geometryTimer.restart()

    Timer { id: geometryTimer; interval: 400; onTriggered: window.rememberGeometry() }

    Component.onCompleted: {
        const sx = Prefs.windowX, sy = Prefs.windowY
        if (sx >= 0 && sy >= 0 && sx < Screen.desktopAvailableWidth - 100 && sy < Screen.desktopAvailableHeight - 100) {
            x = sx
            y = sy
        }
    }

    function reveal() {
        showNormal()
        raise()
        requestActivate()
        revealAnimation.start()
        Qt.callLater(() => downloadModel.pendingFileConflicts().forEach(id => fileConflictDialog.enqueue(id)))
    }

    SequentialAnimation {
        id: revealAnimation
        PauseAnimation { duration: 170 }
        ParallelAnimation {
            NumberAnimation { target: shell; property: "opacity"; from: 0; to: 1; duration: 320; easing.type: Theme.easeStandard }
            NumberAnimation { target: shellShift; property: "y"; from: 10; to: 0; duration: 420; easing.type: Theme.easeEmphasized }
        }
    }

    SpeedHistory {
        id: speedHistory
        onSampled: {
            const s = downloadModel.summary
            const parts = []
            if (s.active > 0) parts.push(s.active + " downloading")
            if (s.seeding > 0) parts.push(s.seeding + " seeding")
            if (s.queued > 0) parts.push(s.queued + " queued")
            const rates = s.downloading || s.uploading ? "\n↓ " + s.downloadRateText + "   ↑ " + s.uploadRateText : ""
            trayController.setStatus("Orbit" + (parts.length ? " — " + parts.join(", ") : "") + rates,
                                     s.active + s.seeding > 0, s.paused > 0)
        }
    }

    Connections {
        target: trayController
        function onPauseAllRequested() { downloadModel.pauseAll() }
        function onResumeAllRequested() { downloadModel.resumeAll() }
    }

    Connections {
        target: downloadModel
        function onTransferFinished(id, name, success, detail) {
            if (!Prefs.notifyOnFinish || (window.visible && window.active)) return
            if (success) trayController.notify("Download complete", name, false)
            else trayController.notify("Download failed", name + (detail.length > 0 ? "\n" + detail : ""), true)
        }
        function onFileConflict(id, name) {
            fileConflictDialog.enqueue(id)
            if (!(window.visible && window.active))
                trayController.notify("File already exists", "“" + name + "” is waiting - open Orbit to replace it or keep both", true)
        }
        function onNotice(id, name, ok, message) {
            if (window.visible && window.active) {
                toast.show(message, ok)
            } else if (!ok || Prefs.notifyOnFinish) {
                trayController.notify(ok ? name : "Problem with " + name, message, !ok)
            }
        }
    }

    Connections {
        target: powerManager
        function onCountdownRequested(action) {
            if (!(window.visible && window.active)) {
                trayController.notify("All downloads finished", powerManager.actionLabel(action) + " in 30 seconds - open Orbit to cancel", false)
            }
            window.showNormal()
            window.raise()
            whenDoneDialog.start(action)
        }
    }

    Connections {
        target: clipboardMonitor
        function onLinksDetected(links) { clipboardPrompt.offer(links) }
    }

    ClipboardPrompt {
        id: clipboardPrompt
        onDownloadRequested: (links) => {
            const added = appController.addLinks(links)
            if (window.visible && window.active) toast.show(added === 1 ? "Download added" : added + " downloads added", true)
            else trayController.notify(added === 1 ? "Download added" : added + " downloads added", links[0], false)
        }
        onReviewRequested: (link) => {
            window.showNormal()
            window.raise()
            window.requestActivate()
            addDialog.openWithSource(link)
        }
    }

    WhenDoneDialog { id: whenDoneDialog }

    Connections {
        target: appController
        function onExternalTorrentNeedsReview(source) {
            window.showNormal()
            window.raise()
            window.requestActivate()
            addDialog.openWithSource(source)
        }
        function onLinkCaptured(id, url, withSession) {
            window.showNormal()
            window.raise()
            window.requestActivate()
        }
    }

    property string selectedCategory: "all"

    readonly property var categoryLabels: ({
        "all": "All Downloads",
        "downloading": "Downloading",
        "completed": "Completed",
        "torrents": "Torrents",
        "media": "Media"
    })

    function emptyStateTitle() {
        if (selectedCategory === "media") {
            if (table.count === 0 && topBar.searchText.length > 0) return "No matches for “" + topBar.searchText + "”"
            return "No media downloads yet"
        }
        if (downloadModel.count === 0) return "No downloads yet"
        if (table.count === 0 && topBar.searchText.length > 0) return "No matches for “" + topBar.searchText + "”"
        if (selectedCategory === "torrents") return "No torrents yet"
        if (selectedCategory === "completed") return "Nothing completed yet"
        if (selectedCategory === "downloading") return "Nothing downloading right now"
        return "Nothing here"
    }

    function emptyStateSubtitle() {
        if (selectedCategory === "media") {
            if (table.count === 0 && topBar.searchText.length > 0) return "Try a different search term or switch categories."
            return "Paste a YouTube, TikTok, Twitch (or similar) link to get started."
        }
        if (downloadModel.count === 0) return "Paste a link or drag in a torrent file to get started."
        if (table.count === 0 && topBar.searchText.length > 0) return "Try a different search term or switch categories."
        return "Items will show up here once they match this view."
    }

    function emptyStateIcon() {
        if (selectedCategory === "media") return "media"
        if (selectedCategory === "torrents") return "torrent"
        if (selectedCategory === "completed") return "completed"
        if (selectedCategory === "downloading") return "downloading"
        return "all"
    }

    function openAddDialog() {
        if (selectedCategory === "media") addDialog.openMediaMode()
        else addDialog.open()
    }

    Binding { target: downloadProxyModel; property: "filterCategory"; value: window.selectedCategory }
    Binding { target: downloadProxyModel; property: "searchText"; value: topBar.searchText }
    Binding { target: downloadProxyModel; property: "sortKey"; value: Prefs.sortKey }
    Binding { target: downloadProxyModel; property: "sortDescending"; value: Prefs.sortDescending }

    Shortcut { sequence: StandardKey.Find; onActivated: topBar.focusSearch() }
    Shortcut { sequence: StandardKey.New; enabled: !addDialog.visible && !settingsDialog.visible; onActivated: window.openAddDialog() }

    AddDownloadDialog { id: addDialog }
    SettingsPage {
        id: settingsDialog
        onUpdateDialogRequested: updateDialog.open()
    }
    UpdateDialog { id: updateDialog }
    FileConflictDialog { id: fileConflictDialog }

    Connections {
        target: updateManager
        function onUpdateFound(userInitiated) {
            if (!userInitiated && !(window.visible && window.active))
                trayController.notify("Update available", "Orbit " + updateManager.latestVersion + " is ready to download.", false)
            updateDialog.open()
        }
    }

    ColumnLayout {
        id: shell
        anchors.fill: parent
        spacing: 0
        opacity: 0
        transform: Translate { id: shellShift }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Sidebar {
                id: sidebar
                Layout.preferredWidth: Theme.sidebarWidth
                Layout.fillHeight: true
                selectedCategory: window.selectedCategory
                onCategorySelected: (category) => window.selectedCategory = category
                onSettingsRequested: settingsDialog.open()
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                TopBar {
                    id: topBar
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.headerHeight
                    pageTitle: window.categoryLabels[window.selectedCategory]
                    addButtonLabel: window.selectedCategory === "media" ? "Add Media" : "Add Download"
                    itemCount: table.count
                    selectedCount: table.selectionCount
                    onAddRequested: window.openAddDialog()
                }

                Rectangle {
                    id: contentArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.background

                    property bool dragActive: false

                    DropArea {
                        anchors.fill: parent
                        onEntered: (drag) => { if (!drag.hasUrls) drag.accepted = false; else contentArea.dragActive = true }
                        onExited: contentArea.dragActive = false
                        onDropped: (drop) => {
                            contentArea.dragActive = false
                            if (drop.hasUrls && drop.urls.length > 0) {
                                const localPath = appController.toLocalFilePath(drop.urls[0])
                                addDialog.openWithSource(localPath.length > 0 ? localPath : drop.urls[0].toString())
                            }
                        }
                    }

                    Item {
                        id: contentSwap
                        anchors.fill: parent
                        Behavior on opacity { NumberAnimation { duration: Theme.animFast; easing.type: Theme.easeStandard } }

                        DownloadTable {
                            id: table
                            anchors.fill: parent
                            visible: count > 0
                            model: downloadProxyModel
                            proxy: downloadProxyModel
                            sortKey: Prefs.sortKey
                            sortDescending: Prefs.sortDescending
                            onSortRequested: (key, descending) => {
                                Prefs.sortKey = key
                                Prefs.sortDescending = descending
                            }
                            onFeedback: (message, ok) => toast.show(message, ok)
                            onFileConflictRequested: (taskId) => fileConflictDialog.enqueue(taskId)
                        }

                        EmptyState {
                            anchors.fill: parent
                            anchors.margins: Theme.spaceLg
                            visible: table.count === 0
                            dragActive: contentArea.dragActive
                            iconName: window.emptyStateIcon()
                            title: window.emptyStateTitle()
                            subtitle: window.emptyStateSubtitle()
                            isMediaContext: window.selectedCategory === "media"
                            actionLabel: window.selectedCategory === "media" ? "Add Media" : "Add Download"
                            showAction: table.count === 0 && topBar.searchText.length === 0
                            showGuide: downloadModel.count === 0 || window.selectedCategory === "media" && table.count === 0 && topBar.searchText.length === 0
                            onActionRequested: window.openAddDialog()
                        }

                        Toast {
                            id: toast
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 14
                        }

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 8
                            visible: table.count > 0
                            radius: Theme.radiusXl
                            color: Theme.withAlpha(Theme.accent, contentArea.dragActive ? 0.05 : 0.0)
                            border.width: contentArea.dragActive ? 1.5 : 0
                            border.color: Theme.withAlpha(Theme.accent, 0.5)
                            opacity: contentArea.dragActive ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
                        }
                    }
                }

                DetailsPane {
                    id: details
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    visible: table.count > 0 && table.hasSelection
                    table: table
                    speedHistory: speedHistory
                }
            }
        }

        StatusBar {
            speedHistory: speedHistory
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.statusBarHeight
        }
    }

    onSelectedCategoryChanged: {
        table.clearSelection()
        sectionTransition.restart()
    }

    SequentialAnimation {
        id: sectionTransition
        ScriptAction { script: contentSwap.opacity = 0.6 }
        PauseAnimation { duration: 30 }
        ScriptAction { script: contentSwap.opacity = 1.0 }
    }
}
