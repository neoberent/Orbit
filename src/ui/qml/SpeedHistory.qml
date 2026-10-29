import QtQuick

Item {
    id: root
    visible: false

    property int capacity: 120
    property int revision: 0
    property var series: ({})
    property var total: []

    signal sampled()

    function push(list, value) {
        const next = list.slice()
        next.push(value)
        if (next.length > capacity) next.shift()
        return next
    }

    function sample() {
        const rates = downloadModel.rates()
        const next = {}
        const ids = new Set(Object.keys(series).concat(Object.keys(rates)))
        for (const id of ids) {
            const rate = rates[id] !== undefined ? rates[id] : 0
            const history = push(series[id] || [], rate)
            if (rates[id] === undefined && history.every(v => v === 0)) continue
            next[id] = history
        }
        series = next
        total = push(total, downloadModel.summary.downloadRate || 0)
        revision++
        sampled()
    }

    function forId(id) {
        return series[String(id)] || []
    }

    function combined(ids) {
        const out = []
        for (const id of ids) {
            const s = series[String(id)]
            if (!s) continue
            const offset = capacity - s.length
            for (let i = 0; i < s.length; ++i) out[offset + i] = (out[offset + i] || 0) + s[i]
        }
        const firstDefined = out.findIndex(v => v !== undefined)
        if (firstDefined < 0) return []
        return out.slice(firstDefined).map(v => v || 0)
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.sample()
    }
}
