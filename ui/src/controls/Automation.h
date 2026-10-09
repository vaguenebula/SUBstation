#pragma once

// A control's automation as the UI shows and offers it. Every control that
// can be automated (a knob, a value box, a role button: their `automation`)
// shows its state the same way, and every parameter's right-click menu offers
// the same Show, Delete and Re-Enable Automation.

#include <QString>

namespace sub::app {
class EngineBridge;
class Session;
}  // namespace sub::app

namespace sub::ui {

// "on" while its automation plays (the red dot), "off" while overridden (the
// grey dot), else "".
QString automationState(const app::EngineBridge& bridge, const QString& owner, const QString& key);

// One automation target (`owner`'s `key`) of the session, for a parameter's
// right-click menu. Each does nothing (or says no) without a session, or once
// the owner has gone.
struct AutomationTarget {
    app::Session* session = nullptr;
    QString owner;
    QString key;

    bool canAutomate() const;  // Show Automation
    bool hasEnvelope() const;  // Delete Automation
    bool isOverridden() const;  // Re-Enable Automation
    void show() const;  // its lane in the arrangement
    void deleteEnvelope() const;  // one undo step
    void reEnable() const;  // the owner's overridden automation plays again
};

}  // namespace sub::ui
