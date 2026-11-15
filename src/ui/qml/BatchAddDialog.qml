import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Adds many links at once. Each link is checked first (name, size, kind, and whether it
// was downloaded before), and the ones to start are ticked; nothing starts until Add.
Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: Math.min(680, parent ? parent.width - 48 : 680)
    height: Math.min(560, parent ? parent.height - 48 : 560)
    anchors.centerIn: parent
    padding: 0

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    ListModel { id: entries }
    property var pending: ({})
    property string directory: appController.downloadDirectory
    readonly property int checkedCount: { let n = 0; for (let i = 0; i < entries.count; ++i) if (entries.get(i).checked) ++n; return countTick >= 0 ? n : 0 }
    readonly property real checkedSize: { let n = 0; for (let i = 0; i < entries.count; ++i) if (entries.get(i).checked) n += entries.get(i).size; return countTick >= 0 ? n : 0 }
    property int countTick: 0

    // Opens with the links found in text (one per line, or separated by spaces).
    function openWithText(text) {
        linksArea.text = text
        directory = appController.downloadDirectory
        open()
        Qt.callLater(inspect)
    }

    onOpened: linksArea.forceActiveFocus()

    function parseLinks(text) {
        const seen = {}
        const out = []
        for (const part of text.split(/\s+/)) {
            const link = part.trim()
            if (link.length === 0 || seen[link] || !appController.isMagnetOrUrl(link)) continue
            seen[link] = true
            out.push(link)
        }
        return out.slice(0, 500)
    }

    function inspect() {
        const links = parseLinks(linksArea.text)
        entries.clear()
        pending = ({})
        for (const link of links) {
            const before = appController.previousDownloadOf(link)
            entries.append({ link: link, name: link, size: 0, sizeText: "", kind: "", status: "checking",
                             error: "", checked: !before.found, before: before.found ? "Downloaded " + before.when : "" })
            const token = appController.inspectLink(link)
            pending[token] = link
        }
        countTick++
    }

    function rowOf(link) {
        for (let i = 0; i < entries.count; ++i) if (entries.get(i).link === link) return i
        return -1
    }

    function submit() {
        let added = 0
        for (let i = 0; i < entries.count; ++i) {
            const e = entries.get(i)
            if (!e.checked) continue
            const kind = e.kind === "torrent" ? "torrent" : (e.kind === "media" ? "media" : "http")
            downloadModel.addDownload({ kind: kind, source: e.link, directory: root.directory })
            ++added
        }
        close()
        return added
    }

    Connections {
        target: appController
        function onLinkInspected(token, result) {
            const link = root.pending[token]
            if (link === undefined) return
            delete root.pending[token]
            const row = root.rowOf(link)
            if (row < 0) return
            entries.setProperty(row, "kind", result.kind || "")
            entries.setProperty(row, "status", result.ok ? "ok" : "error")
            entries.setProperty(row, "error", result.ok ? "" : (result.error || "Couldn't reach it"))
            if (result.fileName && result.fileName.length > 0) entries.setProperty(row, "name", result.fileName)
            entries.setProperty(row, "size", result.size || 0)
            entries.setProperty(row, "sizeText", result.sizeText || "")
            if (!result.ok) entries.setProperty(row, "checked", false)
            root.countTick++
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.leftMargin: 20
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { Layout.fillWidth: true; text: "Add Many Links"; color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
                Text { text: "Paste links, one per line. Each is checked before anything starts."; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            Layout.margins: 16
            Layout.bottomMargin: 8
            TextArea {
                id: linksArea
                placeholderText: "https://example.org/file.iso\nmagnet:?xt=urn:btih:…"
                color: Theme.textPrimary
                placeholderTextColor: Theme.textTertiary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontCaption
                wrapMode: TextArea.NoWrap
                selectByMouse: true
                background: Rectangle { radius: Theme.radiusMedium; color: Theme.surfaceInset; border.width: 1; border.color: linksArea.activeFocus ? Theme.withAlpha(Theme.accent, 0.8) : Theme.border }
                onTextChanged: inspectTimer.restart()
            }
        }
        Timer { id: inspectTimer; interval: 600; onTriggered: root.inspect() }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            clip: true
            model: entries
            spacing: 2
            delegate: Rectangle {
                id: entry
                required property int index
                required property string link
                required property string name
                required property string sizeText
                required property string kind
                required property string status
                required property string error
                required property bool checked
                required property string before
                width: list.width
                height: 44
                radius: Theme.radiusMedium
                color: entryHover.hovered ? Theme.rowHover : "transparent"
                HoverHandler { id: entryHover }
                Accessible.role: Accessible.CheckBox
                Accessible.name: entry.name
                Accessible.checked: entry.checked
                TapHandler {
                    enabled: entry.status !== "error"
                    onTapped: { entries.setProperty(entry.index, "checked", !entry.checked); root.countTick++ }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        radius: 4
                        opacity: entry.status === "error" ? 0.4 : 1
                        color: entry.checked ? Theme.accent : "transparent"
                        border.width: 1.4
                        border.color: entry.checked ? Theme.accent : Theme.withAlpha(Theme.textPrimary, 0.22)
                        Icon { anchors.centerIn: parent; visible: entry.checked; name: "check"; iconSize: 10; color: "#ffffff" }
                    }
                    FileTile {
                        size: 26
                        fileName: entry.name
                        isTorrent: entry.kind === "torrent"
                        isMedia: entry.kind === "media"
                        muted: entry.status === "error"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text { Layout.fillWidth: true; text: entry.name; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; elide: Text.ElideMiddle }
                        Text {
                            Layout.fillWidth: true
                            text: entry.status === "error" ? entry.error : (entry.before.length > 0 ? entry.before + " · " + entry.link : entry.link)
                            color: entry.status === "error" ? Theme.danger : (entry.before.length > 0 ? Theme.warning : Theme.textTertiary)
                            font.pixelSize: Theme.fontMicro
                            elide: Text.ElideMiddle
                        }
                    }
                    Text {
                        text: entry.status === "checking" ? "Checking…" : (entry.sizeText.length > 0 ? entry.sizeText : (entry.kind === "torrent" ? "Torrent" : (entry.kind === "media" ? "Video" : "")))
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            Layout.leftMargin: 20
            spacing: 10
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: root.checkedCount + (root.checkedCount === 1 ? " download" : " downloads") +
                          (root.checkedSize > 0 ? " · " + Theme.formatBytes(root.checkedSize) + " known" : "")
                    color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.Medium
                }
                Text { Layout.fillWidth: true; text: "Into " + root.directory; color: Theme.textTertiary; font.pixelSize: Theme.fontMicro; elide: Text.ElideMiddle }
            }
            ModernButton {
                text: "Change Folder…"
                variant: "secondary"
                onClicked: {
                    const dir = appController.pickFolder("Save the downloads in", root.directory)
                    if (dir.length > 0) root.directory = dir
                }
            }
            ModernButton {
                text: root.checkedCount > 0 ? "Add " + root.checkedCount : "Add"
                variant: "primary"
                enabled: root.checkedCount > 0
                onClicked: root.submit()
            }
        }
    }
}
