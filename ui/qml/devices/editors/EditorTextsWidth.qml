import QtQuick
import SUBstation

// How wide one of an editor's Texts (a caption, a readout, a button's or a list's name) must be to show any of
// `texts` whole in `font` (EditorCaption's and EditorReadout's by default): `needed`, in whole pixels. They are laid
// out as that Text lays them out, in this one (hidden, a line each): FontMetrics can be a pixel off it, where the
// font is hinted or kerned ("Medium Curve" in Inter). A "#" in a text is any figure, measured as the font's widest
// (figures are as wide as each other in some fonts, not in others). The texts of `knobs` (EditorKnobs: their
// captions and every form their readouts take, `texts()`) are measured too.
//
//   EditorTextsWidth { id: levelTexts; knobs: [outputKnob, mixKnob] }
//   readonly property int levelsCell: Math.max(62, levelTexts.needed)
Text {
    id: measured

    property var texts: []
    property var knobs: []
    readonly property int needed: Math.ceil(implicitWidth)
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

    visible: false
    textFormat: Text.PlainText
    font: Theme.uiFont(8)
    text: texts.concat(...knobs.map(knob => knob.texts())).map(text => text.replace(/#/g, widestFigure)).join("\n")

    FontMetrics {
        id: metrics
        font: measured.font
    }
}
