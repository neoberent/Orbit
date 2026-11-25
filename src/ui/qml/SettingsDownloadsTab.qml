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
                         : appController.fileConflictAction === "rename" ? "The new file gets a numbered name, like “" + (Theme.windows ? "setup (1).exe" : "archive (1).tar.gz") + "”"
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

        SettingsRow {
            label: "Try failed downloads again"
            description: appController.autoRetryCount > 0
                         ? "After a dropped connection, a timeout or a busy server; waits a little longer each time"
                         : "Failed downloads wait for you to retry them"
            FormField {
                visible: appController.autoRetryCount > 0
                Layout.preferredWidth: 90
                suffix: "times"
                text: appController.autoRetryCount
                validator: IntValidator { bottom: 1; top: 20 }
                accessibleLabel: "Number of tries"
                onEditingFinished: if (field.acceptableInput) appController.autoRetryCount = parseInt(text)
            }
            FormField {
                visible: appController.autoRetryCount > 0
                Layout.preferredWidth: 90
                suffix: "s"
                text: appController.autoRetryDelaySeconds
                validator: IntValidator { bottom: 5; top: 3600 }
                accessibleLabel: "Seconds before the first try"
                onEditingFinished: if (field.acceptableInput) appController.autoRetryDelaySeconds = parseInt(text)
            }
            ToggleSwitch {
                checked: appController.autoRetryCount > 0
                onToggled: appController.autoRetryCount = checked ? 3 : 0
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

    SettingsGroup {
        title: "Folders by type"
        description: "Downloads headed for your download folder go to a folder for their type instead. Choosing a folder in the Add dialog still wins."

        SettingsRow {
            divider: false
            label: "Sort into folders by type"
            ToggleSwitch {
                checked: appController.sortByType
                onToggled: appController.sortByType = checked
            }
        }
        Repeater {
            model: [["video", "Videos"], ["audio", "Music"], ["image", "Pictures"], ["document", "Documents"],
                    ["archive", "Archives"], ["program", "Programs and disk images"]]
            delegate: SettingsRow {
                required property var modelData
                dimmed: !appController.sortByType
                enabled: appController.sortByType
                label: modelData[1]
                description: appController.typeFolders[modelData[0]] || ""
                ModernButton {
                    text: "Change…"
                    variant: "secondary"
                    onClicked: {
                        const dir = appController.pickFolder("Folder for " + modelData[1].toLowerCase(), appController.typeFolders[modelData[0]])
                        if (dir.length > 0) appController.setTypeFolder(modelData[0], dir)
                    }
                }
            }
        }
    }

    SettingsGroup {
        title: "When a download finishes"
        description: "Not for files whose checksum or signature didn't match, or where ClamAV found a threat."

        SettingsRow {
            divider: false
            label: "Unpack archives"
            description: "ZIP, 7z, RAR and tar files are unpacked into a folder beside them; the archive stays"
            ToggleSwitch {
                checked: appController.unpackArchives
                onToggled: appController.unpackArchives = checked
            }
        }
        SettingsRow {
            visible: Qt.platform.os === "linux"
            label: "Remember where files came from"
            description: "Saves the link and its page with the file, as browsers do; file managers show it. Sign-in details are left out."
            ToggleSwitch {
                checked: appController.rememberOrigin
                onToggled: appController.rememberOrigin = checked
            }
        }
        SettingsRow {
            label: "Check signatures"
            description: "Looks for the .asc, .sig or .minisig file published beside a release download and checks it with GnuPG (your keyring) or your minisign keys"
            ToggleSwitch {
                checked: appController.checkSignatures
                onToggled: appController.checkSignatures = checked
            }
        }
        SettingsRow {
            dimmed: !appController.checkSignatures
            enabled: appController.checkSignatures
            label: "Trusted minisign keys"
            description: "The publishers' public keys (they start with RW), separated by spaces"
            FormField {
                Layout.preferredWidth: 260
                monospace: true
                accessibleLabel: "Trusted minisign public keys"
                placeholderText: "RW… (from the publisher's website)"
                text: appController.minisignKeys
                onEditingFinished: {
                    appController.minisignKeys = text
                    text = appController.minisignKeys
                }
            }
        }
        SettingsRow {
            label: "Scan with ClamAV"
            description: appController.clamavAvailable
                         ? "Uses the clamd daemon when it's running, otherwise clamscan. A file with a threat is left where it is, but never opened or unpacked."
                         : "Install ClamAV (the clamav package) to scan downloads."
            ToggleSwitch {
                enabled: appController.clamavAvailable || appController.scanDownloads
                checked: appController.scanDownloads
                onToggled: appController.scanDownloads = checked
            }
        }
        SettingsRow {
            label: "Spot duplicates"
            description: "Says when a finished file is the same as one you already have, and offers to move the new copy to the trash"
            ToggleSwitch {
                checked: appController.spotDuplicates
                onToggled: appController.spotDuplicates = checked
            }
        }
        SettingsRow {
            label: "Run a command"
            description: "%f is the file, %d its folder, %n its name. Run directly, without a shell; not included in backups."
            FormField {
                Layout.preferredWidth: 260
                monospace: true
                accessibleLabel: "Command to run after a download"
                placeholderText: "e.g. notify-send Done %n"
                text: appController.afterCommand
                onEditingFinished: appController.afterCommand = text
            }
        }
    }
}
