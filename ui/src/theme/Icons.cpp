#include "theme/Icons.h"

#include "theme/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace sub::ui {

namespace {

using Draw = std::function<void(QPainter&, const QColor&, bool on)>;

struct Icon {
    QColor color;  // its default colour
    Draw draw;
};

QPen pen(const QColor& c, qreal width, Qt::PenCapStyle cap = Qt::SquareCap, Qt::PenJoinStyle join = Qt::BevelJoin) {
    return QPen(c, width, Qt::SolidLine, cap, join);
}

QPainterPath path(std::initializer_list<QPointF> points, bool close) {
    QPainterPath result(*points.begin());
    for (auto it = points.begin() + 1; it != points.end(); ++it)
        result.lineTo(*it);
    if (close)
        result.closeSubpath();
    return result;
}

// The icons, one function each.
const std::map<QString, Icon>& icons() {
    static const std::map<QString, Icon> table = [] {
        std::map<QString, Icon> t;
        const QColor text = Theme::kText, dim = Theme::kTextDim;

        t[QStringLiteral("play")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.fillPath(path({{18, 12}, {52, 32}, {18, 52}}, true), c);
                                     }};
        t[QStringLiteral("stop")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.fillRect(QRectF(16, 16, 32, 32), c);
                                     }};
        t[QStringLiteral("record")] = {QColor(0xff, 0x5a, 0x4d), [](QPainter& p, const QColor& c, bool) {
                                           p.setBrush(c);
                                           p.setPen(Qt::NoPen);
                                           p.drawEllipse(QRectF(16, 16, 32, 32));
                                       }};
        t[QStringLiteral("metronome")] = {text, [](QPainter& p, const QColor& c, bool) {
                                              p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                              p.setBrush(Qt::NoBrush);
                                              p.drawPath(path({{24, 10}, {40, 10}, {52, 54}, {12, 54}}, true));
                                              p.drawLine(QPointF(32, 44), QPointF(46, 18));
                                          }};
        t[QStringLiteral("loop")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.setPen(pen(c, 6, Qt::RoundCap));
                                         p.drawArc(QRectF(10, 16, 44, 32), 30 * 16, 300 * 16);
                                         p.fillPath(path({{46, 8}, {58, 20}, {42, 24}}, true), c);
                                     }};
        t[QStringLiteral("follow")] = {text, [](QPainter& p, const QColor& c, bool) {
                                           p.setPen(pen(c, 5, Qt::RoundCap));
                                           p.drawLine(QPointF(14, 32), QPointF(46, 32));
                                           p.fillPath(path({{40, 18}, {56, 32}, {40, 46}}, true), c);
                                           p.drawLine(QPointF(10, 12), QPointF(10, 52));
                                       }};
        // An envelope with a loop back: automation plays again.
        t[QStringLiteral("re_enable_automation")] = {
            text, [](QPainter& p, const QColor& c, bool) {
                p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path({{8, 50}, {24, 26}, {38, 40}, {56, 14}}, false));
                p.setBrush(c);
                p.setPen(Qt::NoPen);
                for (const QPointF& point : {QPointF(8, 50), QPointF(24, 26), QPointF(38, 40), QPointF(56, 14)})
                    p.drawEllipse(point, 6, 6);
            }};
        // A padlock: closed when on (automation stays put), open when off.
        t[QStringLiteral("lock_envelopes")] = {
            text, [](QPainter& p, const QColor& c, bool closed) {
                p.setPen(pen(c, 5, Qt::FlatCap));
                p.setBrush(Qt::NoBrush);
                // The shackle: in the body when closed; raised, its left leg free, when open.
                const qreal top = closed ? 10.0 : 3.0;
                QPainterPath shackle(QPointF(21, closed ? 32 : top + 17));
                shackle.lineTo(21, top + 12);
                shackle.arcTo(QRectF(21, top, 22, 24), 180, -180);
                shackle.lineTo(43, 32);
                p.drawPath(shackle);
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                p.drawRoundedRect(QRectF(12, 30, 40, 28), 5, 5);
            }};
        t[QStringLiteral("headphones")] = {text, [](QPainter& p, const QColor& c, bool) {
                                               p.setPen(pen(c, 5, Qt::RoundCap));
                                               p.drawArc(QRectF(12, 10, 40, 40), 0, 180 * 16);
                                               p.setBrush(c);
                                               p.drawRoundedRect(QRectF(10, 32, 10, 20), 3, 3);
                                               p.drawRoundedRect(QRectF(44, 32, 10, 20), 3, 3);
                                           }};
        t[QStringLiteral("folder")] = {dim, [](QPainter& p, const QColor& c, bool) {
                                           p.fillPath(path({{8, 16}, {26, 16}, {31, 22}, {56, 22}, {56, 50}, {8, 50}},
                                                           true),
                                                      c);
                                       }};
        t[QStringLiteral("waveform")] = {dim, [](QPainter& p, const QColor& c, bool) {
                                             p.setPen(pen(c, 5, Qt::RoundCap));
                                             const qreal bars[][2] = {{12, 8}, {22, 24}, {32, 36}, {42, 18}, {52, 10}};
                                             for (const auto& bar : bars)
                                                 p.drawLine(QPointF(bar[0], 32 - bar[1] / 2),
                                                            QPointF(bar[0], 32 + bar[1] / 2));
                                         }};
        t[QStringLiteral("plugin")] = {dim, [](QPainter& p, const QColor& c, bool) {
                                           p.setPen(pen(c, 5));
                                           p.setBrush(Qt::NoBrush);
                                           p.drawRoundedRect(QRectF(12, 14, 40, 36), 5, 5);
                                           p.drawLine(QPointF(24, 14), QPointF(24, 6));
                                           p.drawLine(QPointF(40, 14), QPointF(40, 6));
                                       }};
        // Three sliders, set: a device's settings saved.
        t[QStringLiteral("preset")] = {dim, [](QPainter& p, const QColor& c, bool) {
                                           p.setPen(pen(c, 5, Qt::RoundCap));
                                           const qreal sliders[][2] = {{14, 40}, {32, 20}, {50, 34}};  // (y, x)
                                           for (const auto& slider : sliders) {
                                               p.drawLine(QPointF(8, slider[0]), QPointF(56, slider[0]));
                                               p.setBrush(c);
                                               p.drawEllipse(QPointF(slider[1], slider[0]), 5, 5);
                                           }
                                       }};
        // A window with a title bar: shows a plug-in's own editor.
        t[QStringLiteral("plugin_window")] = {text, [](QPainter& p, const QColor& c, bool) {
                                                  p.setPen(pen(c, 5));
                                                  p.setBrush(Qt::NoBrush);
                                                  p.drawRoundedRect(QRectF(8, 12, 48, 40), 4, 4);
                                                  p.fillRect(QRectF(8, 12, 48, 11), c);
                                              }};
        // An arrow coming in from the side: a device's sidechain input.
        t[QStringLiteral("sidechain")] = {text, [](QPainter& p, const QColor& c, bool) {
                                              p.setPen(pen(c, 6, Qt::RoundCap, Qt::RoundJoin));
                                              p.setBrush(Qt::NoBrush);
                                              p.drawLine(QPointF(48, 10), QPointF(48, 54));  // what it goes into
                                              p.drawLine(QPointF(8, 32), QPointF(36, 32));
                                              p.drawPath(path({{26, 20}, {38, 32}, {26, 44}}, false));
                                          }};
        // Six spokes with a pair of twigs each: a frozen track.
        t[QStringLiteral("snowflake")] = {Theme::kFrozen, [](QPainter& p, const QColor& c, bool) {
                                              p.setPen(pen(c, 4.5, Qt::RoundCap));
                                              p.translate(32, 32);
                                              for (int i = 0; i < 6; ++i) {
                                                  p.drawLine(QPointF(0, 0), QPointF(0, -26));
                                                  p.drawLine(QPointF(0, -15), QPointF(-8, -23));
                                                  p.drawLine(QPointF(0, -15), QPointF(8, -23));
                                                  p.rotate(60);
                                              }
                                          }};
        // A floppy disk.
        t[QStringLiteral("save")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                         p.setBrush(Qt::NoBrush);
                                         p.drawPath(path({{10, 10}, {46, 10}, {54, 18}, {54, 54}, {10, 54}}, true));
                                         p.fillRect(QRectF(20, 10, 22, 13), c);
                                         p.drawRect(QRectF(19, 35, 26, 19));
                                     }};
        // Two chain links: one side follows the other.
        t[QStringLiteral("link")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.setPen(pen(c, 6, Qt::RoundCap, Qt::RoundJoin));
                                         p.setBrush(Qt::NoBrush);
                                         p.save();
                                         p.translate(32, 32);
                                         p.rotate(-45);
                                         p.drawRoundedRect(QRectF(-26, -9, 30, 18), 9, 9);
                                         p.drawRoundedRect(QRectF(-4, -9, 30, 18), 9, 9);
                                         p.restore();
                                     }};
        // A lemniscate: what is held goes round for ever (a delay's Freeze).
        t[QStringLiteral("infinity")] = {text, [](QPainter& p, const QColor& c, bool) {
                                             QPainterPath curve(QPointF(32, 32));
                                             curve.cubicTo(QPointF(42, 16), QPointF(58, 20), QPointF(58, 32));
                                             curve.cubicTo(QPointF(58, 44), QPointF(42, 48), QPointF(32, 32));
                                             curve.cubicTo(QPointF(22, 16), QPointF(6, 20), QPointF(6, 32));
                                             curve.cubicTo(QPointF(6, 44), QPointF(22, 48), QPointF(32, 32));
                                             p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                             p.setBrush(Qt::NoBrush);
                                             p.drawPath(curve);
                                         }};
        // Two arrows apart: show it bigger, in a window of its own.
        t[QStringLiteral("expand")] = {text, [](QPainter& p, const QColor& c, bool) {
                                           p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                           p.setBrush(Qt::NoBrush);
                                           p.drawLine(QPointF(14, 50), QPointF(50, 14));
                                           const qreal corners[][4] = {{50, 14, -1, 1}, {14, 50, 1, -1}};  // x, y, dx, dy
                                           for (const auto& k : corners)
                                               p.drawPath(path({{k[0] + 18 * k[2], k[1]}, {k[0], k[1]}, {k[0], k[1] + 18 * k[3]}},
                                                               false));
                                       }};
        // Three faders: show a device's controls.
        t[QStringLiteral("sliders")] = {text, [](QPainter& p, const QColor& c, bool) {
                                            p.setPen(pen(c, 4, Qt::RoundCap));
                                            const qreal faders[][2] = {{16, 40}, {32, 22}, {48, 34}};  // x, the knob's y
                                            for (const auto& fader : faders)
                                                p.drawLine(QPointF(fader[0], 10), QPointF(fader[0], 54));
                                            p.setPen(Qt::NoPen);
                                            p.setBrush(c);
                                            for (const auto& fader : faders)
                                                p.drawRoundedRect(QRectF(fader[0] - 8, fader[1] - 5, 16, 10), 3, 3);
                                        }};
        // Rows, each with its switch: a rack's chain list.
        t[QStringLiteral("chain_list")] = {text, [](QPainter& p, const QColor& c, bool) {
                                               for (const qreal y : {12.0, 28.0, 44.0}) {
                                                   p.fillRect(QRectF(8, y, 9, 9), c);
                                                   p.fillRect(QRectF(23, y + 2, 33, 5), c);
                                               }
                                           }};
        // Devices side by side in a bracket: the devices of a rack's chain, beside it.
        t[QStringLiteral("rack_devices")] = {text, [](QPainter& p, const QColor& c, bool) {
                                                 p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                                 p.setBrush(Qt::NoBrush);
                                                 p.drawPath(path({{14, 10}, {6, 10}, {6, 54}, {14, 54}}, false));
                                                 p.drawPath(path({{50, 10}, {58, 10}, {58, 54}, {50, 54}}, false));
                                                 p.setPen(Qt::NoPen);
                                                 p.setBrush(c);
                                                 p.drawRoundedRect(QRectF(15, 18, 15, 28), 3, 3);
                                                 p.drawRoundedRect(QRectF(34, 18, 15, 28), 3, 3);
                                             }};
        // The Sampler's modes: Classic (a loop), 1-Shot (an arrow to its end), Slice (cuts).
        t[QStringLiteral("sampler_classic")] = {text, [](QPainter& p, const QColor& c, bool) {
                                                    p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                                    p.setBrush(Qt::NoBrush);
                                                    p.drawArc(QRectF(12, 12, 40, 40), 100 * 16, 290 * 16);
                                                    p.setPen(Qt::NoPen);
                                                    p.setBrush(c);
                                                    p.drawPath(path({{24, 4}, {38, 13}, {25, 22}}, true));
                                                }};
        t[QStringLiteral("sampler_oneshot")] = {text, [](QPainter& p, const QColor& c, bool) {
                                                    p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                                    p.drawLine(QPointF(8, 32), QPointF(40, 32));
                                                    p.drawPath(path({{30, 20}, {42, 32}, {30, 44}}, false));
                                                    p.drawLine(QPointF(52, 14), QPointF(52, 50));
                                                }};
        t[QStringLiteral("sampler_slice")] = {text, [](QPainter& p, const QColor& c, bool) {
                                                  p.setPen(pen(c, 5, Qt::RoundCap));
                                                  for (const qreal x : {12.0, 32.0, 52.0})
                                                      p.drawLine(QPointF(x, 12), QPointF(x, 52));
                                                  p.setPen(pen(c, 3, Qt::RoundCap));
                                                  for (const qreal x : {22.0, 42.0})
                                                      p.drawLine(QPointF(x, 24), QPointF(x, 40));
                                              }};
        // Filter shapes: low-pass, high-pass, band-pass, notch.
        const auto curve = [](std::initializer_list<QPointF> through) {
            return [points = std::vector<QPointF>(through)](QPainter& p, const QColor& c, bool) {
                QPainterPath shape(points.front());
                for (size_t i = 1; i + 1 < points.size(); i += 2)
                    shape.quadTo(points[i], points[i + 1]);
                p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(shape);
            };
        };
        t[QStringLiteral("filter_lowpass")] = {text, curve({{6, 26}, {30, 26}, {38, 22}, {46, 18}, {58, 54}})};
        t[QStringLiteral("filter_highpass")] = {text, curve({{6, 54}, {18, 18}, {26, 22}, {34, 26}, {58, 26}})};
        t[QStringLiteral("filter_bandpass")] = {text, curve({{6, 54}, {20, 54}, {26, 36}, {32, 14}, {38, 36},
                                                             {44, 54}, {58, 54}})};
        t[QStringLiteral("filter_notch")] = {text, curve({{6, 18}, {20, 18}, {26, 34}, {32, 54}, {38, 34},
                                                          {44, 18}, {58, 18}})};
        // LFO shapes: sine, triangle, saw up, saw down, square, random.
        const auto line = [](std::initializer_list<QPointF> through) {
            return [points = std::vector<QPointF>(through)](QPainter& p, const QColor& c, bool) {
                QPainterPath shape(points.front());
                for (size_t i = 1; i < points.size(); ++i)
                    shape.lineTo(points[i]);
                p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(shape);
            };
        };
        t[QStringLiteral("wave_sine")] = {text, [](QPainter& p, const QColor& c, bool) {
                                              QPainterPath shape(QPointF(6, 32));
                                              shape.cubicTo(QPointF(14, 4), QPointF(24, 4), QPointF(32, 32));
                                              shape.cubicTo(QPointF(40, 60), QPointF(50, 60), QPointF(58, 32));
                                              p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                              p.setBrush(Qt::NoBrush);
                                              p.drawPath(shape);
                                          }};
        t[QStringLiteral("wave_triangle")] = {text, line({{6, 32}, {19, 12}, {45, 52}, {58, 32}})};
        t[QStringLiteral("wave_saw_up")] = {text, line({{6, 52}, {32, 12}, {32, 52}, {58, 12}})};
        t[QStringLiteral("wave_saw_down")] = {text, line({{6, 12}, {32, 52}, {32, 12}, {58, 52}})};
        t[QStringLiteral("wave_square")] = {text, line({{6, 52}, {6, 12}, {32, 12}, {32, 52}, {58, 52}, {58, 12}})};
        t[QStringLiteral("wave_random")] = {text, line({{6, 40}, {17, 40}, {17, 14}, {28, 14}, {28, 50}, {39, 50},
                                                        {39, 26}, {50, 26}, {50, 44}, {58, 44}})};
        // A quaver: synced to the song's tempo.
        t[QStringLiteral("note")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.setPen(Qt::NoPen);
                                         p.setBrush(c);
                                         p.drawEllipse(QRectF(12, 38, 20, 16));
                                         p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                         p.setBrush(Qt::NoBrush);
                                         p.drawPath(path({{30, 46}, {30, 10}, {48, 22}}, false));
                                     }};
        // A note bending up: the piano roll's bend mode (its notes' pitch curves).
        t[QStringLiteral("bend")] = {text, [](QPainter& p, const QColor& c, bool) {
                                         p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                         p.setBrush(Qt::NoBrush);
                                         QPainterPath curve(QPointF(8, 48));
                                         curve.lineTo(22, 48);
                                         curve.cubicTo(QPointF(34, 48), QPointF(36, 18), QPointF(48, 18));
                                         curve.lineTo(56, 18);
                                         p.drawPath(curve);
                                         p.setPen(Qt::NoPen);
                                         p.setBrush(c);
                                         p.drawEllipse(QPointF(22, 48), 5, 5);
                                         p.drawEllipse(QPointF(48, 18), 5, 5);
                                     }};
        // A line swinging, wider as it goes: drawing vibrato onto a note.
        t[QStringLiteral("vibrato")] = {text, [](QPainter& p, const QColor& c, bool) {
                                            p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                            p.setBrush(Qt::NoBrush);
                                            QPainterPath wave(QPointF(6, 32));
                                            for (int x = 7; x <= 58; ++x) {
                                                const double swell = std::min(1.0, (x - 6) / 26.0);
                                                wave.lineTo(x, 32 - 16 * swell * std::sin((x - 6) * 0.36));
                                            }
                                            p.drawPath(wave);
                                        }};
        // A device's fold button: a triangle pointing down while it is open, right while folded.
        t[QStringLiteral("fold")] = {text, [](QPainter& p, const QColor& c, bool folded) {
                                         p.fillPath(folded ? path({{22, 14}, {46, 32}, {22, 50}}, true)
                                                           : path({{14, 22}, {50, 22}, {32, 46}}, true),
                                                    c);
                                     }};
        t[QStringLiteral("search")] = {dim, [](QPainter& p, const QColor& c, bool) {
                                           p.setPen(pen(c, 5, Qt::RoundCap));
                                           p.setBrush(Qt::NoBrush);
                                           p.drawEllipse(QRectF(10, 10, 30, 30));
                                           p.drawLine(QPointF(37, 37), QPointF(54, 54));
                                       }};
        // Two arrows, one each way: a hot swap (what the browser selects plays in its place).
        t[QStringLiteral("hotswap")] = {text, [](QPainter& p, const QColor& c, bool) {
                                            p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                            p.setBrush(Qt::NoBrush);
                                            p.drawLine(QPointF(10, 21), QPointF(52, 21));
                                            p.drawPath(path({{41, 10}, {52, 21}, {41, 32}}, false));
                                            p.drawLine(QPointF(54, 43), QPointF(12, 43));
                                            p.drawPath(path({{23, 32}, {12, 43}, {23, 54}}, false));
                                        }};
        // A warning triangle: a file that isn't there any more.
        t[QStringLiteral("missing")] = {Theme::kRecordOn, [](QPainter& p, const QColor& c, bool) {
                                            p.setPen(pen(c, 5, Qt::RoundCap, Qt::RoundJoin));
                                            p.setBrush(Qt::NoBrush);
                                            p.drawPath(path({{32, 8}, {58, 54}, {6, 54}}, true));
                                            p.drawLine(QPointF(32, 24), QPointF(32, 38));
                                            p.setBrush(c);
                                            p.setPen(Qt::NoPen);
                                            p.drawEllipse(QPointF(32, 46), 3.5, 3.5);
                                        }};
        // The application's own icon: its colours are its own, even disabled.
        t[QStringLiteral("app_icon")] = {Theme::kAccent, [](QPainter& p, const QColor&, bool) {
                                             p.setBrush(Theme::kPanelAlt);
                                             p.setPen(Qt::NoPen);
                                             p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);
                                             p.setPen(pen(Theme::kAccent, 6, Qt::RoundCap));
                                             const qreal bars[][2] = {{14, 12}, {24, 30}, {34, 40}, {44, 22}, {52, 10}};
                                             for (const auto& bar : bars)
                                                 p.drawLine(QPointF(bar[0], 32 - bar[1] / 2),
                                                            QPointF(bar[0], 32 + bar[1] / 2));
                                         }};
        return t;
    }();
    return table;
}

}  // namespace

