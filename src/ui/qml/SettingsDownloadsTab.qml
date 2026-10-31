import QtQuick
import QtQuick.Layouts

// Downloads settings: where files go, connections and bandwidth.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        speedLimitField.text = appController.httpSpeedLimitKBps > 0 ? appController.httpSpeedLimitKBps : 1024
    }

    SettingsGroup {
        title: "Downloads"

        SettingsRow {
            divider: false
            label: "Download folder"
            description: appController.downloadDirectory
            ModernButton {
                text: "Change…"
                variant: "secondary"
                iconName: "folder"
                onClicked: appController.pickDownloadDirectory()
            }
        }

        SettingsRow {
            label: "Connections per download"
            description: "Parallel segments used for direct downloads"
            ValueChip { text: connSlider.value.toFixed(0) }
            extra: SliderControl {
                id: connSlider
                Layout.fillWidth: true
                from: 1
                to: 32
                stepSize: 1
                value: appController.maxConnectionsPerDownload
                onMoved: appController.maxConnectionsPerDownload = value
            }
        }

        SettingsRow {
            label: "If the file already exists"
            description: appController.fileConflictAction === "overwrite" ? "The existing file is replaced"
                         : appController.fileConflictAction === "rename" ? "The new file gets a numbered name, like “setup (1).exe”"
                         : "Orbit asks whether to replace the file or keep both"
            SegmentedControl {
                options: [{ label: "Ask", value: "ask" }, { label: "Keep both", value: "rename" }, { label: "Replace", value: "overwrite" }]
                value: appController.fileConflictAction
                onActivated: (v) => appController.fileConflictAction = v
            }
        }

        SettingsRow {
            label: "Simultaneous downloads"
            description: "Additional downloads wait in the queue"
            ValueChip { text: concurrentSlider.value.toFixed(0) }
            extra: SliderControl {
                id: concurrentSlider
                Layout.fillWidth: true
                from: 1
                to: 10
                stepSize: 1
                value: appController.maxConcurrentDownloads
                onMoved: appController.maxConcurrentDownloads = value
            }
        }
    }

    SettingsGroup {
        title: "Bandwidth"

        SettingsRow {
            divider: false
            label: "Limit download speed"
            description: "Caps the combined speed of direct downloads. Torrents and media are not affected."
            ToggleSwitch {
                checked: appController.httpSpeedLimitKBps > 0
                onToggled: appController.httpSpeedLimitKBps = checked
                                           ? (speedLimitField.text.length > 0 ? parseInt(speedLimitField.text) : 1024)
                                           : 0
            }
        }

        SettingsRow {
            label: "Maximum speed"
            description: "Applies to all direct downloads combined"
            dimmed: appController.httpSpeedLimitKBps <= 0
            enabled: appController.httpSpeedLimitKBps > 0
            FormField {
                id: speedLimitField
                Layout.preferredWidth: 130
                suffix: "KB/s"
                validator: IntValidator { bottom: 1; top: 1000000 }
                onEditingFinished: appController.httpSpeedLimitKBps = parseInt(text.length > 0 ? text : "1024")
            }
        }
    }
}
