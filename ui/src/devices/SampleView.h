#pragma once

// The Sampler's waveform, as Simpler draws its sample: the waveform from its
// peaks (flipped while the sampler plays it reversed: what is drawn is what
// plays, left to right), what plays of it (Start to End, flagged; the rest
// dimmed), and by the sampler's mode: Classic's loop (from Loop Start, its
// crossfade shaded), 1-Shot's fades, Slice's slices (the one playing lit,
// numbered at the bottom while there is room), where they snap to with Snap on; the newest
// note's place (the display "position"); the sample's name in its top left
// corner, and a time ruler along the bottom.
//
// Drag a marker (within kMarkerGrab px: Start, End, and Classic's Loop Start
// while it loops, by its handle at the bottom where it is on another) to set it (one undo step per drag, kept within the others);
// drop an audio file on it to load it, or double-click it to browse
// (browseRequested: the editor shows the file dialog). Loading a sample is an
// undoable state change (ProjectEditor::setDeviceState); the path lives in the
// device's state (DeviceState.h, "sample"). The waveform is the bridge's
// (decoded already for the sampler, so it is quick; requested if it isn't);
// the slices are the engine's own (audio/SampleSlices.h).

#include "devices/DeviceCanvas.h"

#include "audio/SampleSlices.h"
#include "audio/Waveform.h"

#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <vector>

namespace sub::ui {

// The waveform's lowest and highest value under each of `width` columns (all
// channels together), from the source's peaks.
std::pair<std::vector<float>, std::vector<float>> waveformColumns(const sub::app::Waveform& waveform, int width);

class SampleView : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString samplePath READ samplePath NOTIFY sampleChanged)
    Q_PROPERTY(QUrl sampleFolder READ sampleFolder NOTIFY sampleChanged)  // where browsing starts
    Q_PROPERTY(bool decoded READ decoded NOTIFY sampleChanged)  // its waveform is there
    Q_PROPERTY(QString loadError READ loadError NOTIFY sampleChanged)  // why it couldn't be loaded ("": it could)
    Q_PROPERTY(double duration READ duration NOTIFY sampleChanged)  // seconds (0: none)
    Q_PROPERTY(double playhead READ playhead NOTIFY playheadChanged)  // 0..1 of the sample as it plays; < 0: none
    Q_PROPERTY(QString marker READ marker NOTIFY markerChanged)  // the one dragged: "start", "end", "loop" or ""
    Q_PROPERTY(int mode READ mode NOTIFY layoutChanged)  // the sampler's: 0 Classic, 1 1-Shot, 2 Slice
    // Slice: where its slices start (0..1 of the sample, as it plays), and the one playing (-1: none).
    Q_PROPERTY(QList<qreal> slices READ slices NOTIFY layoutChanged)
    Q_PROPERTY(int playingSlice READ playingSlice NOTIFY playheadChanged)

public:
    static constexpr int kWidth = 240;
    static constexpr int kMinimumHeight = 40;
    static constexpr double kMarkerGrab = 5.0;  // px either side of a marker that grab it
    static constexpr double kRulerHeight = 10.0;  // the time ruler under the waveform
    static constexpr double kLoopHandle = 10.0;   // the loop's marker grabbed first this far up from the bottom
    static inline const QString kFileFilter = QStringLiteral("Audio Files (*.wav *.wave *.flac *.mp3)");

    explicit SampleView(QQuickItem* parent = nullptr);

    QString samplePath() const;
    QUrl sampleFolder() const;
    bool decoded() const { return !waveform_.isNull(); }
    QString loadError() const { return error_; }
    double duration() const { return waveform_.isNull() ? 0.0 : waveform_.duration(); }
    double playhead() const { return playhead_; }
    QString marker() const { return drag_; }
    int mode() const { return mode_; }
    QList<qreal> slices() const;
    int playingSlice() const;
    void setPlayhead(double where);

    // Play `path` ("": no sample), undoably ("Load Sample" / "Clear Sample").
    Q_INVOKABLE void loadSample(const QString& path);
    Q_INVOKABLE void loadSampleUrl(const QUrl& url);
    Q_INVOKABLE void clearSample() { loadSample(QString()); }

    // Where the waveform is drawn.
    QRectF plot() const;
    // A Start or End (percent of the sample, as it plays) as x.
    double xOf(double percent) const;
    // The marker within reach of x ("start", "end", "loop"; "" none), the nearest (the first of
    // equals); at y in the loop's handle (the bottom kLoopHandle px), the loop's first.
    QString markerAt(double x, double y = -1.0) const;

Q_SIGNALS:
    void sampleChanged();
    void playheadChanged();
    void markerChanged();
    void layoutChanged();
    void browseRequested();

protected:
    void sync() override;
    void stateChanged() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void readSample();
    void updateColumns();
    void updateLayout();
    static QString droppedFile(const QDropEvent* event);
    void setDrag(const QString& marker);
    // A frame (as the sample plays) as x; a percent of it as a frame (snapped with Snap on), as the sampler has it.
    double xOfFrame(double frame) const;
    qint64 frameOf(double percent) const;
    void paintRuler(SgPainter& painter, const QRectF& area) const;
    void paintName(SgPainter& painter, const QRectF& plot, const QFont& font) const;

    QString path_;
    sub::app::Waveform waveform_;
    QString error_;
    std::vector<float> top_;     // each column's waveform, for paint()
    std::vector<float> bottom_;
    QString columnsKey_;
    // The parameters drawn.
    int mode_ = 0;
    double start_ = 0.0;  // percent
    double end_ = 100.0;
    bool looping_ = false;
    double loopStart_ = 0.0;
    double loopFade_ = 0.0;
    bool reverse_ = false;
    bool snap_ = false;
    double fadeIn_ = 0.0;  // ms
    double fadeOut_ = 0.0;
    bool warp_ = false;
    double rate_ = 1.0;  // the sample's frames a frame played (warped)
    sub::app::sampleSlices::Settings sliceSettings_;
    // Where they put things, in frames as the sample plays.
    qint64 startFrame_ = 0, endFrame_ = 0, loopFrame_ = 0, fadeFrames_ = 0;
    std::vector<qint64> slices_;
    // The sample's transients (forwards, reversed), found once it is decoded, when slicing by them.
    std::array<std::vector<sub::app::sampleSlices::Transient>, 2> transients_;
    std::array<bool, 2> transientsFound_{};
    double playhead_ = -1.0;
    QString drag_;
    QString gesture_;
    bool connectedSources_ = false;
};

}  // namespace sub::ui
