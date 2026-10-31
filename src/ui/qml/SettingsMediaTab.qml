import QtQuick
import QtQuick.Layouts

// Media settings: sign-in for video sites and keeping yt-dlp current.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    function ytDlpTitle() {
        const version = ytDlpUpdater.currentVersion.length > 0 ? "yt-dlp " + ytDlpUpdater.currentVersion : "yt-dlp"
        switch (ytDlpUpdater.state) {
        case "detecting": return "Looking for yt-dlp…"
        case "checking": return "Checking for a newer yt-dlp…"
        case "downloading": return "Downloading yt-dlp " + ytDlpUpdater.latestVersion + "… " + Math.round(ytDlpUpdater.progress * 100) + "%"
        case "installing": return "Installing yt-dlp " + ytDlpUpdater.latestVersion + "…"
        case "updated": return "Updated to yt-dlp " + ytDlpUpdater.currentVersion
        case "upToDate": return version + " is up to date"
        default: return ytDlpUpdater.currentVersion.length > 0 ? version : "yt-dlp not found"
        }
    }

    function ytDlpDetail() {
        if (ytDlpUpdater.state === "error") return ytDlpUpdater.errorMessage
        const source = ytDlpUpdater.usingUpdatedCopy ? "Updated copy in Orbit's data folder" : "Copy bundled with Orbit"
        const checked = tab.dialog.relativeTime(ytDlpUpdater.lastChecked)
        return checked.length > 0 ? source + "  ·  Last checked " + checked : source
    }

    SettingsGroup {
        title: "Sign-in for video sites"
        description: "Age-restricted, members-only and private videos need your sign-in. Links sent from the Orbit extension already carry it; for pasted links, yt-dlp can read cookies from a browser."

        SettingsRow {
            divider: false
            label: "Read cookies from"
            description: appController.mediaCookiesBrowser === "" ? "Off - pasted links are downloaded signed out"
                         : appController.mediaCookiesBrowser === "firefox" ? "Firefox works while it's open or closed"
                         : "Chrome-based browsers may need to be closed while the download starts"
            SegmentedControl {
                options: [{ label: "Off", value: "" }, { label: "Firefox", value: "firefox" }, { label: "Chrome", value: "chrome" },
                          { label: "Edge", value: "edge" }, { label: "Brave", value: "brave" }]
                value: appController.mediaCookiesBrowser
                onActivated: (v) => appController.mediaCookiesBrowser = v
            }
        }
    }

    SettingsGroup {
        title: "Engine"
        description: "Video sites change often, and an older yt-dlp stops working with them. Updates come from yt-dlp's official GitHub releases and are checked against their published SHA-256 checksums."
        SettingsRow {
            divider: false
            label: tab.ytDlpTitle()
            description: tab.ytDlpDetail()
            ModernButton {
                readonly property bool busy: ["detecting", "checking", "downloading", "installing"].indexOf(ytDlpUpdater.state) >= 0
                enabled: !busy
                text: busy ? "Working…" : "Check for Updates"
                variant: "secondary"
                iconName: "retry"
                onClicked: ytDlpUpdater.checkNow()
            }
        }
        SettingsRow {
            label: "Keep yt-dlp up to date"
            description: "Checks once a day, and after a media download fails"
            ToggleSwitch {
                checked: ytDlpUpdater.autoUpdate
                onToggled: ytDlpUpdater.autoUpdate = checked
            }
        }
        SettingsRow {
            label: "ffmpeg"
            description: "Merges video and audio, and converts audio formats"
            StatusBadge {
                label: downloadModel.isFfmpegAvailable() ? "Ready" : "Missing"
                tone: downloadModel.isFfmpegAvailable() ? Theme.success : Theme.warning
            }
        }
    }
}
