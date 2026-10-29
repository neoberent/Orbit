import QtQuick

Item {
    id: root
    property string name: ""
    property color color: "#ffffff"
    property real iconSize: 18

    implicitWidth: iconSize
    implicitHeight: iconSize

    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.FramebufferObject

        function roundedRect(ctx, x, y, w, h, r) {
            ctx.beginPath()
            ctx.moveTo(x + r, y)
            ctx.arcTo(x + w, y, x + w, y + h, r)
            ctx.arcTo(x + w, y + h, x, y + h, r)
            ctx.arcTo(x, y + h, x, y, r)
            ctx.arcTo(x, y, x + w, y, r)
            ctx.closePath()
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var w = width
            var h = height
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.lineWidth = Math.max(1.4, w * 0.09)
            ctx.strokeStyle = root.color
            ctx.fillStyle = root.color

            switch (root.name) {
            case "all":
                var gs = w * 0.32, gap = w * 0.14
                roundedRect(ctx, w * 0.5 - gap / 2 - gs, h * 0.5 - gap / 2 - gs, gs, gs, gs * 0.28); ctx.fill()
                roundedRect(ctx, w * 0.5 + gap / 2, h * 0.5 - gap / 2 - gs, gs, gs, gs * 0.28); ctx.fill()
                roundedRect(ctx, w * 0.5 - gap / 2 - gs, h * 0.5 + gap / 2, gs, gs, gs * 0.28); ctx.fill()
                roundedRect(ctx, w * 0.5 + gap / 2, h * 0.5 + gap / 2, gs, gs, gs * 0.28); ctx.fill()
                break
            case "downloading":
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.12); ctx.lineTo(w * 0.5, h * 0.58); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.28, h * 0.42); ctx.lineTo(w * 0.5, h * 0.64); ctx.lineTo(w * 0.72, h * 0.42); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.22, h * 0.82); ctx.lineTo(w * 0.78, h * 0.82); ctx.stroke()
                break
            case "completed":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.38, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.32, h * 0.52); ctx.lineTo(w * 0.45, h * 0.66); ctx.lineTo(w * 0.7, h * 0.36); ctx.stroke()
                break
            case "torrent":
                ctx.beginPath(); ctx.moveTo(w * 0.18, h * 0.36); ctx.lineTo(w * 0.7, h * 0.36); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.56, h * 0.22); ctx.lineTo(w * 0.78, h * 0.36); ctx.lineTo(w * 0.56, h * 0.5); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.82, h * 0.64); ctx.lineTo(w * 0.3, h * 0.64); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.44, h * 0.5); ctx.lineTo(w * 0.22, h * 0.64); ctx.lineTo(w * 0.44, h * 0.78); ctx.stroke()
                break
            case "settings":
                ctx.save()
                ctx.translate(w * 0.5, h * 0.5)
                var teeth = 8
                for (var i = 0; i < teeth; i++) {
                    ctx.save()
                    ctx.rotate(i * Math.PI * 2 / teeth)
                    ctx.fillRect(-w * 0.07, -h * 0.48, w * 0.14, h * 0.18)
                    ctx.restore()
                }
                ctx.beginPath(); ctx.arc(0, 0, w * 0.34, 0, Math.PI * 2); ctx.fill()
                ctx.restore()
                ctx.save()
                ctx.globalCompositeOperation = "destination-out"
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.14, 0, Math.PI * 2); ctx.fill()
                ctx.restore()
                break
            case "search":
                ctx.beginPath(); ctx.arc(w * 0.42, h * 0.42, w * 0.26, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.61, h * 0.61); ctx.lineTo(w * 0.82, h * 0.82); ctx.stroke()
                break
            case "plus":
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.2); ctx.lineTo(w * 0.5, h * 0.8); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.2, h * 0.5); ctx.lineTo(w * 0.8, h * 0.5); ctx.stroke()
                break
            case "minus":
                ctx.beginPath(); ctx.moveTo(w * 0.2, h * 0.5); ctx.lineTo(w * 0.8, h * 0.5); ctx.stroke()
                break
            case "play":
                ctx.beginPath(); ctx.moveTo(w * 0.32, h * 0.2); ctx.lineTo(w * 0.32, h * 0.8); ctx.lineTo(w * 0.8, h * 0.5); ctx.closePath(); ctx.fill()
                break
            case "pause":
                ctx.fillRect(w * 0.28, h * 0.2, w * 0.16, h * 0.6)
                ctx.fillRect(w * 0.56, h * 0.2, w * 0.16, h * 0.6)
                break
            case "close":
                ctx.beginPath(); ctx.moveTo(w * 0.26, h * 0.26); ctx.lineTo(w * 0.74, h * 0.74); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.74, h * 0.26); ctx.lineTo(w * 0.26, h * 0.74); ctx.stroke()
                break
            case "folder":
                roundedRect(ctx, w * 0.16, h * 0.32, w * 0.68, h * 0.42, w * 0.06); ctx.fill()
                roundedRect(ctx, w * 0.16, h * 0.22, w * 0.32, h * 0.16, w * 0.05); ctx.fill()
                break
            case "trash":
                roundedRect(ctx, w * 0.28, h * 0.32, w * 0.44, h * 0.5, w * 0.05); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.22, h * 0.32); ctx.lineTo(w * 0.78, h * 0.32); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.4, h * 0.22); ctx.lineTo(w * 0.6, h * 0.22); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.4, h * 0.44); ctx.lineTo(w * 0.4, h * 0.72); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.6, h * 0.44); ctx.lineTo(w * 0.6, h * 0.72); ctx.stroke()
                break
            case "chevronDown":
                ctx.beginPath(); ctx.moveTo(w * 0.25, h * 0.38); ctx.lineTo(w * 0.5, h * 0.62); ctx.lineTo(w * 0.75, h * 0.38); ctx.stroke()
                break
            case "orbitRing":
                ctx.beginPath(); ctx.ellipse(w * 0.5, h * 0.5, w * 0.36, h * 0.22, 0, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.1, 0, Math.PI * 2); ctx.fill()
                ctx.beginPath(); ctx.arc(w * 0.86, h * 0.5, w * 0.055, 0, Math.PI * 2); ctx.fill()
                break
            case "link":
                ctx.beginPath(); ctx.arc(w * 0.36, h * 0.64, w * 0.16, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.64, h * 0.36, w * 0.16, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.44, h * 0.56); ctx.lineTo(w * 0.56, h * 0.44); ctx.stroke()
                break
            case "magnet":
                ctx.beginPath()
                ctx.moveTo(w * 0.28, h * 0.22)
                ctx.lineTo(w * 0.28, h * 0.5)
                ctx.arc(w * 0.5, h * 0.5, w * 0.22, Math.PI, 0, false)
                ctx.lineTo(w * 0.72, h * 0.22)
                ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.22, h * 0.22); ctx.lineTo(w * 0.35, h * 0.22); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.65, h * 0.22); ctx.lineTo(w * 0.78, h * 0.22); ctx.stroke()
                break
            case "check":
                ctx.beginPath(); ctx.moveTo(w * 0.22, h * 0.52); ctx.lineTo(w * 0.42, h * 0.71); ctx.lineTo(w * 0.8, h * 0.3); ctx.stroke()
                break
            case "globe":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.36, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.ellipse(w * 0.5, h * 0.5, w * 0.16, h * 0.36, 0, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.14, h * 0.5); ctx.lineTo(w * 0.86, h * 0.5); ctx.stroke()
                break
            case "copy":
                roundedRect(ctx, w * 0.36, h * 0.16, w * 0.48, h * 0.48, w * 0.08); ctx.stroke()
                roundedRect(ctx, w * 0.16, h * 0.36, w * 0.48, h * 0.48, w * 0.08); ctx.stroke()
                break
            case "media":
                roundedRect(ctx, w * 0.14, h * 0.2, w * 0.72, h * 0.6, w * 0.08); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.42, h * 0.36); ctx.lineTo(w * 0.42, h * 0.64); ctx.lineTo(w * 0.64, h * 0.5); ctx.closePath(); ctx.fill()
                break
            case "arrowDown":
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.18); ctx.lineTo(w * 0.5, h * 0.8); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.26, h * 0.56); ctx.lineTo(w * 0.5, h * 0.8); ctx.lineTo(w * 0.74, h * 0.56); ctx.stroke()
                break
            case "arrowUp":
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.82); ctx.lineTo(w * 0.5, h * 0.2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.26, h * 0.44); ctx.lineTo(w * 0.5, h * 0.2); ctx.lineTo(w * 0.74, h * 0.44); ctx.stroke()
                break
            case "retry":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.52, w * 0.3, -Math.PI * 0.1, -Math.PI * 0.5, false); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.42, h * 0.1); ctx.lineTo(w * 0.54, h * 0.22); ctx.lineTo(w * 0.42, h * 0.34); ctx.stroke()
                break
            case "alert":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.38, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.3); ctx.lineTo(w * 0.5, h * 0.54); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.68, w * 0.045, 0, Math.PI * 2); ctx.fill()
                break
            case "clock":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.38, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.28); ctx.lineTo(w * 0.5, h * 0.5); ctx.lineTo(w * 0.66, h * 0.6); ctx.stroke()
                break
            case "sweep":
                ctx.beginPath(); ctx.moveTo(w * 0.18, h * 0.3); ctx.lineTo(w * 0.62, h * 0.3); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.18, h * 0.5); ctx.lineTo(w * 0.5, h * 0.5); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.18, h * 0.7); ctx.lineTo(w * 0.4, h * 0.7); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.56, h * 0.6); ctx.lineTo(w * 0.82, h * 0.86); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.82, h * 0.6); ctx.lineTo(w * 0.56, h * 0.86); ctx.stroke()
                break
            case "sort":
                ctx.beginPath(); ctx.moveTo(w * 0.16, h * 0.28); ctx.lineTo(w * 0.84, h * 0.28); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.16, h * 0.5); ctx.lineTo(w * 0.64, h * 0.5); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.16, h * 0.72); ctx.lineTo(w * 0.44, h * 0.72); ctx.stroke()
                break
            case "open":
                roundedRect(ctx, w * 0.16, h * 0.24, w * 0.6, h * 0.6, w * 0.08); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.5); ctx.lineTo(w * 0.86, h * 0.14); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.6, h * 0.14); ctx.lineTo(w * 0.86, h * 0.14); ctx.lineTo(w * 0.86, h * 0.4); ctx.stroke()
                break
            case "info":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.5, w * 0.38, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.46); ctx.lineTo(w * 0.5, h * 0.7); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.32, w * 0.045, 0, Math.PI * 2); ctx.fill()
                break
            case "edit":
                ctx.beginPath(); ctx.moveTo(w * 0.2, h * 0.8); ctx.lineTo(w * 0.24, h * 0.62); ctx.lineTo(w * 0.66, h * 0.2)
                ctx.lineTo(w * 0.8, h * 0.34); ctx.lineTo(w * 0.38, h * 0.76); ctx.closePath(); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.56, h * 0.3); ctx.lineTo(w * 0.7, h * 0.44); ctx.stroke()
                break
            case "power":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.54, w * 0.32, -Math.PI * 0.3, Math.PI * 1.3); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.12); ctx.lineTo(w * 0.5, h * 0.46); ctx.stroke()
                break
            case "shield":
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.12); ctx.lineTo(w * 0.8, h * 0.24); ctx.lineTo(w * 0.8, h * 0.48)
                ctx.quadraticCurveTo(w * 0.78, h * 0.76, w * 0.5, h * 0.9)
                ctx.quadraticCurveTo(w * 0.22, h * 0.76, w * 0.2, h * 0.48); ctx.lineTo(w * 0.2, h * 0.24); ctx.closePath(); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.36, h * 0.5); ctx.lineTo(w * 0.46, h * 0.6); ctx.lineTo(w * 0.64, h * 0.4); ctx.stroke()
                break
            case "network":
                ctx.beginPath(); ctx.arc(w * 0.5, h * 0.22, w * 0.1, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.22, h * 0.78, w * 0.1, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.78, h * 0.78, w * 0.1, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.32); ctx.lineTo(w * 0.5, h * 0.52); ctx.lineTo(w * 0.24, h * 0.68); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.52); ctx.lineTo(w * 0.76, h * 0.68); ctx.stroke()
                break
            case "clipboard":
                roundedRect(ctx, w * 0.22, h * 0.18, w * 0.56, h * 0.7, w * 0.08); ctx.stroke()
                roundedRect(ctx, w * 0.36, h * 0.1, w * 0.28, h * 0.16, w * 0.05); ctx.fill()
                ctx.beginPath(); ctx.moveTo(w * 0.34, h * 0.48); ctx.lineTo(w * 0.66, h * 0.48); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.34, h * 0.66); ctx.lineTo(w * 0.56, h * 0.66); ctx.stroke()
                break
            case "extension":
                ctx.beginPath()
                ctx.moveTo(w * 0.18, h * 0.32); ctx.lineTo(w * 0.4, h * 0.32)
                ctx.arc(w * 0.5, h * 0.26, w * 0.1, Math.PI * 0.8, Math.PI * 0.2, false)
                ctx.lineTo(w * 0.82, h * 0.32); ctx.lineTo(w * 0.82, h * 0.82); ctx.lineTo(w * 0.18, h * 0.82); ctx.closePath(); ctx.stroke()
                break
            case "sliders":
                ctx.beginPath(); ctx.moveTo(w * 0.16, h * 0.3); ctx.lineTo(w * 0.84, h * 0.3); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.16, h * 0.7); ctx.lineTo(w * 0.84, h * 0.7); ctx.stroke()
                ctx.beginPath(); ctx.arc(w * 0.36, h * 0.3, w * 0.09, 0, Math.PI * 2); ctx.fill()
                ctx.beginPath(); ctx.arc(w * 0.64, h * 0.7, w * 0.09, 0, Math.PI * 2); ctx.fill()
                break
            case "save":
                roundedRect(ctx, w * 0.18, h * 0.18, w * 0.64, h * 0.64, w * 0.08); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.34, h * 0.18); ctx.lineTo(w * 0.34, h * 0.38); ctx.lineTo(w * 0.62, h * 0.38); ctx.lineTo(w * 0.62, h * 0.18); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(w * 0.34, h * 0.64); ctx.lineTo(w * 0.66, h * 0.64); ctx.stroke()
                break
            case "sparkle":
                ctx.beginPath()
                ctx.moveTo(w * 0.5, h * 0.12)
                ctx.quadraticCurveTo(w * 0.55, h * 0.45, w * 0.88, h * 0.5)
                ctx.quadraticCurveTo(w * 0.55, h * 0.55, w * 0.5, h * 0.88)
                ctx.quadraticCurveTo(w * 0.45, h * 0.55, w * 0.12, h * 0.5)
                ctx.quadraticCurveTo(w * 0.45, h * 0.45, w * 0.5, h * 0.12)
                ctx.closePath()
                ctx.fill()
                break
            }
        }

        Component.onCompleted: requestPaint()
    }

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
