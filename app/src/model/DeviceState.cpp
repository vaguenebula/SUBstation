#include "model/DeviceState.h"

namespace sub::app::deviceState {

QByteArray encode(const Values& values) {
    QString text;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        QString escaped = it.value();
        escaped.replace(u'\\', QStringLiteral("\\\\"));
        escaped.replace(u'\n', QStringLiteral("\\n"));
        text += it.key() + u'=' + escaped + u'\n';
    }
    return text.toUtf8();
}

Values decode(const QByteArray& state) {
    Values values;
    const QString text = QString::fromUtf8(state);
    for (const QString& line : text.split(u'\n')) {
        const qsizetype equals = line.indexOf(u'=');
        if (equals < 0) continue;
        const QString name = line.left(equals);
        const QString raw = line.mid(equals + 1);
        QString value;
        for (qsizetype i = 0; i < raw.size(); ++i) {
            if (raw[i] == u'\\' && i + 1 < raw.size()) {
                ++i;
                value += raw[i] == u'n' ? QChar(u'\n') : raw[i];
            } else {
                value += raw[i];
            }
        }
        values.insert(name, value);
    }
    return values;
}

std::optional<QString> toModel(const Values& values) {
    if (values.isEmpty()) return std::nullopt;
    return QString::fromLatin1(encode(values).toBase64());
}

Values fromModel(const std::optional<QString>& state) {
    if (!state || state->isEmpty()) return {};
    // As base64 is read: characters that can't be in it are passed over.
    QByteArray cleaned;
    for (const QChar c : *state) {
        const char16_t u = c.unicode();
        if ((u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'+' ||
            u == u'/' || u == u'=') {
            cleaned.append(static_cast<char>(u));
        }
    }
    const auto decoded = QByteArray::fromBase64Encoding(cleaned, QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) return {};
    return decode(decoded.decoded);
}

}  // namespace sub::app::deviceState
