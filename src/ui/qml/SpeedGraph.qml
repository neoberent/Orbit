import QtQuick

Item {
    id: root

    property var samples: []
    property int capacity: 120
    property color lineColor: Theme.accent
    property bool showGrid: true
    property real lineWidth: 1.5
    property real fillAlpha: 0.14

    readonly property real peak: {
        let m = 0
        for (let i = 0; i < samples.length; ++i) m = Math.max(m, samples[i])
        return m
    }

    function niceCeiling(v) {
        if (v <= 0) return 1
        const exp = Math.pow(10, Math.floor(Math.log(v) / Math.LN10))
        const f = v / exp
        const nice = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10
        return nice * exp
    }

    readonly property real scaleMax: niceCeiling(peak * 1.1)

    onSamplesChanged: canvas.requestPaint()
    onLineColorChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Connections {
        target: Theme
        function onDarkChanged() { canvas.requestPaint() }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.FramebufferObject

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            if (w <= 0 || h <= 0) return

            if (root.showGrid) {
                ctx.strokeStyle = Theme.withAlpha(Theme.textPrimary, 0.06)
                ctx.lineWidth = 1
                for (let g = 1; g <= 3; ++g) {
                    const gy = Math.round(h * g / 4) + 0.5
                    ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(w, gy); ctx.stroke()
                }
            }

            const data = root.samples
            if (!data || data.length < 2) return

            const step = w / (root.capacity - 1)
            const offset = (root.capacity - data.length) * step
            const top = root.lineWidth
            const usable = h - top - 1
            const yOf = (v) => h - 1 - Math.min(1, v / root.scaleMax) * usable

            ctx.beginPath()
            ctx.moveTo(offset, h)
            for (let i = 0; i < data.length; ++i) ctx.lineTo(offset + i * step, yOf(data[i]))
            ctx.lineTo(offset + (data.length - 1) * step, h)
            ctx.closePath()
            ctx.fillStyle = Theme.withAlpha(root.lineColor, root.fillAlpha)
            ctx.fill()

            ctx.beginPath()
            for (let j = 0; j < data.length; ++j) {
                const x = offset + j * step
                const y = yOf(data[j])
                if (j === 0) ctx.moveTo(x, y)
                else ctx.lineTo(x, y)
            }
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            ctx.lineWidth = root.lineWidth
            ctx.strokeStyle = root.lineColor
            ctx.stroke()
        }
    }
}
