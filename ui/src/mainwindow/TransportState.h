#pragma once

// What the transport bar shows that needs working out: the position as
// "bar. beat. sixteenth", changed only when its text does; the time
// signature's two numbers and the denominators it may have; the project key's
// choices; the CPU load, every 15th meter update (about twice a second); and
// the audio device's name and rate, with a tooltip. The bar's controls call
// the session's editor and bridge themselves.

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class TransportState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    // "  1. 1. 1": the bar right-aligned in three places, one-based.
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionTextChanged)
    Q_PROPERTY(int numerator READ numerator NOTIFY settingsChanged)
    Q_PROPERTY(int denominator READ denominator NOTIFY settingsChanged)
    // The denominators a time signature may have (the value box's choices).
    Q_PROPERTY(QList<qreal> denominators READ denominators CONSTANT)
    // The key chooser's entries: [{label, name}], "No Key" (name "") first,
    // then every key ("C Major", "C Minor", ...; name as projects save it).
    Q_PROPERTY(QVariantList keys READ keys CONSTANT)
    // The project's key among them (0: none).
    Q_PROPERTY(int keyIndex READ keyIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString cpuText READ cpuText NOTIFY cpuTextChanged)  // "CPU 12%"
    // "Speakers · 48 kHz" (a long name cut to 28 characters), or "No audio device".
    Q_PROPERTY(QString deviceText READ deviceText NOTIFY deviceChanged)
    Q_PROPERTY(QString deviceToolTip READ deviceToolTip NOTIFY deviceChanged)

public:
    static constexpr int kCpuEvery = 15;      // meter updates per CPU reading shown
    static constexpr int kDeviceNameMax = 28;  // characters of the device's name shown

    explicit TransportState(QObject* parent = nullptr);

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);

    QString positionText() const { return positionText_; }
    int numerator() const;
    int denominator() const;
    QList<qreal> denominators() const;
    QVariantList keys() const;
    int keyIndex() const;
    QString cpuText() const { return cpuText_; }
    QString deviceText() const { return deviceText_; }
    QString deviceToolTip() const { return deviceToolTip_; }

    // The position's text for a beat in a time signature.
    static QString formatPosition(double beat, int numerator, int denominator);

Q_SIGNALS:
    void sessionChanged();
    void positionTextChanged();
    void settingsChanged();
    void cpuTextChanged();
    void deviceChanged();

private:
    void showPosition(double beat);
    void meterTick();
    void refreshDevice();
    void refresh();

    QPointer<app::Session> session_;
    QString positionText_;
    QString cpuText_ = QStringLiteral("CPU 0%");
    QString deviceText_;
    QString deviceToolTip_;
    int cpuTicks_ = 0;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
