#pragma once
// Reading an Ableton Live Set (.als): gzip-compressed XML (plain XML is read
// too), into a tree of its elements for the importer (io/LiveImport.h) to walk.
//
// Live writes most values as an element with a Value attribute
// (<Tempo><Manual Value="174" /></Tempo>): value("Tempo/Manual") reads it.
// Element names are shared between the elements that have them (a Live Set has
// a million elements, of some 1500 names). Text is kept only where there is
// some (a plug-in's state, in hex).
//
// Reading throws ProjectFileError (model/Errors.h) with a message for the user:
// unreadable, not gzip or XML, damaged, or not a Live Set.

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app::live {

class Element {
public:
    QString tag;
    std::vector<std::pair<QString, QString>> attributes;
    QString text;  // its character data, trimmed ("" for most)
    std::vector<std::unique_ptr<Element>> children;

    // An attribute's value ("" if it has none of that name).
    QString attribute(QStringView name) const;
    bool hasAttribute(QStringView name) const;
    // Its first child named `name`; null if none.
    const Element* child(QStringView name) const;
    // The element at a path of child names ("DeviceChain/Mixer/Volume"); null if
    // there is none. "" is this element.
    const Element* at(QStringView path) const;
    // Its children named `name` (all of them for "").
    std::vector<const Element*> all(QStringView name = {}) const;
    // Every element below it named `name`, depth first.
    std::vector<const Element*> descendants(QStringView name) const;

    // The Value attribute of the element at `path` (none if there is no element
    // there, or it has no Value).
    std::optional<QString> value(QStringView path) const;
    double number(QStringView path, double fallback = 0.0) const;
    int integer(QStringView path, int fallback = 0) const;
    // "true" / "false" (and 1 / 0).
    bool flag(QStringView path, bool fallback = false) const;
    // A number attribute of this element ("Time" of an event, of a clip).
    double numberAttribute(QStringView name, double fallback = 0.0) const;

private:
    void collect(QStringView name, std::vector<const Element*>& found) const;
};

// The root element (<Ableton ...>) of a Live Set file; `path` names it in errors.
std::unique_ptr<Element> readLiveSet(const QString& path);
// The same from the file's bytes (gzip or XML).
std::unique_ptr<Element> parseLiveSet(const QByteArray& bytes, const QString& name);
// A gzip file's contents (its first member). Throws ProjectFileError("... is
// damaged") if it isn't gzip or doesn't check out (its length, its CRC).
QByteArray gunzip(const QByteArray& gzip, const QString& name);
// Whether bytes start as a gzip file does.
bool isGzip(const QByteArray& bytes);

}  // namespace sub::app::live
