#include "controls/Automation.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/Session.h"

namespace sub::ui {

QString automationState(const app::EngineBridge& bridge, const QString& owner, const QString& key) {
    if (bridge.isOverridden(owner, key))
        return QStringLiteral("off");
    return bridge.isAutomated(owner, key) ? QStringLiteral("on") : QString();
}

bool AutomationTarget::canAutomate() const { return session && session->bridge()->canAutomate(owner, key); }

bool AutomationTarget::hasEnvelope() const {
    return session && session->project()->hasOwner(owner) && !session->project()->envelope(owner, key).empty();
}

bool AutomationTarget::isOverridden() const { return session && session->bridge()->isOverridden(owner, key); }

void AutomationTarget::show() const {
    if (session && session->project()->hasOwner(owner))
        session->editor()->showAutomation(owner, key);
}

void AutomationTarget::deleteEnvelope() const {
    if (session && session->project()->hasOwner(owner))
        session->editor()->clearEnvelope(owner, key);
}

void AutomationTarget::reEnable() const {
    if (session)
        session->bridge()->reEnableAutomation(owner);
}

}  // namespace sub::ui
