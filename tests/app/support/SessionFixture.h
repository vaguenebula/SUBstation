#pragma once
// A session as the tests use it (the Python tests' `window` fixture, without its
// widgets): an engine without a device (clips without fades, unless asked for),
// and a Session on it that scans no plug-ins and keeps no browser index. What
// the session says (statusMessage, warning, information) is collected. The
// views' tests can drive a session through it too.

#include "Engine.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/Selection.h"
#include "session/Session.h"

#include <QString>
#include <QStringList>
#include <QUndoStack>

#include <cstdint>
#include <memory>
#include <vector>

namespace sub::app::test {

struct SessionFixture {
    explicit SessionFixture(bool clipFades = false);
    ~SessionFixture();
    SessionFixture(const SessionFixture&) = delete;
    SessionFixture& operator=(const SessionFixture&) = delete;

    sub::Engine engine;
    std::unique_ptr<Session> session;
    QStringList messages;      // statusMessage, in order
    QStringList warnings;      // warning, in order
    QStringList informations;  // information, in order

    Session& s() const { return *session; }
    Project& project() const { return *session->project(); }
    ProjectEditor& editor() const { return *session->editor(); }
    Selection& selection() const { return *session->selection(); }
    EngineBridge& bridge() const { return *session->bridge(); }
    QUndoStack& stack() const { return *session->undoStack(); }
    QString lastMessage() const { return messages.isEmpty() ? QString() : messages.back(); }

    // Until the render in the background has ended (false: not within `timeoutMs`).
    bool waitForRender(int timeoutMs = 30000) const;
    // Until the bridge has decoded the file (asked for here; false: not within `timeoutMs`).
    bool waitForSource(const QString& path, int timeoutMs = 10000) const;
    // `frames` of the arrangement from `startBeat`, rendered offline (interleaved stereo).
    std::vector<float> render(int64_t frames, double startBeat = 0.0);
    // The left channel of the last of `frames` rendered.
    float level(int64_t frames = 4000);
    // An audio track playing `path` from `startBeat` for `seconds` (the engine
    // has the file; the bridge decodes it too), as the Python tests' _commit("Add").
    // Its id; its clip's id is the track's name + "c".
    QString clipTrack(const QString& path, double seconds = 1.0, const QString& name = QStringLiteral("A"),
                      double startBeat = 0.0);
};

}  // namespace sub::app::test
