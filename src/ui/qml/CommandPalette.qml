import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Ctrl+K: one search box for everything. Typing finds commands, settings pages and
// downloads; a pasted link offers to add it. Up/Down choose, Enter runs, Esc closes.
Popup {
    id: root

    // Supplied by main.qml: [{ title, detail, icon, keywords, run }] built each time it opens.
    property var commandsProvider: function() { return [] }
    // Runs for a download picked from the results.
    signal downloadPicked(var taskId)
    signal addRequested(string link, bool review)

    property var results: []
    property int current: 0

    modal: true
    focus: true
    padding: 0
    width: Math.min(560, parent ? parent.width - 48 : 560)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round(parent.height * 0.14) : 80
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.45) }
    background: DialogSurface {}

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animFast; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animInstant }
    }

    onOpened: {
        field.text = ""
        refresh()
        field.forceActiveFocus()
    }

    function score(entry, words) {
        const haystack = (entry.title + " " + (entry.keywords || "")).toLowerCase()
        let total = 0
        for (const word of words) {
            const at = haystack.indexOf(word)
            if (at < 0) return -1
            total += at === 0 ? 3 : (haystack.charAt(at - 1) === " " ? 2 : 1)
        }
        return total
    }

    function refresh() {
        const text = field.text.trim()
        const words = text.toLowerCase().split(/\s+/).filter(w => w.length > 0)
        const list = []
        if (text.length > 0 && appController.isMagnetOrUrl(text)) {
            list.push({ title: "Add this link", detail: text, icon: "plus", run: () => root.addRequested(text, false) })
            list.push({ title: "Review before adding…", detail: text, icon: "edit", run: () => root.addRequested(text, true) })
        }
        const commands = root.commandsProvider()
        const scored = words.length === 0 ? commands.map(c => ({ c: c, s: 0 }))
                                          : commands.map(c => ({ c: c, s: score(c, words) })).filter(x => x.s >= 0)
        scored.sort((a, b) => b.s - a.s)
        for (const item of scored.slice(0, 8)) list.push(item.c)
        if (words.length > 0) {
            for (const d of downloadModel.search(text, 6)) {
                list.push({ title: d.name, detail: d.stateLabel, icon: d.isTorrent ? "torrent" : (d.isMedia ? "media" : "downloading"),
                            run: () => root.downloadPicked(d.taskId) })
            }
        }
        results = list
        current = 0
    }

    function runCurrent() {
        if (current < 0 || current >= results.length) return
        const entry = results[current]
        close()
        Qt.callLater(entry.run)
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 10

            Icon { name: "search"; iconSize: 15; color: Theme.textTertiary }
            TextField {
                id: field
                Layout.fillWidth: true
                placeholderText: "Search downloads, settings and actions, or paste a link"
                placeholderTextColor: Theme.textTertiary
                color: Theme.textPrimary
                font.pixelSize: 14
                background: null
                selectByMouse: true
                onTextChanged: root.refresh()
                Keys.onDownPressed: root.current = Math.min(root.results.length - 1, root.current + 1)
                Keys.onUpPressed: root.current = Math.max(0, root.current - 1)
                Keys.onReturnPressed: root.runCurrent()
                Keys.onEnterPressed: root.runCurrent()
                Accessible.name: "Command palette"
            }
            KeyCap { text: "Esc" }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 360)
            Layout.topMargin: 6
            Layout.bottomMargin: 6
            clip: true
            model: root.results
            currentIndex: root.current
            boundsBehavior: Flickable.StopAtBounds
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: list.width
                height: 40
                color: index === root.current ? Theme.withAlpha(Theme.accent, 0.12) : "transparent"
                radius: Theme.radiusMedium

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 10
                    Icon {
                        name: row.modelData.icon || "sparkle"
                        iconSize: 13
                        color: row.index === root.current ? Theme.accent : Theme.textSecondary
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.title
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: row.index === root.current ? Font.DemiBold : Font.Normal
                        elide: Text.ElideMiddle
                    }
                    Text {
                        visible: (row.modelData.detail || "").length > 0
                        Layout.maximumWidth: 220
                        text: row.modelData.detail || ""
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    // Only a moving pointer picks a row, not one that happens to rest
                    // where the list opens.
                    onPositionChanged: root.current = row.index
                    onClicked: root.runCurrent()
                }
            }
        }

        Text {
            visible: root.results.length === 0
            Layout.alignment: Qt.AlignHCenter
            Layout.margins: 18
            text: "Nothing matches"
            color: Theme.textTertiary
            font.pixelSize: Theme.fontCaption
        }
    }
}
