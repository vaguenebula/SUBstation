#pragma once

// The themes (Options › Preferences… › Look and Feel): a palette each, every
// colour Theme hands out. Themes are colours only: the fonts, the metrics and
// the layout are the same in all of them.

#include <QColor>
#include <QString>

#include <vector>

namespace sub::ui {

struct Palette {
    // Base colours
    QColor window, panel, panelAlt, surface, surfaceHover, border;
    QColor text, textDim, textDisabled, accent, accentText;
    // Arrangement
    QColor lane, laneSelected, emptyArea, gridBar, gridBeat, gridSub;
    QColor playhead, insertMarker, loopOn, loopOff, loopRegion;
    QColor selectionOutline;
    QColor selection;  // a time selection on the grid (selected clips are one), and automation ranges
    QColor rubberBand;
    QColor waveform;  // also MIDI notes drawn in clips
    // A group's lane while it is open: the faint outline of its tracks' clips,
    // a row each (ArrangementLanes::drawGroupSummary; folded, they are in their colours).
    QColor groupOutline;
    // A deactivated clip (0): grey whatever its track's colour (its title bar this,
    // its body the darker grey drawn from it, as a clip's is from its colour), its
    // waveform or notes faded.
    QColor deactivatedClip, deactivatedContent;
    // Piano roll
    QColor keyWhite, keyBlack, keyLabel, blackKeyRow;
    QColor outsideClip;  // content a clip has but doesn't play
    QColor outOfKey;     // notes out of the song's key are tinted halfway to it
    // Where the piano roll pastes notes (dashed), apart from the start marker
    // (insertMarker) and the playhead.
    QColor pasteMarker;
    // A track header's volume and pan boxes: the slider's fill under the value.
    QColor volumeFill;
    // Controls
    QColor activatorOn, soloOn, playOn, recordOn;
    QColor meterLow, meterMid, meterHigh, meterBg;
    QColor deviceHeader;          // a device's title bar
    QColor deviceHeaderSelected;  // a selected device's title bar
    QColor deviceHeaderHover;     // a device header button under the mouse
    QColor scopeLine, scopeGlow, scopeAxis;
    QColor frozen;      // a frozen track's snowflake
    QColor frozenTint;  // over a frozen track's lane
    QColor knob, knobTrack;  // a knob's value arc, and the rest of its range
    QColor scrollHandle, scrollHandleHover;
    QColor automationOn;   // the dot of an automated control
    QColor automationOff;  // the dot of a control whose automation is overridden
};

struct NamedPalette {
    QString name;  // what Look and Feel lists, and what the settings keep
    Palette colors;
};

// Every theme, Default first.
const std::vector<NamedPalette>& palettes();

}  // namespace sub::ui
