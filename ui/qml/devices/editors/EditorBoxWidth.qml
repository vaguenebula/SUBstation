import QtQuick
import SUBstation

// How wide a value box (a ParamBox) must be to show any of its texts whole with its automation dot clear of them:
// `needed`, in whole pixels. A box draws its text centred by its advance (as FontMetrics measures it), from a whole
// pixel, and the dot from 3.5 to 8.5 px from its left: with 9.5 px at either side of the widest text the text starts
// 10 px in at the least, a pixel clear of the dot (and further in where a glyph's ink starts left of its advance).
// Its texts: `texts`, and every form the texts of `params` (DeviceParams) take, their textForms(); a "#" in them is
// any figure, measured as the font's widest. `sample` is the widest of them, as the font has it (a ParamBox's
// sampleText). An EditorTextsWidth measures the texts of Texts (captions, readouts, names).
//
//   EditorBoxWidth { id: gainBoxes; params: [p.get("in"), p.get("out")] }
//   ParamBox { width: gainBoxes.needed; sampleText: gainBoxes.sample; ... }
FontMetrics {
    id: metrics

    property var texts: []
    property var params: []
    // The room at either side of the widest text, at the least.
    readonly property real margin: 9.5
    // The font's widest figure, by their advances.
    readonly property string widestFigure: {
        void metrics.font  // (worked out again once the font is set)
        let widest = "0"
        for (const figure of "123456789") {
            if (metrics.advanceWidth(figure) > metrics.advanceWidth(widest))
                widest = figure
        }
        return widest
    }
    readonly property string sample: {
        void metrics.font
        const forms = texts.concat(...params.map(param => param && param.valid ? param.textForms() : []))
        let widest = ""
        for (const form of forms) {
            const text = form.replace(/#/g, widestFigure)
            if (metrics.advanceWidth(text) > metrics.advanceWidth(widest))
                widest = text
        }
        return widest
    }
    readonly property int needed: {
        void metrics.font
        const inkBefore = Math.max(0, -metrics.boundingRect(sample).x)  // (a glyph reaching left of its advance)
        return Math.ceil(metrics.advanceWidth(sample) + 2 * (inkBefore + margin))
    }

    font: Theme.uiFont(8)  // (a ParamBox's)
}
