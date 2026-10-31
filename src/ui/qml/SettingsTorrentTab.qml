import QtQuick
import QtQuick.Layouts

// BitTorrent settings: speed limits and when to stop seeding.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        torrentDownField.text = appController.torrentDownloadLimitKBps > 0 ? appController.torrentDownloadLimitKBps : 2048
        torrentUpField.text = appController.torrentUploadLimitKBps > 0 ? appController.torrentUploadLimitKBps : 512
        seedRatioField.text = appController.seedRatioLimit > 0 ? appController.seedRatioLimit : 2
        seedTimeField.text = appController.seedTimeLimitMinutes > 0 ? appController.seedTimeLimitMinutes : 120
    }

    SettingsGroup {
        title: "Speed"

        SettingsRow {
            divider: false
            label: "Limit torrent downloads"
            description: "All torrents combined"
            FormField {
                id: torrentDownField
                visible: appController.torrentDownloadLimitKBps > 0
                Layout.preferredWidth: 120
                suffix: "KB/s"
                validator: IntValidator { bottom: 1; top: 10000000 }
                onEditingFinished: appController.torrentDownloadLimitKBps = parseInt(text.length > 0 ? text : "2048")
            }
            ToggleSwitch {
                checked: appController.torrentDownloadLimitKBps > 0
                onToggled: appController.torrentDownloadLimitKBps = checked ? parseInt(torrentDownField.text || "2048") : 0
            }
        }
        SettingsRow {
            label: "Limit uploads"
            description: "Seeding and uploading while downloading"
            FormField {
                id: torrentUpField
                visible: appController.torrentUploadLimitKBps > 0
                Layout.preferredWidth: 120
                suffix: "KB/s"
                validator: IntValidator { bottom: 1; top: 10000000 }
                onEditingFinished: appController.torrentUploadLimitKBps = parseInt(text.length > 0 ? text : "512")
            }
            ToggleSwitch {
                checked: appController.torrentUploadLimitKBps > 0
                onToggled: appController.torrentUploadLimitKBps = checked ? parseInt(torrentUpField.text || "512") : 0
            }
        }
    }

    SettingsGroup {
        title: "Seeding"
        description: "A finished torrent keeps sharing with others until one of these limits is reached, then shows as Completed."

        SettingsRow {
            divider: false
            label: "Stop at share ratio"
            description: "Uploaded ÷ downloaded, e.g. 2 means twice the torrent's size"
            FormField {
                id: seedRatioField
                visible: appController.seedRatioLimit > 0
                Layout.preferredWidth: 90
                validator: DoubleValidator { bottom: 0.1; top: 100; decimals: 2 }
                onEditingFinished: appController.seedRatioLimit = parseFloat(text.length > 0 ? text : "2")
            }
            ToggleSwitch {
                checked: appController.seedRatioLimit > 0
                onToggled: appController.seedRatioLimit = checked ? parseFloat(seedRatioField.text || "2") : 0
            }
        }
        SettingsRow {
            label: "Stop after seeding for"
            FormField {
                id: seedTimeField
                visible: appController.seedTimeLimitMinutes > 0
                Layout.preferredWidth: 110
                suffix: "min"
                validator: IntValidator { bottom: 1; top: 1000000 }
                onEditingFinished: appController.seedTimeLimitMinutes = parseInt(text.length > 0 ? text : "120")
            }
            ToggleSwitch {
                checked: appController.seedTimeLimitMinutes > 0
                onToggled: appController.seedTimeLimitMinutes = checked ? parseInt(seedTimeField.text || "120") : 0
            }
        }
    }

    Text {
        Layout.fillWidth: true
        text: "Per-torrent limits, sequential download and file priorities are under Download Options and the Files tab in the details pane."
        color: Theme.textTertiary
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }
}
