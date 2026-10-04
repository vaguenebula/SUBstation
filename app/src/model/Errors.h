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

#include <QString>

#include <stdexcept>

namespace sub::app {

class EditError : public std::runtime_error {
public:
    explicit EditError(const QString& message) : std::runtime_error(message.toStdString()), message_(message) {}

    QString message() const { return message_; }

private:
    QString message_;
};

class ProjectFileError : public std::runtime_error {
public:
    explicit ProjectFileError(const QString& message) : std::runtime_error(message.toStdString()), message_(message) {}

    QString message() const { return message_; }

private:
    QString message_;
};

}  // namespace sub::app
