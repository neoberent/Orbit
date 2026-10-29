import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property var taskId: -1
    property string page: "files"

    property var files: []
    property var peers: []
    property var trackers: []

    function refresh() {
        if (taskId < 0 || !visible) return
        if (page === "files") files = downloadModel.torrentFiles(taskId)
        else if (page === "peers") peers = downloadModel.torrentPeers(taskId)
        else if (page === "trackers") trackers = downloadModel.torrentTrackers(taskId)
    }

    function priorityLabel(p) {
        if (p === 0) return "Skip"
        if (p <= 2) return "Low"
        if (p >= 6) return "High"
        return "Normal"
    }

    onTaskIdChanged: refresh()
    onPageChanged: refresh()
    onVisibleChanged: refresh()

    Timer {
        interval: 1500
        repeat: true
        running: root.visible && root.taskId >= 0
        onTriggered: root.refresh()
    }

    OrbitMenu {
        id: priorityMenu
        property int fileIndex: -1
        Instantiator {
            model: [{ label: "High", value: 7 }, { label: "Normal", value: 4 }, { label: "Low", value: 1 }, { label: "Skip", value: 0 }]
            delegate: OrbitMenuItem {
                required property var modelData
                text: modelData.label
                onTriggered: {
                    downloadModel.setTorrentFilePriority(root.taskId, priorityMenu.fileIndex, modelData.value)
                    root.refresh()
                }
            }
            onObjectAdded: (index, object) => priorityMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => priorityMenu.removeItem(object)
        }
    }

    component HeaderLabel: Text {
        color: Theme.textTertiary
        font.pixelSize: Theme.fontMicro
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.trackingWider
        elide: Text.ElideRight
    }

    component Cell: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        font.features: { "tnum": 1 }
        elide: Text.ElideRight
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: Theme.surfaceInset
        border.width: 1
        border.color: Theme.border
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                Layout.leftMargin: 10
                Layout.rightMargin: 12
                spacing: 10

                HeaderLabel { visible: root.page === "files"; Layout.fillWidth: true; text: "FILE" }
                HeaderLabel { visible: root.page === "files"; Layout.preferredWidth: 70; text: "SIZE" }
                HeaderLabel { visible: root.page === "files"; Layout.preferredWidth: 110; text: "PROGRESS" }
                HeaderLabel { visible: root.page === "files"; Layout.preferredWidth: 70; text: "PRIORITY" }

                HeaderLabel { visible: root.page === "peers"; Layout.preferredWidth: 170; text: "ADDRESS" }
                HeaderLabel { visible: root.page === "peers"; Layout.fillWidth: true; text: "CLIENT" }
                HeaderLabel { visible: root.page === "peers"; Layout.preferredWidth: 90; text: "CONNECTION" }
                HeaderLabel { visible: root.page === "peers"; Layout.preferredWidth: 56; text: "HAS" }
                HeaderLabel { visible: root.page === "peers"; Layout.preferredWidth: 80; text: "DOWN" }
                HeaderLabel { visible: root.page === "peers"; Layout.preferredWidth: 80; text: "UP" }

                HeaderLabel { visible: root.page === "trackers"; Layout.fillWidth: true; text: "TRACKER" }
                HeaderLabel { visible: root.page === "trackers"; Layout.preferredWidth: 90; text: "STATUS" }
                HeaderLabel { visible: root.page === "trackers"; Layout.preferredWidth: 60; text: "SEEDS" }
                HeaderLabel { visible: root.page === "trackers"; Layout.preferredWidth: 60; text: "PEERS" }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.page === "files" ? root.files : root.page === "peers" ? root.peers : root.trackers
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: rowItem
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 26

                    Rectangle {
                        anchors.fill: parent
                        color: rowItem.index % 2 === 1 ? Theme.withAlpha(Theme.textPrimary, 0.02) : "transparent"
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 12
                        spacing: 10
                        visible: root.page === "files"

                        Cell {
                            Layout.fillWidth: true
                            text: rowItem.modelData.path !== undefined ? rowItem.modelData.path : ""
                            color: rowItem.modelData.priority === 0 ? Theme.textDisabled : Theme.textPrimary
                            elide: Text.ElideMiddle
                        }
                        Cell { Layout.preferredWidth: 70; text: rowItem.modelData.size !== undefined ? Theme.formatBytes(rowItem.modelData.size) : "" }
                        RowLayout {
                            Layout.preferredWidth: 110
                            spacing: 6
                            ProgressIndicator {
                                Layout.fillWidth: true
                                height: 4
                                value: rowItem.modelData.progress !== undefined ? rowItem.modelData.progress : 0
                                fillColor: Theme.accent
                            }
                            Cell {
                                Layout.preferredWidth: 34
                                horizontalAlignment: Text.AlignRight
                                text: rowItem.modelData.progress !== undefined ? Theme.percentText(rowItem.modelData.progress) : ""
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 70
                            Layout.preferredHeight: 20
                            radius: Theme.radiusSmall
                            color: prioMouse.containsMouse ? Theme.surfaceHover : "transparent"
                            border.width: 1
                            border.color: Theme.border
                            Accessible.role: Accessible.Button
                            Accessible.name: "Priority " + root.priorityLabel(rowItem.modelData.priority)
                            Accessible.onPressAction: { priorityMenu.fileIndex = rowItem.modelData.index; priorityMenu.popup() }
                            Text {
                                anchors.centerIn: parent
                                text: root.priorityLabel(rowItem.modelData.priority)
                                color: rowItem.modelData.priority === 0 ? Theme.textTertiary
                                       : rowItem.modelData.priority >= 6 ? Theme.accent : Theme.textSecondary
                                font.pixelSize: Theme.fontMicro
                                font.weight: Font.DemiBold
                            }
                            MouseArea {
                                id: prioMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    priorityMenu.fileIndex = rowItem.modelData.index
                                    priorityMenu.popup()
                                }
                            }
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 12
                        spacing: 10
                        visible: root.page === "peers"

                        Cell { Layout.preferredWidth: 170; text: rowItem.modelData.address || ""; font.family: Theme.monoFamily; color: Theme.textPrimary }
                        Cell { Layout.fillWidth: true; text: rowItem.modelData.client || "—" }
                        Cell { Layout.preferredWidth: 90; text: rowItem.modelData.flags || "" }
                        Cell {
                            Layout.preferredWidth: 56
                            text: rowItem.modelData.seed ? "Seed" : (rowItem.modelData.progress !== undefined ? Theme.percentText(rowItem.modelData.progress) : "")
                        }
                        Cell { Layout.preferredWidth: 80; text: rowItem.modelData.downloadRate > 0 ? Theme.formatRate(rowItem.modelData.downloadRate) : "—" }
                        Cell { Layout.preferredWidth: 80; text: rowItem.modelData.uploadRate > 0 ? Theme.formatRate(rowItem.modelData.uploadRate) : "—" }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 12
                        spacing: 10
                        visible: root.page === "trackers"

                        Cell {
                            Layout.fillWidth: true
                            text: rowItem.modelData.url || ""
                            color: Theme.textPrimary
                            elide: Text.ElideMiddle
                        }
                        Cell {
                            Layout.preferredWidth: 90
                            text: rowItem.modelData.status || ""
                            color: rowItem.modelData.status === "Working" ? Theme.success
                                   : (rowItem.modelData.status === "Error" || rowItem.modelData.status === "Not working") ? Theme.danger
                                   : Theme.textSecondary
                            OrbitToolTip {
                                visible: trackerHover.hovered && (rowItem.modelData.message || "").length > 0
                                text: rowItem.modelData.message || ""
                            }
                            HoverHandler { id: trackerHover }
                        }
                        Cell { Layout.preferredWidth: 60; text: rowItem.modelData.seeds >= 0 ? rowItem.modelData.seeds : "—" }
                        Cell { Layout.preferredWidth: 60; text: rowItem.modelData.peers >= 0 ? rowItem.modelData.peers : "—" }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: root.page === "files" ? "The file list appears once the torrent's metadata arrives"
                          : root.page === "peers" ? "Not connected to any peers right now"
                          : "No trackers - peers are found through DHT"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
    }
}
