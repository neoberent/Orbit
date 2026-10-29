import QtQuick

Canvas {
    id: root

    property var cells: []
    property color fillColor: Theme.accent
    property color emptyColor: Theme.withAlpha(Theme.textPrimary, 0.06)

    implicitHeight: 14
    onCellsChanged: requestPaint()
    onWidthChanged: requestPaint()
    onFillColorChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.fillStyle = root.emptyColor
        ctx.fillRect(0, 0, width, height)
        const n = root.cells.length
        if (n === 0) return
        const cellWidth = width / n
        for (let i = 0; i < n; ++i) {
            const v = root.cells[i]
            if (v <= 0) continue
            ctx.globalAlpha = 0.25 + 0.75 * (v / 255)
            ctx.fillStyle = root.fillColor
            ctx.fillRect(Math.floor(i * cellWidth), 0, Math.ceil(cellWidth), height)
        }
        ctx.globalAlpha = 1
    }
}
