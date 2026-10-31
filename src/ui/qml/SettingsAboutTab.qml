import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// About: version, updates, backups and troubleshooting.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    property string backupMessage: ""
    property bool backupMessageOk: true

    function exportBackup() {
        const path = appController.pickBackupFileToSave()
        if (path.length === 0) return
        const error = appController.exportBackup(path, includeDownloadsToggle.checked)
        tab.backupMessageOk = error.length === 0
        tab.backupMessage = error.length > 0 ? error : "Saved to " + path
    }

    function importBackup() {
        const path = appController.pickBackupFileToOpen()
        if (path.length === 0) return
        const result = appController.importBackup(path, includeDownloadsToggle.checked)
        tab.backupMessageOk = result.ok
        if (!result.ok) {
            tab.backupMessage = result.error
            return
        }
        tab.backupMessage = "Restored " + result.settings + " settings" + (includeDownloadsToggle.checked ? " and " + result.downloads + " downloads" : "")
        restartConfirm.ask("Restart Orbit?", "Some restored settings take effect after a restart.", "Restart Now", false,
                           () => appController.restartApp())
    }

    ConfirmDialog { id: restartConfirm; parent: Overlay.overlay }

    function updateTitle() {
        switch (updateManager.state) {
        case "checking": return "Checking for updates…"
        case "upToDate": return "Orbit is up to date"
        case "available": return "Orbit " + updateManager.latestVersion + " is available"
        case "downloading": return "Downloading Orbit " + updateManager.latestVersion + "…"
        case "ready": return "Orbit " + updateManager.latestVersion + " is ready to install"
        case "error": return updateManager.updateAvailable ? "Orbit " + updateManager.latestVersion + " is available" : "Couldn't check for updates"
        default: return "Orbit " + updateManager.currentVersion
        }
    }

    function updateDetail() {
        if (updateManager.state === "error") return updateManager.errorMessage
        const checked = tab.dialog.relativeTime(updateManager.lastChecked)
        const current = "You're on version " + updateManager.currentVersion
        return checked.length > 0 ? current + "  ·  Last checked " + checked : current
    }

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        tab.backupMessage = ""
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 14

        Image {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            source: "qrc:/images/logo.png"
            sourceSize: Qt.size(88, 88)
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: "Orbit Download Manager"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Text {
                text: "Version " + Qt.application.version
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    SettingsGroup {
        title: "Updates"

        SettingsRow {
            divider: false
            label: tab.updateTitle()
            description: tab.updateDetail()
            ModernButton {
                visible: updateManager.updateAvailable && updateManager.state !== "checking"
                text: updateManager.state === "ready" ? "Install Update" : "View Update"
                variant: "primary"
                onClicked: tab.dialog.updateDialogRequested()
            }
            ModernButton {
                visible: !updateManager.updateAvailable || updateManager.state === "checking"
                enabled: updateManager.state !== "checking"
                text: updateManager.state === "checking" ? "Checking…" : "Check for Updates"
                variant: "secondary"
                iconName: "retry"
                onClicked: updateManager.checkNow()
            }
        }
        SettingsRow {
            label: "Check for updates automatically"
            description: "Looks for a new version when Orbit starts and every few hours while it runs"
            ToggleSwitch {
                checked: updateManager.autoCheck
                onToggled: updateManager.autoCheck = checked
            }
        }
    }

    SettingsGroup {
        title: "Components"

        SettingsRow {
            divider: false
            label: "Direct downloads"
            description: "Segmented HTTP(S) engine with resume support"
            StatusBadge { label: "Ready"; tone: Theme.success }
        }
        SettingsRow {
            label: "BitTorrent"
            description: "libtorrent: magnet links, .torrent files, DHT"
            StatusBadge { label: "Ready"; tone: Theme.success }
        }
        SettingsRow {
            label: "Media downloads"
            description: downloadModel.isMediaEngineAvailable()
                         ? (ytDlpUpdater.currentVersion.length > 0 ? "yt-dlp " + ytDlpUpdater.currentVersion + " and ffmpeg" : "yt-dlp and ffmpeg found next to Orbit or on PATH")
                         : (Theme.windows ? "yt-dlp.exe" : "yt-dlp") + " was not found next to Orbit or on PATH"
            StatusBadge {
                label: downloadModel.isMediaEngineAvailable() ? "Ready" : "Missing"
                tone: downloadModel.isMediaEngineAvailable() ? Theme.success : Theme.warning
            }
        }
        SettingsRow {
            label: "Web dashboard"
            description: appController.remoteAccessEnabled ? appController.remoteStatusMessage : "Turned off in Remote Access"
            StatusBadge {
                label: appController.remoteRunning ? "Running" : "Off"
                tone: appController.remoteRunning ? Theme.success : Theme.textTertiary
            }
        }
    }

    SettingsGroup {
        title: "Backup and restore"
        description: "Settings, and optionally your download list. Passwords and browser cookies are never included."

        SettingsRow {
            divider: false
            label: "Include the download list"
            description: "Restored downloads that weren't finished come back paused"
            ToggleSwitch { id: includeDownloadsToggle; checked: true }
        }
        SettingsRow {
            label: tab.backupMessage.length > 0 ? tab.backupMessage : "Back up or move Orbit to another PC"
            ModernButton { text: "Import…"; variant: "secondary"; onClicked: tab.importBackup() }
            ModernButton { text: "Export…"; variant: "secondary"; iconName: "save"; onClicked: tab.exportBackup() }
        }
    }

    SettingsGroup {
        title: "Troubleshooting"

        SettingsRow {
            divider: false
            label: "Mode"
            description: appController.portable ? "Portable - everything is stored next to " + Theme.exeName + ", nothing in your user profile"
                                                : "Installed - to run Orbit from a USB drive, put an empty file named portable.flag next to " + Theme.exeName
            StatusBadge {
                label: appController.portable ? "Portable" : "Installed"
                tone: appController.portable ? Theme.accent : Theme.textTertiary
            }
        }
        SettingsRow {
            label: "Data folder"
            description: appController.dataFolderPath() + "  ·  logs (orbit.log) and download state"
            ModernButton {
                text: "Open"
                variant: "secondary"
                iconName: "folder"
                onClicked: appController.openDataFolder()
            }
        }
    }
}
