#pragma once
// Errors the user should see, with a message for them.
//
// EditError: an edit that can't be made (a send that would close a cycle, a
// name that can't be a preset's...). Anything QML can call catches it and
// reports the message through a signal (refused, statusMessage) instead of
// letting it escape.
//
// ProjectFileError: a project or preset file that can't be read (not one of
// ours, from a newer version, or damaged).
//
// Both are UserErrors: what says either the same way catches that.

#include <QString>

#include <stdexcept>

namespace sub::app {

class UserError : public std::runtime_error {
public:
    explicit UserError(const QString& message) : std::runtime_error(message.toStdString()), message_(message) {}

    QString message() const { return message_; }

private:
    QString message_;
};

class EditError : public UserError {
public:
    using UserError::UserError;
};

class ProjectFileError : public UserError {
public:
    using UserError::UserError;
};

}  // namespace sub::app
