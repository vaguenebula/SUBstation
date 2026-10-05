#pragma once

// The Sampler's waveform (sampler.py's SampleView): its sample's waveform from
// its peaks, what plays of it (Start to End; the rest dimmed, the loop
// bracketed when it loops) and where the newest note is (the display
// "position"). Drag the Start or End marker (within kMarkerGrab px) to set it
// (one undo step per drag, kept within the other marker); drop an audio file
// on it to load it, or double-click it to browse (browseRequested: the editor
// shows the file dialog). Loading a sample is an undoable state change
// (ProjectEditor::setDeviceState); the path lives in the device's state
// (DeviceState.h, "sample"). The waveform is the bridge's (decoded already for
// the sampler, so it is quick; requested if it isn't).

#include "devices/DeviceCanvas.h"

#include "audio/Waveform.h"

#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

// The waveform's lowest and highest value under each of `width` columns (all
// channels together), from the source's peaks (sampler.py's waveform_columns).
std::pair<std::vector<float>, std::vector<float>> waveformColumns(const sub::app::Waveform& waveform, int width);

class SampleView : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString samplePath READ samplePath NOTIFY sampleChanged)
    Q_PROPERTY(QUrl sampleFolder READ sampleFolder NOTIFY sampleChanged)  // where browsing starts
    Q_PROPERTY(bool decoded READ decoded NOTIFY sampleChanged)  // its waveform is there
    Q_PROPERTY(QString loadError READ loadError NOTIFY sampleChanged)  // why it couldn't be loaded ("": it could)
    Q_PROPERTY(double playhead READ playhead NOTIFY playheadChanged)  // 0..1 of the sample; < 0: no note plays
    Q_PROPERTY(QString marker READ marker NOTIFY markerChanged)  // the one dragged: "start", "end" or ""

public:
    static constexpr int kWidth = 240;
    static constexpr int kMinimumHeight = 60;
    static constexpr double kMarkerGrab = 5.0;  // px either side of a marker that grab it
    static inline const QString kFileFilter = QStringLiteral("Audio Files (*.wav *.wave *.flac *.mp3)");

    explicit SampleView(QQuickItem* parent = nullptr);

    QString samplePath() const;
    QUrl sampleFolder() const;
    bool decoded() const { return !waveform_.isNull(); }
    QString loadError() const { return error_; }
    double playhead() const { return playhead_; }
    QString marker() const { return drag_; }
    void setPlayhead(double where);

    // Play `path` ("": no sample), undoably ("Load Sample" / "Clear Sample").
    Q_INVOKABLE void loadSample(const QString& path);
    Q_INVOKABLE void loadSampleUrl(const QUrl& url);
    Q_INVOKABLE void clearSample() { loadSample(QString()); }

    QRectF plot() const;
    // A Start or End (percent) as x.
    double xOf(double percent) const;
    // The marker within reach of x ("start" or "end"; "" none).
    QString markerAt(double x) const;

Q_SIGNALS:
    void sampleChanged();
    void playheadChanged();
    void markerChanged();
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
    static QString droppedFile(const QDropEvent* event);
    void setDrag(const QString& marker);

    QString path_;
    sub::app::Waveform waveform_;
    QString error_;
    std::vector<float> top_;     // each column's waveform, for paint()
    std::vector<float> bottom_;
    QString columnsKey_;
    double start_ = 0.0;  // percent
    double end_ = 100.0;
    bool looping_ = false;
    double playhead_ = -1.0;
    QString drag_;
    QString gesture_;
    bool connectedSources_ = false;
};

}  // namespace sub::ui
