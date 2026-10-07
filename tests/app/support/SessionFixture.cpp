#include "SessionFixture.h"

#include "model/Clip.h"
#include "session/RenderProgress.h"

#include <QFileInfo>
#include <QObject>
#include <QTest>

namespace sub::app::test {

SessionFixture::SessionFixture(bool clipFades) {
    if (!clipFades) engine.setClipFadeMs(0);
    Session::Options options;
    options.scanPlugins = false;
    options.browserIndex = false;
    options.analyseSounds = false;  // (the browser lists the user's Music folder)
    session = std::make_unique<Session>(engine, options);
    QObject::connect(session.get(), &Session::statusMessage, [this](const QString& m) { messages.append(m); });
    QObject::connect(session.get(), &Session::warning, [this](const QString& m) { warnings.append(m); });
    QObject::connect(session.get(), &Session::information, [this](const QString& m) { informations.append(m); });
}

SessionFixture::~SessionFixture() {
    session->shutdown();
    session.reset();
    engine.closeDevice();
}

bool SessionFixture::waitForRender(int timeoutMs) const {
    return QTest::qWaitFor([this] { return !session->render()->active(); }, timeoutMs);
}

bool SessionFixture::waitForSource(const QString& path, int timeoutMs) const {
    session->bridge()->requestSource(path);
    return QTest::qWaitFor([this, path] { return session->bridge()->source(path) != nullptr; }, timeoutMs);
}

std::vector<float> SessionFixture::render(int64_t frames, double startBeat) {
    bridge().waitForDeviceStates();
    return engine.renderOffline(startBeat, frames);
}

float SessionFixture::level(int64_t frames) { return render(frames).at(static_cast<size_t>(2 * (frames - 1))); }

QString SessionFixture::clipTrack(const QString& path, double seconds, const QString& name, double startBeat) {
    engine.loadSource(path.toStdString());
    const QString trackId = editor().addAudioTrack(-1, name);
    const Clip clip = Clip::audio(name + QStringLiteral("c"), path, QFileInfo(path).completeBaseName(), startBeat,
                                  seconds, 0.0, seconds);
    editor().commitClips(QStringLiteral("Add"), {{trackId, {clip}}});
    return trackId;
}

}  // namespace sub::app::test
