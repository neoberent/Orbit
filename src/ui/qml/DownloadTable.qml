import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property alias model: listView.model
    property alias count: listView.count
    property var proxy: null

    property string sortKey: "added"
    property bool sortDescending: false
    signal sortRequested(string key, bool descending)
    signal feedback(string message, bool ok)
    signal fileConflictRequested(var taskId)

    property var selection: ({})
    property int selectionCount: 0
    property int selectionRevision: 0
    property var anchorId: -1
    property var focusId: -1

    readonly property bool hasSelection: selectionCount > 0

    readonly property bool compact: width < 620
    readonly property real wProgress: compact ? 0 : (width >= 1000 ? 220 : 180)
    readonly property real wSpeed: width >= 720 ? 100 : 0
    readonly property real wTime: width >= 830 ? 100 : 0
    readonly property real wStatus: 112
    readonly property real wActions: 92
    readonly property real rightPad: 12

    readonly property var sortOptions: [
        { key: "added", label: "Date added" },
        { key: "name", label: "Name" },
        { key: "size", label: "Size" },
        { key: "progress", label: "Progress" },
        { key: "speed", label: "Speed" },
        { key: "time", label: "Time left" },
        { key: "status", label: "Status" }
    ]

    function setSelection(ids, focus) {
        const map = {}
        for (let i = 0; i < ids.length; ++i) map[ids[i]] = true
        selection = map
        selectionCount = ids.length
        focusId = ids.length > 0 ? (focus !== undefined && map[focus] ? focus : ids[ids.length - 1]) : -1
        selectionRevision++
    }

    function selectedIds() {
        return Object.keys(selection).map(Number)
    }

    function isSelected(id) { return selection[id] === true }

    function clearSelection() {
        setSelection([], -1)
        anchorId = -1
    }

    function selectAll() {
        if (!proxy) return
        const ids = proxy.allIds()
        setSelection(ids, focusId)
        if (anchorId < 0 && ids.length > 0) anchorId = ids[0]
    }

    function prune() {
        if (!proxy || selectionCount === 0) return
        const visible = proxy.allIds()
        const kept = visible.filter(id => selection[id] === true)
        if (kept.length !== selectionCount) setSelection(kept, focusId)
    }

    function handlePress(row, id, modifiers, button) {
        if (button === Qt.RightButton) {
            if (!isSelected(id)) {
                setSelection([id], id)
                anchorId = id
            }
        } else if (modifiers & Qt.ControlModifier) {
            const ids = selectedIds()
            const at = ids.indexOf(id)
            if (at >= 0) ids.splice(at, 1)
            else ids.push(id)
            setSelection(ids, at >= 0 ? focusId : id)
            anchorId = id
        } else if ((modifiers & Qt.ShiftModifier) && anchorId >= 0 && proxy) {
            const anchorRow = proxy.rowOfId(anchorId)
            setSelection(proxy.idsBetween(anchorRow >= 0 ? anchorRow : row, row), id)
        } else {
            setSelection([id], id)
            anchorId = id
        }
        listView.currentIndex = row
        listView.forceActiveFocus()
        if (button === Qt.RightButton) openMenu()
    }

    function moveFocus(delta, extend) {
        if (!proxy || listView.count === 0) return
        const current = focusId >= 0 ? proxy.rowOfId(focusId) : -1
        let row = current < 0 ? (delta > 0 ? 0 : listView.count - 1) : current + delta
        row = Math.max(0, Math.min(listView.count - 1, row))
        const id = proxy.idAt(row)
        if (extend && anchorId >= 0) {
            setSelection(proxy.idsBetween(proxy.rowOfId(anchorId), row), id)
        } else {
            setSelection([id], id)
            anchorId = id
        }
        listView.currentIndex = row
        listView.positionViewAtIndex(row, ListView.Contain)
    }

    function items() {
        return selectedIds().map(id => downloadModel.get(id)).filter(it => it.taskId !== undefined)
    }

    function isRetryable(it) { return it.stateLabel === "Error" || it.stateLabel === "Cancelled" }
    function isRemovable(it) { return !it.isActive }
    function isCancellable(it) { return it.stateLabel !== "Completed" && it.stateLabel !== "Cancelled" && it.stateLabel !== "Error" }

    function pauseSelected() { items().forEach(it => { if (it.isActive) downloadModel.pauseItem(it.taskId) }) }
    function resumeSelected() { items().forEach(it => { if (it.isPaused) downloadModel.resumeItem(it.taskId) }) }
    function retrySelected() { items().forEach(it => { if (root.isRetryable(it)) downloadModel.resumeItem(it.taskId) }) }
    function cancelSelected() { items().forEach(it => { if (root.isCancellable(it)) downloadModel.cancelItem(it.taskId) }) }
    function removeSelected() { items().forEach(it => { if (root.isRemovable(it)) downloadModel.removeItem(it.taskId) }) }

    function toggleSelected() {
        const list = items()
        if (list.some(it => it.isActive)) pauseSelected()
        else if (list.some(it => it.isPaused)) resumeSelected()
        else retrySelected()
    }

    function openPrimary(id) {
        const it = downloadModel.get(id)
        if (it.stateLabel === "Completed" && downloadModel.openFile(id)) return
        downloadModel.openContainingFolder(id)
    }

    function confirmDeleteFiles() {
        const removable = items().filter(it => root.isRemovable(it))
        if (removable.length === 0) return
        const one = removable.length === 1
        confirmDialog.ask(one ? "Delete “" + removable[0].name + "”?" : "Delete " + removable.length + " downloads?",
                          (one ? "The download is removed from the list and its file is deleted from disk."
                               : "The downloads are removed from the list and their files are deleted from disk.")
                          + " This can't be undone.",
                          one ? "Delete File" : "Delete Files", true,
                          () => downloadModel.removeWithFiles(removable.map(it => it.taskId)))
    }

    function copyText(text) {
        clipboardHelper.text = text
        clipboardHelper.selectAll()
        clipboardHelper.copy()
    }

    function openRename(item) { renameDialog.openFor(item) }
    function openOptions(item) { optionsDialog.openFor(item) }
    function openChecksum(item) { checksumDialog.openFor(item) }
    function openRefreshLink(item) { refreshLinkDialog.openFor(item) }

    function moveSelected() {
        const list = items().filter(it => it.isTorrent || !it.isActive)
        if (list.length === 0) return
        const dir = appController.pickFolder("Move To", list[0].destinationDir)
        if (dir.length === 0) return
        const errors = []
        for (const it of list) {
            const error = downloadModel.moveItem(it.taskId, dir)
            if (error.length > 0) errors.push(list.length > 1 ? it.name + ": " + error : error)
        }
        if (errors.length > 0) root.feedback(errors[0] + (errors.length > 1 ? " (+" + (errors.length - 1) + " more)" : ""), false)
    }

    function startNow() { items().forEach(it => { if (it.isPaused) downloadModel.resumeItem(it.taskId) }) }

    property var menuState: ({})

    function openMenu() {
        const list = items()
        const single = list.length === 1 ? list[0] : null
        menuState = {
            count: list.length,
            single: single,
            canOpenFile: single !== null && single.stateLabel === "Completed",
            canPause: list.filter(it => it.isActive).length,
            canResume: list.filter(it => it.isPaused).length,
            canRetry: list.filter(it => root.isRetryable(it)).length,
            canCancel: list.filter(it => root.isCancellable(it)).length,
            canRemove: list.filter(it => root.isRemovable(it)).length,
            canLimit: single !== null,
            canRename: single !== null && (single.isTorrent || !single.isActive),
            canMove: list.some(it => it.isTorrent || !it.isActive),
            canVerify: single !== null && !single.isTorrent && single.stateLabel === "Completed",
            canRefreshLink: single !== null && single.canRefreshLink === true,
            hasConflict: single !== null && single.fileConflictPath.length > 0,
            isTorrent: single !== null && single.isTorrent,
            torrentLive: single !== null && single.isTorrent && (single.isActive || single.isPaused || single.stateLabel === "Completed")
        }
        contextMenu.popup()
    }

    function countLabel(verb, n) {
        return menuState.count > 1 && n > 0 ? verb + " " + n + (n === 1 ? " Download" : " Downloads") : verb
    }

    TextEdit { id: clipboardHelper; visible: false }
    DownloadOptionsDialog { id: optionsDialog; parent: Overlay.overlay }
    RenameDialog { id: renameDialog; parent: Overlay.overlay }
    ChecksumDialog { id: checksumDialog; parent: Overlay.overlay }
    RefreshLinkDialog {
        id: refreshLinkDialog
        parent: Overlay.overlay
        onRefreshed: (message) => root.feedback(message, true)
    }
    ConfirmDialog { id: confirmDialog; parent: Overlay.overlay }

    component MenuDivider: MenuSeparator {
        contentItem: Rectangle { implicitHeight: 1; color: Theme.border }
        topPadding: 4
        bottomPadding: 4
    }

    OrbitMenu {
        id: contextMenu

        OrbitMenuItem {
            text: "Open File"
            iconName: "open"
            visible: root.menuState.canOpenFile === true
            height: visible ? implicitHeight : 0
            onTriggered: downloadModel.openFile(root.menuState.single.taskId)
        }
        OrbitMenuItem {
            text: "Open Containing Folder"
            iconName: "folder"
            visible: root.menuState.single !== null && root.menuState.single !== undefined
            height: visible ? implicitHeight : 0
            onTriggered: downloadModel.openContainingFolder(root.menuState.single.taskId)
        }
        OrbitMenuItem {
            text: "File Already Exists…"
            iconName: "alert"
            visible: root.menuState.hasConflict === true
            height: visible ? implicitHeight : 0
            onTriggered: root.fileConflictRequested(root.menuState.single.taskId)
        }
        MenuDivider { visible: root.menuState.count === 1; height: visible ? implicitHeight : 0 }
        OrbitMenuItem {
            text: root.countLabel("Resume", root.menuState.canResume)
            iconName: "play"
            visible: root.menuState.canResume > 0
            height: visible ? implicitHeight : 0
            onTriggered: root.resumeSelected()
        }
        OrbitMenuItem {
            text: root.countLabel("Pause", root.menuState.canPause)
            iconName: "pause"
            visible: root.menuState.canPause > 0
            height: visible ? implicitHeight : 0
            onTriggered: root.pauseSelected()
        }
        OrbitMenuItem {
            text: root.countLabel("Retry", root.menuState.canRetry)
            iconName: "retry"
            visible: root.menuState.canRetry > 0
            height: visible ? implicitHeight : 0
            onTriggered: root.retrySelected()
        }
        MenuDivider { visible: root.menuState.count === 1; height: visible ? implicitHeight : 0 }
        OrbitMenuItem {
            text: "Rename…"
            iconName: "edit"
            visible: root.menuState.count === 1
            height: visible ? implicitHeight : 0
            enabled: root.menuState.canRename === true
            onTriggered: root.openRename(root.menuState.single)
        }
        OrbitMenuItem {
            text: "Refresh Link…"
            iconName: "link"
            visible: root.menuState.canRefreshLink === true
            height: visible ? implicitHeight : 0
            onTriggered: root.openRefreshLink(root.menuState.single)
        }
        OrbitMenuItem {
            text: "Move To…"
            iconName: "folder"
            enabled: root.menuState.canMove === true
            onTriggered: root.moveSelected()
        }
        OrbitMenuItem {
            text: "Download Options…"
            iconName: "sliders"
            visible: root.menuState.canLimit === true
            height: visible ? implicitHeight : 0
            onTriggered: root.openOptions(root.menuState.single)
        }
        OrbitMenuItem {
            text: "Verify Checksum…"
            iconName: "shield"
            visible: root.menuState.canVerify === true
            height: visible ? implicitHeight : 0
            onTriggered: root.openChecksum(root.menuState.single)
        }
        OrbitMenuItem {
            text: "Force Recheck"
            iconName: "retry"
            visible: root.menuState.torrentLive === true
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (downloadModel.forceRecheck(root.menuState.single.taskId)) root.feedback("Rechecking “" + root.menuState.single.name + "”", true)
            }
        }
        OrbitMenuItem {
            text: "Ask Trackers for Peers"
            iconName: "network"
            visible: root.menuState.torrentLive === true
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (downloadModel.forceReannounce(root.menuState.single.taskId)) root.feedback("Asked trackers and DHT for more peers", true)
            }
        }
        MenuDivider {}
        OrbitMenuItem {
            text: root.menuState.count > 1 ? "Copy Source Links" : "Copy Source Link"
            iconName: "link"
            onTriggered: root.copyText(root.items().map(it => it.source).join("\n"))
        }
        OrbitMenuItem {
            text: "Copy File Path"
            iconName: "copy"
            visible: root.menuState.single !== null && root.menuState.single !== undefined
            height: visible ? implicitHeight : 0
            enabled: visible && root.menuState.single.destination.length > 0
            onTriggered: root.copyText(root.menuState.single.destination)
        }
        MenuDivider {}
        OrbitMenuItem {
            text: root.countLabel("Cancel", root.menuState.canCancel)
            iconName: "close"
            danger: true
            enabled: root.menuState.canCancel > 0
            onTriggered: root.cancelSelected()
        }
        OrbitMenuItem {
            text: root.menuState.count > 1 && root.menuState.canRemove > 0 ? "Remove " + root.menuState.canRemove + " From List" : "Remove From List"
            iconName: "trash"
            danger: true
            enabled: root.menuState.canRemove > 0
            onTriggered: root.removeSelected()
        }
        OrbitMenuItem {
            text: root.menuState.canRemove > 1 ? "Delete " + root.menuState.canRemove + " Files…" : "Delete File…"
            iconName: "trash"
            danger: true
            enabled: root.menuState.canRemove > 0
            onTriggered: root.confirmDeleteFiles()
        }
    }

    OrbitMenu {
        id: sortMenu

        Instantiator {
            model: root.sortOptions
            delegate: OrbitMenuItem {
                required property var modelData
                text: modelData.label
                iconName: "check"
                iconHidden: root.sortKey !== modelData.key
                onTriggered: root.sortRequested(modelData.key, root.sortKey === modelData.key ? root.sortDescending
                                                                                               : (modelData.key === "speed" || modelData.key === "size"))
            }
            onObjectAdded: (index, object) => sortMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => sortMenu.removeItem(object)
        }
        MenuDivider {}
        OrbitMenuItem {
            text: "Ascending"
            iconName: "check"
            iconHidden: root.sortDescending
            onTriggered: root.sortRequested(root.sortKey, false)
        }
        OrbitMenuItem {
            text: "Descending"
            iconName: "check"
            iconHidden: !root.sortDescending
            onTriggered: root.sortRequested(root.sortKey, true)
        }
    }

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.columnHeaderHeight
        color: Theme.headerStrip

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

        component ColumnLabel: Item {
            id: colLabel
            property string text
            property string key
            readonly property bool active: root.sortKey === key
            Layout.fillHeight: true

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Text {
                    text: colLabel.text
                    color: colLabel.active ? Theme.textPrimary : (labelMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
                    font.pixelSize: Theme.fontMicro
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.trackingWider
                    anchors.verticalCenter: parent.verticalCenter
                }
                Icon {
                    visible: colLabel.active
                    name: "chevronDown"
                    iconSize: 10
                    color: Theme.textPrimary
                    rotation: root.sortDescending ? 0 : 180
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            MouseArea {
                id: labelMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (colLabel.active) root.sortRequested(colLabel.key, !root.sortDescending)
                    else root.sortRequested(colLabel.key, colLabel.key === "speed" || colLabel.key === "size")
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16 + 34 + 12
            anchors.rightMargin: root.rightPad
            spacing: 0

            ColumnLabel { Layout.fillWidth: true; text: "NAME"; key: "name" }
            ColumnLabel { visible: root.wProgress > 0; Layout.preferredWidth: root.wProgress; text: "PROGRESS"; key: "progress" }
            ColumnLabel { visible: root.wSpeed > 0; Layout.preferredWidth: root.wSpeed; text: "SPEED"; key: "speed" }
            ColumnLabel { visible: root.wTime > 0; Layout.preferredWidth: root.wTime; text: "TIME"; key: "time" }
            ColumnLabel { Layout.preferredWidth: root.wStatus; text: "STATUS"; key: "status" }
            Item {
                Layout.preferredWidth: root.wActions
                Layout.fillHeight: true

                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: 22
                    width: sortRow.implicitWidth + 14
                    radius: Theme.radiusSmall
                    color: sortMouse.containsMouse || sortMenu.visible ? Theme.surfaceHover : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.animFast } }

                    Row {
                        id: sortRow
                        anchors.centerIn: parent
                        spacing: 5
                        Icon { name: "sort"; iconSize: 11; color: Theme.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            text: "Sort"
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMicro
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: sortMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sortMenu.popup(parent, parent.width - sortMenu.width, parent.height + 4)
                    }
                    OrbitToolTip {
                        visible: sortMouse.containsMouse && !sortMenu.visible
                        text: "Sorted by " + root.sortOptions.find(o => o.key === root.sortKey).label.toLowerCase()
                    }
                }
            }
        }
    }

    ListView {
        id: listView
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        bottomMargin: Theme.spaceSm
        activeFocusOnTab: true

        delegate: DownloadRow { table: root }

        onCountChanged: root.prune()
        onHeightChanged: if (currentIndex >= 0 && root.hasSelection) positionViewAtIndex(currentIndex, ListView.Contain)

        Keys.onPressed: (event) => {
            const shift = (event.modifiers & Qt.ShiftModifier) !== 0
            const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
            switch (event.key) {
            case Qt.Key_Down: root.moveFocus(1, shift); break
            case Qt.Key_Up: root.moveFocus(-1, shift); break
            case Qt.Key_PageDown: root.moveFocus(Math.max(1, Math.floor(listView.height / Theme.rowHeight) - 1), shift); break
            case Qt.Key_PageUp: root.moveFocus(-Math.max(1, Math.floor(listView.height / Theme.rowHeight) - 1), shift); break
            case Qt.Key_Home: root.moveFocus(-listView.count, shift); break
            case Qt.Key_End: root.moveFocus(listView.count, shift); break
            case Qt.Key_Escape: root.clearSelection(); break
            case Qt.Key_A:
                if (!ctrl) return
                root.selectAll()
                break
            case Qt.Key_Space:
                if (!root.hasSelection) return
                root.toggleSelected()
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (root.selectionCount !== 1) return
                root.openPrimary(root.focusId)
                break
            case Qt.Key_Delete:
                if (!root.hasSelection) return
                if (shift) root.confirmDeleteFiles()
                else root.removeSelected()
                break
            case Qt.Key_Menu:
                if (!root.hasSelection) return
                root.openMenu()
                break
            default:
                return
            }
            event.accepted = true
        }

        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animSlow; easing.type: Theme.easeStandard }
        }
        remove: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: Theme.animNormal }
        }
        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        }
        move: Transition {
            NumberAnimation { properties: "x,y"; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        }

        ScrollBar.vertical: ScrollBar {
            id: vbar
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                implicitWidth: 5
                radius: 2.5
                color: vbar.pressed ? Theme.textTertiary : Theme.borderStrong
                opacity: vbar.active || vbar.hovered ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
            }
            background: Item {}
        }
    }
}
