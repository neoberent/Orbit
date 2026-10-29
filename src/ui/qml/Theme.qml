pragma Singleton
import QtQuick

QtObject {
    id: theme

    property bool dark: true

    // For the few texts that name the operating system or Orbit's program file.
    readonly property bool windows: Qt.platform.os === "windows"
    readonly property string osName: windows ? "Windows" : "Linux"
    readonly property string exeName: windows ? "orbit.exe" : "orbit"

    readonly property color canvas: dark ? "#09090c" : "#eceef1"
    readonly property color background: dark ? "#0e0e12" : "#f6f7f9"
    readonly property color sidebar: dark ? "#0a0a0d" : "#eceef1"
    readonly property color sidebarBottom: sidebar
    readonly property color topBar: dark ? "#0e0e12" : "#f6f7f9"
    readonly property color surface: dark ? "#15151a" : "#ffffff"
    readonly property color surfaceElevated: dark ? "#19191f" : "#ffffff"
    readonly property color surfaceHover: dark ? "#1c1c23" : "#f0f1f4"
    readonly property color surfacePressed: dark ? "#22222a" : "#e6e8ec"
    readonly property color surfaceInset: dark ? "#0b0b0e" : "#eef0f3"

    readonly property color panel: dark ? "#121217" : "#fbfbfc"
    readonly property color headerStrip: dark ? "#111116" : "#f1f2f5"
    readonly property color rowHover: dark ? "#16161c" : "#f3f4f7"
    readonly property color rowSelected: dark ? "#181a26" : "#e9ecfb"
    readonly property color statusBar: dark ? "#0a0a0d" : "#eceef1"

    readonly property color border: dark ? "#212128" : "#e1e3e8"
    readonly property color borderSoft: dark ? "#19191f" : "#eceef1"
    readonly property color borderStrong: dark ? "#2e2e37" : "#d0d3d9"
    readonly property color glassStroke: dark ? "#ffffff" : "#0a0a12"
    readonly property real glassStrokeAlpha: dark ? 0.045 : 0.05
    readonly property color highlight: dark ? Qt.rgba(1, 1, 1, 0.035) : Qt.rgba(1, 1, 1, 0.9)

    readonly property color textPrimary: dark ? "#ececf0" : "#16161a"
    readonly property color textSecondary: dark ? "#9b9ba6" : "#5d5d68"
    readonly property color textTertiary: dark ? "#6d6d78" : "#8e8e98"
    readonly property color textDisabled: dark ? "#4a4a53" : "#b3b3ba"

    // Single, controlled accent. No two-stop rainbow gradients on interactive chrome.
    readonly property color accent: "#5b6ef2"
    readonly property color accentHover: "#6c7ef5"
    readonly property color accentPressed: "#4c5edb"
    readonly property color accentInk: "#ffffff"
    readonly property color accent2: "#a179e8"
    readonly property color success: "#3fbf83"
    readonly property color warning: "#e0a63f"
    readonly property color danger: "#e5677a"
    readonly property color seeding: "#a179e8"
    readonly property color shadow: "#000000"

    readonly property color kindArchive: "#d19a44"
    readonly property color kindVideo: "#a47be6"
    readonly property color kindAudio: "#d9739b"
    readonly property color kindDocument: "#5d9bea"
    readonly property color kindImage: "#3fb3a2"
    readonly property color kindProgram: "#6f7ff0"
    readonly property color kindTorrent: "#48a8c8"
    readonly property color kindGeneric: dark ? "#8a8a96" : "#7a7a86"

    readonly property string monoFamily: "Cascadia Mono, Consolas, monospace"

    readonly property real radiusTiny: 4
    readonly property real radiusSmall: 6
    readonly property real radiusMedium: 8
    readonly property real radiusLarge: 10
    readonly property real radiusXl: 13
    readonly property real radiusPill: 999

    readonly property real spaceXxs: 2
    readonly property real spaceXs: 4
    readonly property real spaceSm: 8
    readonly property real spaceMd: 12
    readonly property real spaceLg: 16
    readonly property real spaceXl: 24
    readonly property real spaceXxl: 32

    readonly property real headerHeight: 56
    readonly property real sidebarWidth: 220
    readonly property real columnHeaderHeight: 30
    readonly property real rowHeight: 60
    readonly property real statusBarHeight: 28

    readonly property int fontTitle: 18
    readonly property int fontHeading: 14
    readonly property real fontBody: 12.5
    readonly property real fontCaption: 11.5
    readonly property real fontMicro: 10.5

    readonly property real trackingTight: -0.15
    readonly property real trackingNormal: 0
    readonly property real trackingWide: 0.2
    readonly property real trackingWider: 0.9

    readonly property int animInstant: 80
    readonly property int animFast: 120
    readonly property int animNormal: 180
    readonly property int animSlow: 260
    readonly property int animSlower: 420

    readonly property int easeStandard: Easing.OutCubic
    readonly property int easeEmphasized: Easing.OutQuint
    readonly property int easeSpring: Easing.OutBack
    readonly property int easeEnter: Easing.OutExpo

    function withAlpha(baseColor, alpha) {
        return Qt.rgba(baseColor.r, baseColor.g, baseColor.b, alpha)
    }

    function stateColor(stateLabel) {
        switch (stateLabel) {
        case "Downloading": return accent
        case "Connecting": return warning
        case "Seeding": return seeding
        case "Completed": return success
        case "Error": return danger
        case "Paused": return textSecondary
        case "Queued": return textTertiary
        case "Cancelled": return textDisabled
        default: return textSecondary
        }
    }

    function formatBytes(bytes) {
        const units = ["B", "KB", "MB", "GB", "TB"]
        let value = Math.max(0, bytes)
        let i = 0
        while (value >= 1024 && i < units.length - 1) { value /= 1024; i++ }
        return (i === 0 ? value.toFixed(0) : value.toFixed(value >= 100 ? 0 : (value >= 10 ? 1 : 2))) + " " + units[i]
    }

    function formatRate(bytesPerSec) {
        return formatBytes(bytesPerSec) + "/s"
    }

    function percentText(progress) {
        const v = Math.max(0, Math.min(1, progress)) * 100
        if (v > 0 && v < 10) return (Math.floor(v * 10) / 10).toFixed(1) + "%"
        return Math.floor(v) + "%"
    }

    function fileExtension(fileName) {
        const dot = fileName.lastIndexOf(".")
        if (dot <= 0 || dot >= fileName.length - 1) return ""
        const ext = fileName.substring(dot + 1)
        return ext.length <= 4 && /^[A-Za-z0-9]+$/.test(ext) ? ext.toLowerCase() : ""
    }

    function fileKind(fileName, isTorrent, isMedia) {
        if (isTorrent) return "torrent"
        const ext = fileExtension(fileName)
        if (isMedia) return ["mp3", "m4a", "opus", "ogg", "wav", "flac", "aac"].indexOf(ext) >= 0 ? "audio" : "video"
        if (["zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst", "cab"].indexOf(ext) >= 0) return "archive"
        if (["mp4", "mkv", "webm", "avi", "mov", "wmv", "flv", "m4v", "ts"].indexOf(ext) >= 0) return "video"
        if (["mp3", "m4a", "flac", "wav", "ogg", "opus", "aac", "wma"].indexOf(ext) >= 0) return "audio"
        if (["pdf", "doc", "docx", "txt", "md", "epub", "xls", "xlsx", "ppt", "pptx", "csv", "rtf", "odt"].indexOf(ext) >= 0) return "document"
        if (["png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "tiff", "heic", "psd"].indexOf(ext) >= 0) return "image"
        if (["exe", "msi", "msix", "dmg", "pkg", "deb", "rpm", "apk", "appimage", "iso", "jar"].indexOf(ext) >= 0) return "program"
        return "file"
    }

    function kindColor(kind) {
        switch (kind) {
        case "archive": return kindArchive
        case "video": return kindVideo
        case "audio": return kindAudio
        case "document": return kindDocument
        case "image": return kindImage
        case "program": return kindProgram
        case "torrent": return kindTorrent
        default: return kindGeneric
        }
    }

    function kindLabel(kind) {
        switch (kind) {
        case "archive": return "Archive"
        case "video": return "Video"
        case "audio": return "Audio"
        case "document": return "Document"
        case "image": return "Image"
        case "program": return "Program"
        case "torrent": return "Torrent"
        default: return "File"
        }
    }

    function sourceHost(source, isTorrent) {
        if (!source || source.length === 0) return ""
        if (source.indexOf("magnet:") === 0) return "Magnet link"
        const m = /^[a-z][a-z0-9+.-]*:\/\/(?:[^@\/]*@)?([^\/:?#]+)/i.exec(source)
        if (m) return m[1].replace(/^www\./i, "")
        if (isTorrent) return "Torrent file"
        return "Local file"
    }
}
