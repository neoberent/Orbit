import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: Math.min(800, parent ? parent.width - 48 : 800)
    height: Math.min(640, parent ? parent.height - 48 : 640)
    anchors.centerIn: parent
    padding: 0

    signal updateDialogRequested()

    property int currentTab: 0
    onCurrentTabChanged: settingsScroll.contentItem.contentY = 0

    readonly property var pages: [
        { label: "General", icon: "settings", title: "General", description: "Appearance, notifications, power and the clipboard" },
        { label: "Downloads", icon: "downloading", title: "Downloads", description: "Where files go, connections, and bandwidth" },
        { label: "Network", icon: "network", title: "Network", description: "Send Orbit's traffic through a proxy" },
        { label: "BitTorrent", icon: "torrent", title: "BitTorrent", description: "Torrent speed limits and when to stop seeding" },
        { label: "Media", icon: "media", title: "Media", description: "Sign-in for video sites, and keeping yt-dlp up to date" },
        { label: "Browser", icon: "extension", title: "Browser Integration", description: "Hand downloads from your browser to Orbit" },
        { label: "Schedule", icon: "clock", title: "Schedule", description: "When Orbit may download, and how fast" },
        { label: "Remote Access", icon: "globe", title: "Remote Access", description: "Check on downloads from your phone or another computer" },
        { label: "About", icon: "info", title: "About Orbit", description: "Version, updates, backups, and troubleshooting" }
    ]

    property string proxyMode: "none"
    property string proxyMessage: ""
    property bool proxyMessageOk: true
    property string backupMessage: ""
    property bool backupMessageOk: true

    function applyProxy() {
        const error = appController.setProxy(root.proxyMode, proxyHostField.text, parseInt(proxyPortField.text || "0"),
                                             proxyUserField.text, proxyPasswordField.text, false)
        root.proxyMessageOk = error.length === 0
        root.proxyMessage = error.length > 0 ? error
                            : (root.proxyMode === "none" ? "Orbit connects directly" : "Saved - new connections go through the proxy")
        proxyPasswordField.text = ""
    }

    function exportBackup() {
        const path = appController.pickBackupFileToSave()
        if (path.length === 0) return
        const error = appController.exportBackup(path, includeDownloadsToggle.checked)
        root.backupMessageOk = error.length === 0
        root.backupMessage = error.length > 0 ? error : "Saved to " + path
    }

    function importBackup() {
        const path = appController.pickBackupFileToOpen()
        if (path.length === 0) return
        const result = appController.importBackup(path, includeDownloadsToggle.checked)
        root.backupMessageOk = result.ok
        if (!result.ok) {
            root.backupMessage = result.error
            return
        }
        root.backupMessage = "Restored " + result.settings + " settings" + (includeDownloadsToggle.checked ? " and " + result.downloads + " downloads" : "")
        restartConfirm.ask("Restart Orbit?", "Some restored settings take effect after a restart.", "Restart Now", false,
                           () => appController.restartApp())
    }

    ConfirmDialog { id: restartConfirm; parent: Overlay.overlay }

    property int clockTick: 0
    Timer { interval: 30000; repeat: true; running: root.visible; onTriggered: root.clockTick++ }

    function relativeTime(when) {
        void root.clockTick
        if (!when || isNaN(when.getTime())) return ""
        const minutes = Math.floor((Date.now() - when.getTime()) / 60000)
        if (minutes < 1) return "just now"
        if (minutes < 60) return minutes + (minutes === 1 ? " minute ago" : " minutes ago")
        const hours = Math.floor(minutes / 60)
        if (hours < 24) return hours + (hours === 1 ? " hour ago" : " hours ago")
        return Qt.formatDateTime(when, "MMM d, yyyy 'at' h:mm AP")
    }

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
        const checked = relativeTime(ytDlpUpdater.lastChecked)
        return checked.length > 0 ? source + "  ·  Last checked " + checked : source
    }

    function updateDetail() {
        if (updateManager.state === "error") return updateManager.errorMessage
        const checked = relativeTime(updateManager.lastChecked)
        const current = "You're on version " + updateManager.currentVersion
        return checked.length > 0 ? current + "  ·  Last checked " + checked : current
    }

    onOpened: {
        remoteUsernameField.text = appController.remoteCredentialsConfigured ? appController.remoteUsername : ""
        remotePasswordField.text = ""
        remotePasswordConfirmField.text = ""
        remoteCredError.text = ""
        portField.text = appController.remotePort
        speedLimitField.text = appController.httpSpeedLimitKBps > 0 ? appController.httpSpeedLimitKBps : 1024
        scheduleStartField.text = appController.scheduleStartTime
        scheduleEndField.text = appController.scheduleEndTime
        ruleStartField.text = appController.speedRuleStartTime
        ruleEndField.text = appController.speedRuleEndTime
        ruleLimitField.text = appController.speedRuleLimitKBps
        proxyMode = appController.proxyType
        proxyHostField.text = appController.proxyHost
        proxyPortField.text = appController.proxyPort > 0 ? appController.proxyPort : ""
        proxyUserField.text = appController.proxyUsername
        proxyPasswordField.text = ""
        proxyMessage = ""
        backupMessage = ""
        torrentDownField.text = appController.torrentDownloadLimitKBps > 0 ? appController.torrentDownloadLimitKBps : 2048
        torrentUpField.text = appController.torrentUploadLimitKBps > 0 ? appController.torrentUploadLimitKBps : 512
        seedRatioField.text = appController.seedRatioLimit > 0 ? appController.seedRatioLimit : 2
        seedTimeField.text = appController.seedTimeLimitMinutes > 0 ? appController.seedTimeLimitMinutes : 120
        clipboardExtensionsField.text = clipboardMonitor.extensions
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
        NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: Theme.animFast }
    }

    Overlay.modal: Rectangle {
        color: Theme.withAlpha("#000000", 0.6)
    }

    background: DialogSurface {}

    component ValueChip: Rectangle {
        property string text
        implicitWidth: Math.max(32, chipText.implicitWidth + 16)
        implicitHeight: 22
        radius: Theme.radiusSmall
        color: Theme.surfaceInset
        border.width: 1
        border.color: Theme.border
        Text {
            id: chipText
            anchors.centerIn: parent
            text: parent.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            font.features: { "tnum": 1 }
        }
    }

    component StepText: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }

    contentItem: RowLayout {
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 196
            Layout.fillHeight: true
            Layout.leftMargin: 1
            Layout.topMargin: 1
            Layout.bottomMargin: 1
            color: Theme.surfaceInset
            radius: Theme.radiusXl - 1

            Rectangle {
                anchors.right: parent.right
                width: Theme.radiusXl
                height: parent.height
                color: parent.color
            }
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 18
                anchors.bottomMargin: 14
                spacing: 2

                Text {
                    Layout.leftMargin: 20
                    Layout.bottomMargin: 16
                    text: "Settings"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.trackingTight
                }

                Repeater {
                    model: root.pages
                    SidebarItem {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        iconName: modelData.icon
                        label: modelData.label
                        selected: root.currentTab === index
                        count: -1
                        onClicked: root.currentTab = index
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 14
                Layout.topMargin: 16
                Layout.bottomMargin: 14
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: root.pages[root.currentTab].title
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        font.letterSpacing: Theme.trackingTight
                    }
                    Text {
                        text: root.pages[root.currentTab].description
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontCaption
                    }
                }
                IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            ScrollView {
                id: settingsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                contentWidth: availableWidth

                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 0

                    ColumnLayout {
                        visible: root.currentTab === 0
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

                        SettingsGroup {
                            title: "Appearance"

                            SettingsRow {
                                divider: false
                                label: "Dark theme"
                                description: "Use a dark interface throughout Orbit"
                                ToggleSwitch {
                                    checked: appController.darkTheme
                                    onToggled: appController.darkTheme = checked
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Notifications"

                            SettingsRow {
                                divider: false
                                label: "Notify when downloads finish"
                                description: "Shows a " + (Theme.windows ? "Windows " : "desktop ") + "notification when a download completes or fails while Orbit is in the background"
                                ToggleSwitch {
                                    checked: Prefs.notifyOnFinish
                                    onToggled: Prefs.notifyOnFinish = checked
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Power"

                            SettingsRow {
                                divider: false
                                label: "Keep the PC awake while downloading"
                                description: (Theme.windows ? "Windows" : "Your PC") + " won't go to sleep while a download is running"
                                ToggleSwitch {
                                    checked: powerManager.preventSleep
                                    onToggled: powerManager.preventSleep = checked
                                }
                            }
                            SettingsRow {
                                label: "When downloads finish"
                                description: "Runs once after everything that's downloading or queued is done, with 30 seconds to cancel"
                                SegmentedControl {
                                    options: {
                                        const list = [{ label: "Nothing", value: "none" }, { label: "Exit", value: "exit" }, { label: "Sleep", value: "sleep" }]
                                        if (powerManager.hibernateAvailable) list.push({ label: "Hibernate", value: "hibernate" })
                                        list.push({ label: "Shut down", value: "shutdown" })
                                        return list
                                    }
                                    value: powerManager.whenDoneAction
                                    onActivated: (v) => powerManager.whenDoneAction = v
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Clipboard"

                            SettingsRow {
                                divider: false
                                label: "Offer to download copied links"
                                description: "When you copy a magnet link or a link to a file in another app, Orbit offers to download it"
                                ToggleSwitch {
                                    checked: clipboardMonitor.enabled
                                    onToggled: clipboardMonitor.enabled = checked
                                }
                            }
                            SettingsRow {
                                label: "File types"
                                description: "Extensions that count as a download link"
                                dimmed: !clipboardMonitor.enabled
                                enabled: clipboardMonitor.enabled
                                extra: FormField {
                                    id: clipboardExtensionsField
                                    Layout.fillWidth: true
                                    monospace: true
                                    onEditingFinished: clipboardMonitor.extensions = text
                                }
                            }
                            SettingsRow {
                                label: "Include video page links"
                                description: "YouTube, TikTok, Twitch and similar"
                                dimmed: !clipboardMonitor.enabled
                                enabled: clipboardMonitor.enabled
                                ToggleSwitch {
                                    checked: clipboardMonitor.catchMedia
                                    onToggled: clipboardMonitor.catchMedia = checked
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.currentTab === 1
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

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

                    ColumnLayout {
                        visible: root.currentTab === 2
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

                        SettingsGroup {
                            title: "Proxy"
                            description: "Applies to direct downloads, torrents (peers, trackers and DHT), media downloads and update checks."

                            SettingsRow {
                                divider: false
                                label: "Connection"
                                SegmentedControl {
                                    options: [{ label: "Direct", value: "none" }, { label: "HTTP", value: "http" }, { label: "SOCKS5", value: "socks5" }]
                                    value: root.proxyMode
                                    onActivated: (v) => root.proxyMode = v
                                }
                            }
                            SettingsRow {
                                label: "Server"
                                description: "Host name or IP address, and port"
                                dimmed: root.proxyMode === "none"
                                enabled: root.proxyMode !== "none"
                                FormField {
                                    id: proxyHostField
                                    Layout.preferredWidth: 200
                                    monospace: true
                                    accessibleLabel: "Proxy host"
                                    placeholderText: "127.0.0.1"
                                }
                                FormField {
                                    id: proxyPortField
                                    Layout.preferredWidth: 80
                                    monospace: true
                                    accessibleLabel: "Proxy port"
                                    placeholderText: "1080"
                                    validator: IntValidator { bottom: 1; top: 65535 }
                                }
                            }
                            SettingsRow {
                                label: "Username"
                                description: "Leave empty if the proxy doesn't need a sign-in"
                                dimmed: root.proxyMode === "none"
                                enabled: root.proxyMode !== "none"
                                FormField {
                                    id: proxyUserField
                                    Layout.preferredWidth: 200
                                    accessibleLabel: "Proxy username"
                                }
                            }
                            SettingsRow {
                                label: "Password"
                                description: appController.proxyHasPassword ? "A password is saved (encrypted for your " + Theme.osName + " account). Type a new one to replace it." : "Stored encrypted for your " + Theme.osName + " account"
                                dimmed: root.proxyMode === "none"
                                enabled: root.proxyMode !== "none"
                                FormField {
                                    id: proxyPasswordField
                                    Layout.preferredWidth: 200
                                    echoMode: TextInput.Password
                                    accessibleLabel: "Proxy password"
                                    placeholderText: appController.proxyHasPassword ? "••••••••" : ""
                                }
                            }
                            SettingsRow {
                                label: " "
                                Text {
                                    visible: root.proxyMessage.length > 0
                                    text: root.proxyMessage
                                    color: root.proxyMessageOk ? Theme.success : Theme.danger
                                    font.pixelSize: Theme.fontCaption
                                }
                                ModernButton {
                                    text: "Apply"
                                    variant: "primary"
                                    onClicked: root.applyProxy()
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.currentTab === 3
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

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

                    ColumnLayout {
                        visible: root.currentTab === 4
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

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
                                label: root.ytDlpTitle()
                                description: root.ytDlpDetail()
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

                    ColumnLayout {
                        visible: root.currentTab === 5
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

                        SettingsGroup {
                            title: "Browser integration"

                            SettingsRow {
                                divider: false
                                label: "Connect Orbit to your browsers"
                                description: "Registers orbit:// links, .torrent files and the extension's native connection for your " + Theme.osName + " account"
                                ToggleSwitch {
                                    checked: appController.browserIntegrationEnabled
                                    onToggled: appController.browserIntegrationEnabled = checked
                                }
                            }
                            SettingsRow {
                                label: "Status"
                                description: appController.browserIntegrationStatus
                                StatusBadge {
                                    label: appController.nativeHostRegistered ? "Connected" : (appController.browserIntegrationEnabled ? "Needs repair" : "Off")
                                    tone: appController.nativeHostRegistered ? Theme.success : (appController.browserIntegrationEnabled ? Theme.warning : Theme.textTertiary)
                                }
                                ModernButton {
                                    visible: appController.browserIntegrationEnabled
                                    text: "Repair"
                                    variant: "secondary"
                                    iconName: "retry"
                                    onClicked: appController.repairBrowserIntegration()
                                }
                            }
                            SettingsRow {
                                label: "Extension ID"
                                description: "Chrome, Edge and Brave"
                                Text {
                                    text: appController.chromeExtensionId
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontCaption
                                    font.family: Theme.monoFamily
                                }
                            }
                        }

                        SettingsGroup {
                            title: "How it works"

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.margins: 14
                                spacing: 6
                                StepText { text: "1. Install the Orbit extension in Chrome, Edge, Brave or Firefox." }
                                StepText { text: "2. Click its toolbar icon once and choose “Allow” so Orbit receives your sign-in cookies with each download." }
                                StepText { text: "3. Right-click a link and choose Download with Orbit, or turn on auto-catch in the extension. Downloads behind a sign-in now work, and the browser stops asking to open Orbit." }
                                StepText { text: "Cookies are only kept until the download finishes, and are never included in backups."; color: Theme.textTertiary }
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.currentTab === 7
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

                        SettingsGroup {
                            title: "Web dashboard"

                            SettingsRow {
                                divider: false
                                label: "Enable remote access"
                                description: "Runs a private, password-protected dashboard for your downloads"
                                ToggleSwitch {
                                    checked: appController.remoteAccessEnabled
                                    onToggled: appController.remoteAccessEnabled = checked
                                }
                            }

                            SettingsRow {
                                label: "Status"
                                description: appController.remoteStatusMessage
                                StatusBadge {
                                    label: appController.remoteRunning ? "Running" : (appController.remoteAccessEnabled ? "Offline" : "Off")
                                    tone: appController.remoteRunning ? Theme.success : (appController.remoteAccessEnabled ? Theme.danger : Theme.textTertiary)
                                }
                            }

                            SettingsRow {
                                label: "Port"
                                description: "Always bound to 127.0.0.1 and never exposed directly to your network"
                                FormField {
                                    id: portField
                                    Layout.preferredWidth: 100
                                    monospace: true
                                    validator: IntValidator { bottom: 1024; top: 65535 }
                                    onEditingFinished: appController.remotePort = parseInt(text.length > 0 ? text : "8765")
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Sign-in"
                            description: appController.remoteCredentialsConfigured
                                         ? "Signed in as “" + appController.remoteUsername + "”. Enter a new password to change it."
                                         : "Required before remote access can turn on. Use a password you don't use anywhere else."

                            SettingsRow {
                                divider: false
                                label: "Username"
                                FormField {
                                    id: remoteUsernameField
                                    Layout.preferredWidth: 240
                                    placeholderText: "Username"
                                }
                            }
                            SettingsRow {
                                label: "New password"
                                description: "At least 8 characters"
                                FormField {
                                    id: remotePasswordField
                                    Layout.preferredWidth: 240
                                    echoMode: TextInput.Password
                                    placeholderText: "New password"
                                }
                            }
                            SettingsRow {
                                label: "Confirm password"
                                FormField {
                                    id: remotePasswordConfirmField
                                    Layout.preferredWidth: 240
                                    echoMode: TextInput.Password
                                    placeholderText: "Repeat password"
                                }
                            }
                            SettingsRow {
                                label: " "
                                implicitHeight: 52
                                Text {
                                    id: remoteCredError
                                    text: ""
                                    visible: text.length > 0
                                    color: Theme.danger
                                    font.pixelSize: Theme.fontCaption
                                }
                                ModernButton {
                                    text: "Save Credentials"
                                    variant: "primary"
                                    onClicked: {
                                        if (remoteUsernameField.text.trim().length === 0) {
                                            remoteCredError.text = "Enter a username."
                                            return
                                        }
                                        if (remotePasswordField.text.length < 8) {
                                            remoteCredError.text = "Password must be at least 8 characters."
                                            return
                                        }
                                        if (remotePasswordField.text !== remotePasswordConfirmField.text) {
                                            remoteCredError.text = "Passwords don't match."
                                            return
                                        }
                                        if (appController.setRemoteCredentials(remoteUsernameField.text.trim(), remotePasswordField.text)) {
                                            remoteCredError.text = ""
                                            remotePasswordField.text = ""
                                            remotePasswordConfirmField.text = ""
                                        } else {
                                            remoteCredError.text = "Couldn't save those credentials."
                                        }
                                    }
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Access from anywhere"
                            description: "The dashboard only listens on this PC. To reach it from your phone or another network, tunnel it out with Cloudflare's free cloudflared tool instead of opening a port on your router."

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.margins: 14
                                spacing: 6

                                RowLayout {
                                    spacing: 8
                                    Text { text: "Quick Tunnel"; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold }
                                    StatusBadge { label: "Testing"; tone: Theme.textTertiary }
                                }
                                StepText { text: "1. Install cloudflared" }
                                CopyableCommand { command: "winget install --id Cloudflare.cloudflared" }
                                StepText { text: "2. Run this in a terminal" }
                                CopyableCommand { command: "cloudflared tunnel --url " + appController.remoteLocalUrl }
                                StepText { text: "3. Open the printed *.trycloudflare.com link on your phone and sign in with your Orbit username and password." }
                                StepText { text: "The link changes every time you run the command. Good for a quick check, not for daily use."; color: Theme.textTertiary }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.preferredHeight: 1; color: Theme.border }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.margins: 14
                                spacing: 6

                                RowLayout {
                                    spacing: 8
                                    Text { text: "Named Tunnel"; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold }
                                    StatusBadge { label: "Recommended"; tone: Theme.success }
                                }
                                StepText { text: "1. Link your Cloudflare account" }
                                CopyableCommand { command: "cloudflared tunnel login" }
                                StepText { text: "2. Create a tunnel named orbit" }
                                CopyableCommand { command: "cloudflared tunnel create orbit" }
                                StepText { text: "3. Route a hostname to it" }
                                CopyableCommand { command: "cloudflared tunnel route dns orbit orbit.yourdomain.com" }
                                StepText { text: "4. Point the tunnel's config.yml at " + appController.remoteLocalUrl + ", then run" }
                                CopyableCommand { command: "cloudflared tunnel run orbit" }
                                StepText { text: "5. Optional: keep it running after a reboot" }
                                CopyableCommand { command: "cloudflared service install" }
                                StepText { text: "Then visit https://orbit.yourdomain.com from anywhere."; color: Theme.textTertiary }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: warnText.implicitHeight + 22
                            radius: Theme.radiusLarge
                            color: Theme.withAlpha(Theme.warning, 0.07)
                            border.width: 1
                            border.color: Theme.withAlpha(Theme.warning, 0.22)

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 11
                                anchors.leftMargin: 14
                                spacing: 10
                                Icon { Layout.alignment: Qt.AlignTop; name: "alert"; iconSize: 14; color: Theme.warning }
                                Text {
                                    id: warnText
                                    Layout.fillWidth: true
                                    text: "Keep Orbit's username and password protection on at all times. The tunnel only controls who can reach the page, not who can sign in."
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontCaption
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.currentTab === 6
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

                        SettingsGroup {
                            title: "Download window"
                            description: "Crossing midnight is fine, e.g. 22:00 to 07:00 - the window belongs to the day it starts on."

                            SettingsRow {
                                divider: false
                                label: "Only download during a window"
                                description: "Active downloads pause outside the window and resume when it opens"
                                ToggleSwitch {
                                    checked: appController.scheduleEnabled
                                    onToggled: appController.scheduleEnabled = checked
                                }
                            }

                            SettingsRow {
                                label: "Days"
                                dimmed: !appController.scheduleEnabled
                                enabled: appController.scheduleEnabled
                                DayPicker {
                                    days: appController.scheduleDays
                                    onChanged: (d) => appController.scheduleDays = d
                                }
                            }

                            SettingsRow {
                                label: "From"
                                description: "24-hour time"
                                dimmed: !appController.scheduleEnabled
                                enabled: appController.scheduleEnabled
                                FormField {
                                    id: scheduleStartField
                                    Layout.preferredWidth: 90
                                    iconName: "clock"
                                    monospace: true
                                    placeholderText: "22:00"
                                    validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                                    onEditingFinished: if (field.acceptableInput) appController.scheduleStartTime = text
                                }
                                Text { text: "to"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
                                FormField {
                                    id: scheduleEndField
                                    Layout.preferredWidth: 90
                                    iconName: "clock"
                                    monospace: true
                                    placeholderText: "07:00"
                                    validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                                    onEditingFinished: if (field.acceptableInput) appController.scheduleEndTime = text
                                }
                            }
                        }

                        SettingsGroup {
                            title: "Scheduled speed limit"
                            description: "Caps direct downloads and torrents during busy hours; the lower of this and your normal limit applies."

                            SettingsRow {
                                divider: false
                                label: "Limit speed on a schedule"
                                description: appController.speedRuleActive ? "In effect right now" : "Not in effect right now"
                                ToggleSwitch {
                                    checked: appController.speedRuleEnabled
                                    onToggled: appController.speedRuleEnabled = checked
                                }
                            }
                            SettingsRow {
                                label: "Days"
                                dimmed: !appController.speedRuleEnabled
                                enabled: appController.speedRuleEnabled
                                DayPicker {
                                    days: appController.speedRuleDays
                                    onChanged: (d) => appController.speedRuleDays = d
                                }
                            }
                            SettingsRow {
                                label: "From"
                                dimmed: !appController.speedRuleEnabled
                                enabled: appController.speedRuleEnabled
                                FormField {
                                    id: ruleStartField
                                    Layout.preferredWidth: 90
                                    iconName: "clock"
                                    monospace: true
                                    validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                                    onEditingFinished: if (field.acceptableInput) appController.speedRuleStartTime = text
                                }
                                Text { text: "to"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
                                FormField {
                                    id: ruleEndField
                                    Layout.preferredWidth: 90
                                    iconName: "clock"
                                    monospace: true
                                    validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                                    onEditingFinished: if (field.acceptableInput) appController.speedRuleEndTime = text
                                }
                            }
                            SettingsRow {
                                label: "Maximum speed"
                                dimmed: !appController.speedRuleEnabled
                                enabled: appController.speedRuleEnabled
                                FormField {
                                    id: ruleLimitField
                                    Layout.preferredWidth: 120
                                    suffix: "KB/s"
                                    validator: IntValidator { bottom: 1; top: 10000000 }
                                    onEditingFinished: appController.speedRuleLimitKBps = parseInt(text.length > 0 ? text : "512")
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "A download you pause yourself stays paused, even after the window opens. To start one download later, use Start at a set time in the Add dialog or in Download Options."
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }

                    ColumnLayout {
                        visible: root.currentTab === 8
                        Layout.fillWidth: true
                        Layout.margins: 24
                        Layout.topMargin: 20
                        spacing: 22

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
                                label: root.updateTitle()
                                description: root.updateDetail()
                                ModernButton {
                                    visible: updateManager.updateAvailable && updateManager.state !== "checking"
                                    text: updateManager.state === "ready" ? "Install Update" : "View Update"
                                    variant: "primary"
                                    onClicked: root.updateDialogRequested()
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
                                label: root.backupMessage.length > 0 ? root.backupMessage : "Back up or move Orbit to another PC"
                                ModernButton { text: "Import…"; variant: "secondary"; onClicked: root.importBackup() }
                                ModernButton { text: "Export…"; variant: "secondary"; iconName: "save"; onClicked: root.exportBackup() }
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
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 16
                Layout.topMargin: 12
                Layout.bottomMargin: 12

                Text {
                    Layout.fillWidth: true
                    text: "Changes are saved automatically"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                }
                ModernButton { text: "Done"; variant: "primary"; onClicked: root.close() }
            }
        }
    }
}
