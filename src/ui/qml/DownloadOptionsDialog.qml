import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 500
    anchors.centerIn: parent
    padding: 0

    property var item: ({})
    property string errorText: ""
    property int connections: 8

    readonly property bool isTorrent: item.isTorrent === true
    readonly property bool isMedia: item.isMedia === true
    readonly property bool isHttp: !isTorrent && !isMedia
    readonly property bool unfinished: item.stateLabel !== "Completed" && item.stateLabel !== "Cancelled"

    function openFor(it) {
        item = it
        errorText = ""
        limitToggle.checked = it.speedLimitKBps > 0
        limitField.text = it.speedLimitKBps > 0 ? it.speedLimitKBps : 1024
        uploadToggle.checked = it.uploadLimitKBps > 0
        uploadField.text = it.uploadLimitKBps > 0 ? it.uploadLimitKBps : 256
        connections = it.maxConnections > 0 ? it.maxConnections : 8
        sequentialToggle.checked = it.sequential === true
        checksumField.text = it.expectedChecksum || ""
        const scheduled = it.scheduledStart > Date.now() / 1000
        startLaterToggle.checked = scheduled
        startField.text = scheduled ? Qt.formatTime(new Date(it.scheduledStart * 1000), "HH:mm") : "23:00"
        open()
    }

    function nextOccurrence(hhmm) {
        const parts = hhmm.split(":")
        const when = new Date()
        when.setHours(parseInt(parts[0]), parseInt(parts[1]), 0, 0)
        if (when.getTime() <= Date.now()) when.setDate(when.getDate() + 1)
        return Math.floor(when.getTime() / 1000)
    }

    function apply() {
        const options = { speedLimitKBps: limitToggle.checked ? Math.max(1, parseInt(limitField.text || "1024")) : 0 }
        if (isTorrent) {
            options.uploadLimitKBps = uploadToggle.checked ? Math.max(1, parseInt(uploadField.text || "256")) : 0
            options.sequential = sequentialToggle.checked
        }
        if (isHttp) options.connections = connections
        if (!isTorrent && checksumField.text.trim() !== (item.expectedChecksum || "")) {
            const error = downloadModel.setExpectedChecksum(item.taskId, checksumField.text.trim())
            if (error.length > 0) { errorText = error; return }
        }
        downloadModel.setDownloadOptions(item.taskId, options)
        if (unfinished && !item.isActive) {
            if (startLaterToggle.checked && startField.field.acceptableInput) {
                downloadModel.setScheduledStart(item.taskId, nextOccurrence(startField.text))
            } else if (!startLaterToggle.checked && item.scheduledStart > 0) {
                downloadModel.setScheduledStart(item.taskId, 0)
            }
        }
        close()
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 14
            spacing: 12
            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.accent, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.accent, 0.22)
                Icon { anchors.centerIn: parent; name: "sliders"; iconSize: 15; color: Theme.accent }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "Download Options"; color: Theme.textPrimary; font.pixelSize: Theme.fontHeading + 1; font.weight: Font.DemiBold }
                Text {
                    Layout.fillWidth: true
                    text: root.item.name || ""
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            spacing: 16

            SettingsGroup {
                title: "Speed"

                SettingsRow {
                    divider: false
                    label: "Limit download speed"
                    description: root.isMedia ? "Takes effect the next time this download starts" : "Applies right away and is kept after a restart"
                    FormField {
                        id: limitField
                        visible: limitToggle.checked
                        Layout.preferredWidth: 120
                        suffix: "KB/s"
                        validator: IntValidator { bottom: 1; top: 10000000 }
                    }
                    ToggleSwitch { id: limitToggle }
                }
                SettingsRow {
                    visible: root.isTorrent
                    label: "Limit upload speed"
                    FormField {
                        id: uploadField
                        visible: uploadToggle.checked
                        Layout.preferredWidth: 120
                        suffix: "KB/s"
                        validator: IntValidator { bottom: 1; top: 10000000 }
                    }
                    ToggleSwitch { id: uploadToggle }
                }
                SettingsRow {
                    visible: root.isHttp
                    label: "Connections"
                    description: "Parallel segments for this download"

                    Rectangle {
                        Layout.preferredWidth: 112
                        Layout.preferredHeight: 32
                        radius: Theme.radiusMedium
                        color: Theme.surfaceInset
                        border.width: 1
                        border.color: Theme.border

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 3
                            spacing: 0
                            IconButton {
                                implicitWidth: 26
                                implicitHeight: 26
                                iconName: "minus"
                                tooltip: "Fewer connections"
                                enabled: root.connections > 1
                                onClicked: root.connections = Math.max(1, root.connections - 1)
                            }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: root.connections
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                font.features: { "tnum": 1 }
                            }
                            IconButton {
                                implicitWidth: 26
                                implicitHeight: 26
                                iconName: "plus"
                                tooltip: "More connections"
                                enabled: root.connections < 32
                                onClicked: root.connections = Math.min(32, root.connections + 1)
                            }
                        }
                    }
                }
                SettingsRow {
                    visible: root.isTorrent
                    label: "Download pieces in order"
                    description: "Lets you preview media while it downloads; slightly slower overall"
                    ToggleSwitch { id: sequentialToggle }
                }
            }

            SettingsGroup {
                title: "Start"
                visible: root.unfinished && root.item.isActive !== true

                SettingsRow {
                    divider: false
                    label: "Start at a set time"
                    description: "The download stays paused until then"
                    FormField {
                        id: startField
                        visible: startLaterToggle.checked
                        Layout.preferredWidth: 90
                        iconName: "clock"
                        monospace: true
                        validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                    }
                    ToggleSwitch { id: startLaterToggle }
                }
            }

            SettingsGroup {
                title: "Checksum"
                visible: !root.isTorrent
                description: "Paste the MD5, SHA-1, SHA-256 or SHA-512 the site publishes. Orbit checks the file when it finishes."

                SettingsRow {
                    divider: false
                    label: "Expected checksum"
                    extra: FormField {
                        id: checksumField
                        Layout.fillWidth: true
                        monospace: true
                        iconName: "shield"
                        placeholderText: "sha256:… or just the hex digest"
                        field.onTextChanged: root.errorText = ""
                    }
                }
            }

            Text {
                visible: root.errorText.length > 0
                Layout.fillWidth: true
                text: root.errorText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton { text: "Cancel"; variant: "ghost"; onClicked: root.close() }
            ModernButton { text: "Apply"; variant: "primary"; onClicked: root.apply() }
        }
    }
}