Icons::Icons(QObject* parent) : QObject(parent) {}

QStringList Icons::names() {
    QStringList result;
    for (const auto& [name, icon] : icons())
        result << name;
    return result;
}

bool Icons::has(const QString& name) { return icons().count(name) > 0; }

QColor Icons::defaultColor(const QString& name) {
    const auto it = icons().find(name);
    return it == icons().end() ? QColor() : it->second.color;
}

bool Icons::draw(QPainter& painter, const QString& name, const QColor& color, bool on) {
    const auto it = icons().find(name);
    if (it == icons().end())
        return false;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    it->second.draw(painter, color.isValid() ? color : it->second.color, on);
    painter.restore();
    return true;
}

QImage Icons::image(const QString& name, int pixels, const QColor& color, bool on, bool disabled) {
    if (!has(name) || pixels <= 0)
        return {};
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.scale(qreal(pixels) / kGrid, qreal(pixels) / kGrid);
    draw(painter, name, disabled ? QColor(Theme::kTextDisabled) : color, on);
    return image;
}

QString Icons::url(const QString& name, const QVariant& color, bool on, bool disabled) {
    QUrlQuery query;
    if (color.isValid() && !color.isNull()) {
        const QColor c = color.value<QColor>();
        if (c.isValid())
            query.addQueryItem(QStringLiteral("color"), c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb));
    }
    if (on)
        query.addQueryItem(QStringLiteral("state"), QStringLiteral("on"));
    if (disabled)
        query.addQueryItem(QStringLiteral("mode"), QStringLiteral("disabled"));
    QUrl url;
    url.setScheme(QStringLiteral("image"));
    url.setHost(QStringLiteral("icons"));
    url.setPath(QLatin1Char('/') + name);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

IconProvider::IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage IconProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    // `id` is "<name>?<query>", the query maybe still percent-encoded.
    const qsizetype mark = id.indexOf(QLatin1Char('?'));
    const QString name = mark < 0 ? id : id.left(mark);
    const QUrlQuery query(mark < 0 ? QString() : id.mid(mark + 1));
    // "#rrggbb", "#aarrggbb", a colour name, or hex digits without the "#".
    QColor color;
    const QString colorText = query.queryItemValue(QStringLiteral("color"), QUrl::FullyDecoded);
    if (!colorText.isEmpty()) {
        color = QColor::fromString(colorText);
        if (!color.isValid())
            color = QColor::fromString(QString(QLatin1Char('#') + colorText));
    }
    const bool on = query.queryItemValue(QStringLiteral("state")) == QLatin1String("on");
    const bool disabled = query.queryItemValue(QStringLiteral("mode")) == QLatin1String("disabled");
    int pixels = Icons::kGrid;
    if (requestedSize.width() > 0 || requestedSize.height() > 0)
        pixels = qMax(requestedSize.width(), requestedSize.height());
    const QImage image = Icons::image(name, pixels, color, on, disabled);
    if (size)
        *size = image.size();
    return image;
}

}  // namespace sub::ui
