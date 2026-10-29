import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 620
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    anchors.centerIn: parent
    padding: 0

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

    ListModel { id: torrentFilesModelInstance }
    ListModel { id: playlistModel }

    property int connectionsCount: 8
    property string errorMessage: ""
    property string torrentName: ""
    property int selectedFilesCount: 0
    property int selectedFilesSize: 0
    property string pendingSource: ""

    property bool mediaContext: false
    property bool pendingMediaMode: false
    property bool sourceIsMedia: false
    property bool mediaAudioOnly: false
    property string mediaQuality: "best"
    property string mediaAudioFormat: "auto"
    readonly property bool showMediaOptions: root.mediaContext || root.sourceIsMedia

    property bool subtitles: false
    property bool embedSubtitles: true
    property bool showMore: false
    property bool startLater: false
    property int mediaInfoToken: -1
    property bool mediaInfoLoading: false
    property var mediaInfo: null
    property int selectedEntries: 0
    readonly property bool playlistMode: playlistModel.count > 0

    function recountEntries() {
        let n = 0
        for (let i = 0; i < playlistModel.count; i++) if (playlistModel.get(i).checked) n++
        selectedEntries = n
    }

    function selectAllEntries(checked) {
        for (let i = 0; i < playlistModel.count; i++) playlistModel.setProperty(i, "checked", checked)
        recountEntries()
    }

    function fetchInfo() {
        const url = sourceField.text.trim()
        if (url.length === 0) return
        playlistModel.clear()
        mediaInfo = null
        selectedEntries = 0
        mediaInfoLoading = true
        mediaInfoToken = downloadModel.fetchMediaInfo(url)
    }

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0) return ""
        const s = Math.round(seconds)
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + mm + ":" + (sec < 10 ? "0" : "") + sec
    }

    function nextOccurrence(hhmm) {
        const parts = hhmm.split(":")
        const when = new Date()
        when.setHours(parseInt(parts[0]), parseInt(parts[1]), 0, 0)
        if (when.getTime() <= Date.now()) when.setDate(when.getDate() + 1)
        return Math.floor(when.getTime() / 1000)
    }

    Connections {
        target: downloadModel
        function onMediaInfoReady(token, info) {
            if (token !== root.mediaInfoToken) return
            root.mediaInfoLoading = false
            root.mediaInfo = info
            playlistModel.clear()
            if (info.ok && info.isPlaylist) {
                for (let i = 0; i < info.entries.length; i++) {
                    const e = info.entries[i]
                    const available = e.title.length > 0
                    playlistModel.append({ entryUrl: e.url, entryTitle: available ? e.title : "Unavailable video", entryDuration: e.duration, checked: available })
                }
            }
            root.recountEntries()
        }
    }

    function resetForm() {
        sourceField.text = ""
        destField.text = appController.downloadDirectory
        connectionsCount = appController.maxConnectionsPerDownload
        torrentFilesModelInstance.clear()
        torrentName = ""
        errorMessage = ""
        selectedFilesCount = 0
        selectedFilesSize = 0
        mediaContext = false
        sourceIsMedia = false
        mediaAudioOnly = false
        mediaQuality = "best"
        mediaAudioFormat = "auto"
        subtitles = Prefs.mediaSubtitles
        subtitleLanguagesField.text = Prefs.mediaSubtitleLanguages
        embedSubtitles = Prefs.mediaEmbedSubtitles
        showMore = false
        startLater = false
        startTimeField.text = "23:00"
        checksumField.text = ""
        playlistModel.clear()
        mediaInfo = null
        mediaInfoLoading = false
        mediaInfoToken = -1
        selectedEntries = 0
    }

    function openWithSource(source) {
        pendingSource = source
        open()
    }

    function openMediaMode() {
        pendingMediaMode = true
        open()
    }

    onOpened: {
        resetForm()
        if (pendingMediaMode) {
            mediaContext = true
            pendingMediaMode = false
        }
        if (pendingSource.length > 0) {
            sourceField.text = pendingSource
            pendingSource = ""
        }
        sourceField.forceActiveFocus()
    }

    function isMagnet(text) {
        return text.indexOf("magnet:?") === 0
    }

    function isTorrentSource(text) {
        return isMagnet(text) || text.toLowerCase().endsWith(".torrent")
    }

    function sourceTypeIcon(text) {
        if (text.length === 0) return root.mediaContext ? "media" : "link"
        if (isMagnet(text)) return "magnet"
        if (text.toLowerCase().endsWith(".torrent")) return "torrent"
        if (downloadModel.isMediaSource(text)) return "media"
        return "link"
    }

    function formatBytes(n) {
        const units = ["B", "KB", "MB", "GB", "TB"]
        let value = n
        let i = 0
        while (value >= 1024 && i < units.length - 1) { value /= 1024; i++ }
        return (i === 0 ? value.toFixed(0) : value.toFixed(2)) + " " + units[i]
    }

    function recomputeSelection() {
        let count = 0
        let size = 0
        for (let i = 0; i < torrentFilesModelInstance.count; i++) {
            const item = torrentFilesModelInstance.get(i)
            if (item.checked) { count++; size += item.fileSize }
        }
        root.selectedFilesCount = count
        root.selectedFilesSize = size
    }

    function tryParseTorrentFile(text) {
        torrentFilesModelInstance.clear()
        torrentName = ""
        if (isMagnet(text) || !text.toLowerCase().endsWith(".torrent")) { recomputeSelection(); return }

        const result = downloadModel.parseTorrentFile(text)
        if (!result.ok) {
            errorMessage = result.error.length > 0 ? ("Could not read torrent: " + result.error) : ""
            recomputeSelection()
            return
        }
        for (let i = 0; i < result.files.length; i++) {
            const f = result.files[i]
            torrentFilesModelInstance.append({ fileIndex: f.index, filePath: f.path, fileSize: f.size, checked: true })
        }
        torrentName = result.name
        recomputeSelection()
    }

    function selectAllFiles(checked) {
        for (let i = 0; i < torrentFilesModelInstance.count; i++) {
            torrentFilesModelInstance.setProperty(i, "checked", checked)
        }
        recomputeSelection()
    }

    function toggleFile(index, checked) {
        torrentFilesModelInstance.setProperty(index, "checked", checked)
        recomputeSelection()
    }

    function submit() {
        const source = sourceField.text.trim()
        if (source.length === 0) {
            errorMessage = "Please enter a URL, magnet link, or torrent file."
            return
        }
        const checksum = checksumField.text.trim()
        if (!torrentSource && !showMediaOptions && checksum.length > 0 && !downloadModel.isValidChecksum(checksum)) {
            errorMessage = "The checksum isn't a valid MD5, SHA-1, SHA-256 or SHA-512 value."
            return
        }
        if (startLater && !startTimeField.field.acceptableInput) {
            errorMessage = "Enter the start time as HH:mm, for example 23:30."
            return
        }
        const startAt = startLater ? nextOccurrence(startTimeField.text) : 0

        if (isTorrentSource(source)) {
            const excluded = []
            for (let i = 0; i < torrentFilesModelInstance.count; i++) {
                const item = torrentFilesModelInstance.get(i)
                if (!item.checked) excluded.push(item.fileIndex)
            }
            if (torrentFilesModelInstance.count > 0 && excluded.length === torrentFilesModelInstance.count) {
                errorMessage = "Select at least one file to download."
                return
            }
            downloadModel.addDownload({ kind: "torrent", source: source, directory: destField.text, excludedFiles: excluded, startAt: startAt })
        } else if (root.showMediaOptions) {
            const base = {
                kind: "media", directory: destField.text, audioOnly: root.mediaAudioOnly, quality: root.mediaQuality,
                audioFormat: root.mediaAudioFormat, subtitles: root.subtitles && !root.mediaAudioOnly,
                subtitleLanguages: subtitleLanguagesField.text.trim(), embedSubtitles: root.embedSubtitles, startAt: startAt
            }
            Prefs.mediaSubtitles = root.subtitles
            Prefs.mediaSubtitleLanguages = subtitleLanguagesField.text.trim()
            Prefs.mediaEmbedSubtitles = root.embedSubtitles
            if (root.playlistMode) {
                if (root.selectedEntries === 0) {
                    errorMessage = "Select at least one item from the playlist."
                    return
                }
                for (let i = 0; i < playlistModel.count; i++) {
                    const entry = playlistModel.get(i)
                    if (!entry.checked) continue
                    const spec = Object.assign({}, base)
                    spec.source = entry.entryUrl
                    spec.title = entry.entryTitle
                    downloadModel.addDownload(spec)
                }
            } else {
                base.source = source
                downloadModel.addDownload(base)
            }
        } else {
            downloadModel.addDownload({ kind: "http", source: source, directory: destField.text, connections: connectionsCount,
                                        checksum: checksum, startAt: startAt })
        }
        root.close()
    }

    function sourceKindLabel(text) {
        if (text.length === 0) return ""
        if (isMagnet(text)) return "Magnet link"
        if (text.toLowerCase().endsWith(".torrent")) return "Torrent file"
        if (root.sourceIsMedia) return "Media page"
        return "Direct download"
    }

    readonly property string sourceText: sourceField.text.trim()
    readonly property bool torrentSource: isTorrentSource(sourceText)

    readonly property string footerSummary: {
        if (sourceText.length === 0) return ""
        if (torrentSource) {
            if (torrentFilesModelInstance.count > 0)
                return selectedFilesCount + " of " + torrentFilesModelInstance.count + " files  ·  " + formatBytes(selectedFilesSize)
            return "File list is fetched once the download starts"
        }
        if (showMediaOptions && playlistMode)
            return selectedEntries + " of " + playlistModel.count + " items selected"
        if (showMediaOptions)
            return mediaAudioOnly ? "Audio only  ·  " + (mediaAudioFormat === "auto" ? "best format" : mediaAudioFormat.toUpperCase())
                                  : "Video + audio  ·  " + (mediaQuality === "best" ? "best quality" : mediaQuality + "p")
        return connectionsCount + (connectionsCount === 1 ? " connection" : " connections")
    }

    component FieldLabel: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
    }

    component SmallButton: Rectangle {
        id: sb
        property string text
        signal clicked()
        implicitWidth: sbText.implicitWidth + 18
        implicitHeight: 24
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.onPressAction: clicked()
        radius: Theme.radiusSmall
        color: sbMouse.containsMouse ? Theme.surfaceHover : "transparent"
        border.width: 1
        border.color: Theme.border
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
        Text {
            id: sbText
            anchors.centerIn: parent
            text: sb.text
            color: Theme.textSecondary
            font.pixelSize: Theme.fontMicro
            font.weight: Font.DemiBold
        }
        MouseArea {
            id: sbMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: sb.clicked()
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 14
            Layout.topMargin: 16
            Layout.bottomMargin: 14
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.accent, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.accent, 0.22)
                Icon { anchors.centerIn: parent; name: root.showMediaOptions ? "media" : "arrowDown"; iconSize: 15; color: Theme.accent }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: root.showMediaOptions ? "Add Media" : "Add Download"
                    color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold; font.letterSpacing: Theme.trackingTight
                }
                Text {
                    text: root.showMediaOptions ? "Save video or audio from YouTube, TikTok, Twitch and similar sites"
                                                 : "Direct links, magnet links, and .torrent files"
                    color: Theme.textTertiary; font.pixelSize: Theme.fontCaption
                }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ScrollView {
            id: bodyScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: body.implicitHeight + 40
            Layout.minimumHeight: 160
            contentWidth: availableWidth
            contentHeight: body.implicitHeight + 40
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: body
                x: 20
                y: 20
                width: bodyScroll.availableWidth - 40
                spacing: 18

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 20
                        FieldLabel { text: "Source"; Layout.fillWidth: true }
                        StatusBadge {
                            visible: root.sourceText.length > 0
                            label: root.sourceKindLabel(root.sourceText)
                            tone: root.torrentSource ? Theme.kindTorrent : (root.sourceIsMedia ? Theme.kindVideo : Theme.accent)
                        }
                    }

                    DropArea {
                        id: dropArea
                        Layout.fillWidth: true
                        Layout.preferredHeight: 46
                        onEntered: (drag) => { if (!drag.hasUrls) drag.accepted = false }
                        onDropped: (drop) => {
                            if (drop.hasUrls && drop.urls.length > 0) {
                                const localPath = appController.toLocalFilePath(drop.urls[0])
                                sourceField.text = localPath.length > 0 ? localPath : drop.urls[0].toString()
                                root.errorMessage = ""
                            }
                        }

                        Rectangle {
                            id: sourceBox
                            anchors.fill: parent
                            radius: Theme.radiusMedium
                            color: dropArea.containsDrag ? Theme.withAlpha(Theme.accent, 0.08) : Theme.surfaceInset
                            border.width: 1
                            border.color: root.errorMessage.length > 0 ? Theme.withAlpha(Theme.danger, 0.7)
                                          : dropArea.containsDrag || sourceField.activeFocus ? Theme.withAlpha(Theme.accent, 0.8)
                                          : sourceHover.hovered ? Theme.borderStrong : Theme.border
                            Behavior on border.color { ColorAnimation { duration: Theme.animFast } }
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }

                            HoverHandler { id: sourceHover }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 6
                                spacing: 10

                                Icon {
                                    name: root.sourceTypeIcon(sourceField.text)
                                    iconSize: 15
                                    color: sourceField.text.length > 0 ? Theme.textPrimary : Theme.textTertiary
                                }

                                TextField {
                                    id: sourceField
                                    Layout.fillWidth: true
                                    placeholderText: root.mediaContext
                                                     ? "https://youtube.com/watch?v=…"
                                                     : "https://example.com/file.zip  or  magnet:?xt=…"
                                    placeholderTextColor: Theme.textDisabled
                                    selectByMouse: true
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontBody
                                    verticalAlignment: TextInput.AlignVCenter
                                    background: Item {}
                                    leftPadding: 0
                                    rightPadding: 0
                                    onTextChanged: {
                                        root.errorMessage = ""
                                        root.tryParseTorrentFile(text)
                                        root.sourceIsMedia = text.length > 0 && downloadModel.isMediaSource(text)
                                        playlistModel.clear()
                                        root.mediaInfo = null
                                        root.mediaInfoLoading = false
                                        root.mediaInfoToken = -1
                                        root.selectedEntries = 0
                                    }
                                    Keys.onReturnPressed: root.submit()
                                    Keys.onEnterPressed: root.submit()
                                }

                                IconButton {
                                    visible: sourceField.text.length > 0
                                    implicitWidth: 24
                                    implicitHeight: 24
                                    iconName: "close"
                                    tooltip: "Clear"
                                    onClicked: { sourceField.text = ""; sourceField.forceActiveFocus() }
                                }

                                ModernButton {
                                    visible: !root.showMediaOptions
                                    text: "Browse…"
                                    variant: "secondary"
                                    onClicked: {
                                        const path = appController.pickTorrentFile()
                                        if (path.length > 0) sourceField.text = path
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Icon {
                            visible: root.errorMessage.length > 0
                            name: "alert"
                            iconSize: 11
                            color: Theme.danger
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.errorMessage.length > 0 ? root.errorMessage
                                  : (dropArea.containsDrag ? "Drop to use this torrent file"
                                     : root.showMediaOptions ? "Works with YouTube, TikTok, Twitch, and similar sites"
                                     : "You can also drag a .torrent file onto this dialog or the main window")
                            color: root.errorMessage.length > 0 ? Theme.danger : (dropArea.containsDrag ? Theme.accent : Theme.textTertiary)
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    FieldLabel { text: "Save to" }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        FormField {
                            id: destField
                            Layout.fillWidth: true
                            implicitHeight: 34
                            iconName: "folder"
                        }

                        ModernButton {
                            text: "Choose…"
                            variant: "secondary"
                            onClicked: {
                                const dir = appController.pickDownloadDirectory()
                                if (dir.length > 0) destField.text = dir
                            }
                        }
                    }
                }

                SettingsGroup {
                    visible: !root.torrentSource

                    SettingsRow {
                        visible: !root.showMediaOptions
                        divider: false
                        label: "Connections"
                        description: "Parallel segments for faster downloads"

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
                                    enabled: root.connectionsCount > 1
                                    onClicked: root.connectionsCount = Math.max(1, root.connectionsCount - 1)
                                }
                                Text {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignHCenter
                                    text: root.connectionsCount
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontBody
                                    font.weight: Font.DemiBold
                                    font.features: { "tnum": 1 }
                                }
                                IconButton {
                                    implicitWidth: 26
                                    implicitHeight: 26
                                    iconName: "plus"
                                    enabled: root.connectionsCount < 32
                                    onClicked: root.connectionsCount = Math.min(32, root.connectionsCount + 1)
                                }
                            }
                        }
                    }

                    SettingsRow {
                        visible: root.showMediaOptions
                        divider: false
                        label: "Download"
                        description: root.mediaAudioOnly ? "Extract the audio track only" : "Video with its best matching audio"
                        SegmentedControl {
                            options: [{ label: "Video + Audio", value: false }, { label: "Audio Only", value: true }]
                            value: root.mediaAudioOnly
                            onActivated: (v) => root.mediaAudioOnly = v
                        }
                    }

                    SettingsRow {
                        visible: root.showMediaOptions && !root.mediaAudioOnly
                        label: "Quality"
                        description: "Falls back to the closest available"
                        SegmentedControl {
                            options: [
                                { label: "Best", value: "best" },
                                { label: "1080p", value: "1080" },
                                { label: "720p", value: "720" },
                                { label: "480p", value: "480" }
                            ]
                            value: root.mediaQuality
                            onActivated: (v) => root.mediaQuality = v
                        }
                    }

                    SettingsRow {
                        visible: root.showMediaOptions && root.mediaAudioOnly
                        label: "Format"
                        description: "Auto keeps the original without re-encoding"
                        SegmentedControl {
                            options: [
                                { label: "Auto", value: "auto" },
                                { label: "M4A", value: "m4a" },
                                { label: "Opus", value: "opus" },
                                { label: "MP3", value: "mp3" }
                            ]
                            value: root.mediaAudioFormat
                            onActivated: (v) => root.mediaAudioFormat = v
                        }
                    }

                    SettingsRow {
                        visible: root.showMediaOptions && !root.mediaAudioOnly
                        label: "Subtitles"
                        description: root.subtitles ? (root.embedSubtitles ? "Embedded in the video" : "Saved next to the video as .srt") : "Not downloaded"
                        FormField {
                            id: subtitleLanguagesField
                            visible: root.subtitles
                            Layout.preferredWidth: 110
                            accessibleLabel: "Subtitle languages"
                            placeholderText: "en, de, fr"
                        }
                        SegmentedControl {
                            visible: root.subtitles
                            options: [{ label: "Embed", value: true }, { label: "Separate", value: false }]
                            value: root.embedSubtitles
                            onActivated: (v) => root.embedSubtitles = v
                        }
                        ToggleSwitch {
                            checked: root.subtitles
                            onToggled: root.subtitles = checked
                        }
                    }

                    SettingsRow {
                        visible: root.showMediaOptions
                        label: root.mediaInfoLoading ? "Checking link…"
                               : root.mediaInfo && root.mediaInfo.ok ? (root.mediaInfo.isPlaylist ? "Playlist · " + playlistModel.count + " items" : (root.mediaInfo.title || "Single video"))
                               : "Playlists and channels"
                        description: root.mediaInfoLoading ? "Asking yt-dlp what this link contains"
                                     : root.mediaInfo && !root.mediaInfo.ok ? root.mediaInfo.error
                                     : root.mediaInfo && root.mediaInfo.ok && !root.mediaInfo.isPlaylist
                                       ? [root.mediaInfo.uploader, root.formatDuration(root.mediaInfo.duration)].filter(x => x && x.length > 0).join("  ·  ")
                                     : root.playlistMode ? (root.mediaInfo.title || "")
                                     : "Fetch the link's details to pick items from a playlist or channel"
                        ModernButton {
                            text: root.mediaInfoLoading ? "Checking…" : (root.mediaInfo ? "Check Again" : "Fetch Info")
                            variant: "secondary"
                            enabled: !root.mediaInfoLoading && root.sourceText.length > 0 && downloadModel.isMediaEngineAvailable()
                            onClicked: root.fetchInfo()
                        }
                    }

                    RowLayout {
                        visible: root.showMediaOptions && !downloadModel.isMediaEngineAvailable()
                        Layout.fillWidth: true
                        Layout.margins: 14
                        spacing: 8
                        Icon { Layout.alignment: Qt.AlignTop; name: "alert"; iconSize: 13; color: Theme.warning }
                        Text {
                            Layout.fillWidth: true
                            text: Theme.windows ? "yt-dlp wasn't found. Place yt-dlp.exe (and ffmpeg.exe) next to Orbit, or install them and add them to PATH."
                                          : "yt-dlp wasn't found. Install yt-dlp (and ffmpeg) with your package manager, or place them next to Orbit."
                            color: Theme.warning
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.showMediaOptions && root.playlistMode
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        FieldLabel { text: "Items" }
                        Text {
                            Layout.fillWidth: true
                            text: root.selectedEntries + " of " + playlistModel.count + " selected"
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontCaption
                        }
                        SmallButton { text: "Select all"; onClicked: root.selectAllEntries(true) }
                        SmallButton { text: "None"; onClicked: root.selectAllEntries(false) }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(204, playlistModel.count * 34 + 2)
                        radius: Theme.radiusLarge
                        color: Theme.panel
                        border.width: 1
                        border.color: Theme.border
                        clip: true

                        ListView {
                            anchors.fill: parent
                            anchors.margins: 1
                            model: playlistModel
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true
                            ScrollBar.vertical: ScrollBar {}

                            delegate: Item {
                                width: ListView.view.width
                                height: 34

                                Accessible.role: Accessible.CheckBox
                                Accessible.name: model.entryTitle
                                Accessible.checked: model.checked
                                Accessible.onToggleAction: { playlistModel.setProperty(index, "checked", !model.checked); root.recountEntries() }

                                Rectangle {
                                    anchors.fill: parent
                                    color: entryMouse.containsMouse ? Theme.rowHover : "transparent"
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 14
                                    spacing: 10
                                    Rectangle {
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 16
                                        radius: 4
                                        color: model.checked ? Theme.accent : "transparent"
                                        border.width: 1.4
                                        border.color: model.checked ? Theme.accent : Theme.withAlpha(Theme.textPrimary, 0.22)
                                        Icon { anchors.centerIn: parent; visible: model.checked; name: "check"; iconSize: 10; color: "#ffffff" }
                                    }
                                    Text {
                                        Layout.preferredWidth: 26
                                        text: index + 1
                                        color: Theme.textTertiary
                                        font.pixelSize: Theme.fontCaption
                                        font.features: { "tnum": 1 }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: model.entryTitle
                                        color: model.checked ? Theme.textPrimary : Theme.textTertiary
                                        font.pixelSize: Theme.fontCaption
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: root.formatDuration(model.entryDuration)
                                        color: Theme.textTertiary
                                        font.pixelSize: Theme.fontCaption
                                        font.features: { "tnum": 1 }
                                    }
                                }
                                MouseArea {
                                    id: entryMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        playlistModel.setProperty(index, "checked", !model.checked)
                                        root.recountEntries()
                                    }
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Rectangle {
                        id: moreToggle
                        Layout.preferredHeight: 22
                        Layout.preferredWidth: moreRow.implicitWidth + 12
                        radius: Theme.radiusSmall
                        color: moreMouse.containsMouse ? Theme.surfaceHover : "transparent"
                        Accessible.role: Accessible.Button
                        Accessible.name: root.showMore ? "Fewer options" : "More options"
                        Accessible.onPressAction: root.showMore = !root.showMore
                        Row {
                            id: moreRow
                            anchors.centerIn: parent
                            spacing: 5
                            Icon {
                                name: "chevronDown"
                                iconSize: 10
                                color: Theme.textSecondary
                                rotation: root.showMore ? 180 : 0
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: root.showMore ? "Fewer options" : "More options"
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                font.weight: Font.DemiBold
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: moreMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showMore = !root.showMore
                        }
                    }

                    SettingsGroup {
                        visible: root.showMore

                        SettingsRow {
                            divider: false
                            label: "Start at a set time"
                            description: root.startLater ? "Waits, paused, until the next time the clock reads this" : "Starts as soon as a slot is free"
                            FormField {
                                id: startTimeField
                                visible: root.startLater
                                Layout.preferredWidth: 90
                                iconName: "clock"
                                monospace: true
                                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                            }
                            ToggleSwitch {
                                checked: root.startLater
                                onToggled: root.startLater = checked
                            }
                        }
                        SettingsRow {
                            visible: !root.torrentSource && !root.showMediaOptions
                            label: "Checksum"
                            description: "Checked when the download finishes"
                            extra: FormField {
                                id: checksumField
                                Layout.fillWidth: true
                                monospace: true
                                iconName: "shield"
                                accessibleLabel: "Checksum"
                                placeholderText: "SHA-256, SHA-1 or MD5 published by the site"
                            }
                        }
                    }
                }

                SettingsGroup {
                    visible: root.torrentSource && torrentFilesModelInstance.count === 0 && root.isMagnet(root.sourceText)

                    SettingsRow {
                        divider: false
                        label: "Magnet link"
                        description: "Orbit fetches the torrent's metadata from peers, then downloads every file it contains."
                        Icon { name: "magnet"; iconSize: 16; color: Theme.kindTorrent }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: torrentFilesModelInstance.count > 0
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        FieldLabel { text: "Files" }
                        Text {
                            Layout.fillWidth: true
                            text: (root.torrentName.length > 0 ? root.torrentName + "  ·  " : "")
                                  + root.selectedFilesCount + " of " + torrentFilesModelInstance.count + " selected  ·  " + root.formatBytes(root.selectedFilesSize)
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideMiddle
                        }
                        SmallButton { text: "Select all"; onClicked: root.selectAllFiles(true) }
                        SmallButton { text: "None"; onClicked: root.selectAllFiles(false) }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(216, torrentFilesModelInstance.count * 34 + 2)
                        radius: Theme.radiusLarge
                        color: Theme.panel
                        border.width: 1
                        border.color: Theme.border
                        clip: true

                        ListView {
                            anchors.fill: parent
                            anchors.margins: 1
                            model: torrentFilesModelInstance
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true
                            ScrollBar.vertical: ScrollBar {}

                            delegate: Item {
                                id: fileRow
                                width: ListView.view.width
                                height: 34

                                Rectangle {
                                    anchors.fill: parent
                                    color: fileMouse.containsMouse ? Theme.rowHover : "transparent"
                                    Behavior on color { ColorAnimation { duration: Theme.animFast } }
                                }
                                Rectangle {
                                    visible: index > 0
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    height: 1
                                    color: Theme.borderSoft
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 14
                                    spacing: 10

                                    Rectangle {
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 16
                                        radius: 4
                                        color: model.checked ? Theme.accent : "transparent"
                                        border.width: 1.4
                                        border.color: model.checked ? Theme.accent : Theme.withAlpha(Theme.textPrimary, 0.22)
                                        Behavior on color { ColorAnimation { duration: Theme.animFast } }

                                        Icon {
                                            anchors.centerIn: parent
                                            visible: model.checked
                                            name: "check"
                                            iconSize: 10
                                            color: "#ffffff"
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: model.filePath
                                        color: model.checked ? Theme.textPrimary : Theme.textTertiary
                                        font.pixelSize: Theme.fontCaption
                                        elide: Text.ElideMiddle
                                    }

                                    Text {
                                        text: root.formatBytes(model.fileSize)
                                        color: model.checked ? Theme.textSecondary : Theme.textDisabled
                                        font.pixelSize: Theme.fontCaption
                                        font.features: { "tnum": 1 }
                                    }
                                }

                                MouseArea {
                                    id: fileMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.toggleFile(index, !model.checked)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 16
            Layout.topMargin: 12
            Layout.bottomMargin: 12
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: root.footerSummary
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            ModernButton { text: "Cancel"; variant: "ghost"; onClicked: root.close() }
            ModernButton {
                text: root.showMediaOptions ? "Add Media" : "Add Download"
                variant: "primary"
                iconName: "plus"
                enabled: root.sourceText.length > 0 && !root.mediaInfoLoading
                onClicked: root.submit()
            }
        }
    }
}
