#pragma once
// Numbers appended to bytes in a given byte order, whatever the CPU's: the
// fields of what the application writes itself (a WAV file's header, a
// .vstpreset, a VST2 plug-in's fxb or fxp). (The other layers' binary files
// are made with sub::platform's, platform/Bytes.h.)

#include <QByteArray>
#include <QtEndian>

namespace sub::app {

// Least significant byte first.
inline void appendLe16(QByteArray& bytes, quint16 value) {
    char data[2];
    qToLittleEndian(value, data);
    bytes.append(data, 2);
}

inline void appendLe32(QByteArray& bytes, quint32 value) {
    char data[4];
    qToLittleEndian(value, data);
    bytes.append(data, 4);
}

inline void appendLe64(QByteArray& bytes, quint64 value) {
    char data[8];
    qToLittleEndian(value, data);
    bytes.append(data, 8);
}

// Most significant byte first.
inline void appendBe32(QByteArray& bytes, quint32 value) {
    char data[4];
    qToBigEndian(value, data);
    bytes.append(data, 4);
}

}  // namespace sub::app
