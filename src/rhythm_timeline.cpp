#include "rythmo/rhythm_timeline.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPolygon>
#include <QContextMenuEvent>
#include <QMenu>
#include <QCursor>
#include <QTime>
#include <QWheelEvent>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rythmo {
namespace {
constexpr int ruler_height = 34;
constexpr int left_margin = 76;
constexpr int right_margin = 16;
constexpr double cursor_fraction = 0.34;

QColor role_color(const DetxDocument* document, const std::string& role_id) {
    if (document) {
        for (const auto& role : document->roles()) {
            if (role.id != role_id) continue;
            const QColor parsed(QString::fromUtf8(role.color.c_str()));
            if (parsed.isValid() && parsed != QColor(Qt::black)) return parsed;
            break;
        }
    }
    return QColor(QStringLiteral("#4EA8DE"));
}

QString role_name(const DetxDocument* document, const std::string& role_id) {
    if (document) {
        for (const auto& role : document->roles()) {
            if (role.id == role_id) return QString::fromUtf8(role.name.c_str());
        }
    }
    return role_id.empty() ? QStringLiteral("Sans rôle") : QString::fromUtf8(role_id.c_str());
}

QString to_qstring(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

RhythmTimelineWidget::RhythmTimelineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(190);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void RhythmTimelineWidget::setDocument(const DetxDocument* document, Timecode video_start,
                                       int timecode_fps, double media_fps, qint64 duration_ms) {
    document_ = document;
    video_start_ = video_start;
    timecode_fps_ = std::max(1, timecode_fps);
    media_fps_ = media_fps > 0.0 ? media_fps : static_cast<double>(timecode_fps_);
    duration_ms_ = std::max<qint64>(0, duration_ms);
    if (!document_ || selected_line_ >= static_cast<int>(document_->lines().size())) selected_line_ = -1;
    rebuildSceneCache();
    update();
}

void RhythmTimelineWidget::rebuildSceneCache() {
    cached_lines_.clear();
    cached_markers_.clear();
    if (!document_) return;

    const auto& lines = document_->lines();
    cached_lines_.reserve(lines.size());
    for (const auto& line : lines) {
        CachedLine cached;
        cached.track = line.track;
        cached.role = role_name(document_, line.role_id);
        cached.role_color = role_color(document_, line.role_id);

        auto add_cue = [&](const std::string& source_text, std::optional<Timecode> start_tc,
                           std::optional<Timecode> end_tc) {
            const QString full = QString::fromUtf8(source_text.data(), static_cast<qsizetype>(source_text.size())).trimmed();
            if (full.isEmpty()) return;
            if (!start_tc && !line.lipsyncs.empty()) start_tc = line.lipsyncs.front().timecode;
            if (!end_tc && !line.lipsyncs.empty()) end_tc = line.lipsyncs.back().timecode;

            CachedCue cue;
            cue.start_follows_playhead = !start_tc.has_value();
            cue.end_follows_playhead = !end_tc.has_value();
            cue.start_ms = start_tc ? positionForTimecode(*start_tc) : 0;
            cue.end_ms = end_tc ? positionForTimecode(*end_tc) : 0;
            cue.rows = full.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
            cached.cues.push_back(std::move(cue));
        };

        if (!line.text_segments.empty()) {
            for (std::size_t i = 0; i < line.text_segments.size();) {
                const auto& first = line.text_segments[i];
                std::string phrase = first.text;
                std::size_t next = i + 1;
                while (next < line.text_segments.size() &&
                       line.text_segments[next].start == first.start &&
                       line.text_segments[next].end == first.end) {
                    phrase += line.text_segments[next].text;
                    ++next;
                }
                add_cue(phrase, first.start, first.end);
                i = next;
            }
        } else {
            const std::optional<Timecode> start = line.lipsyncs.empty()
                ? std::nullopt : std::optional<Timecode>(line.lipsyncs.front().timecode);
            const std::optional<Timecode> end = line.lipsyncs.empty()
                ? std::nullopt : std::optional<Timecode>(line.lipsyncs.back().timecode);
            add_cue(line.text, start, end);
        }

        cached.signs.reserve(line.lipsyncs.size());
        for (const auto& sign : line.lipsyncs) {
            const QColor color = sign.kind.rfind("in_", 0) == 0 ? QColor(QStringLiteral("#66D9C2"))
                : (sign.kind.rfind("out_", 0) == 0 ? QColor(QStringLiteral("#F4B860"))
                                                    : QColor(QStringLiteral("#72BDE6")));
            cached.signs.push_back({positionForTimecode(sign.timecode), color});
        }
        cached_lines_.push_back(std::move(cached));
    }

    cached_markers_.reserve(document_->markers().size());
    for (const auto& marker : document_->markers())
        cached_markers_.push_back({positionForTimecode(marker.timecode), marker.kind == "loop"});
}

void RhythmTimelineWidget::setPlayhead(qint64 position_ms) {
    playhead_ms_ = std::max<qint64>(0, position_ms);
    update();
}

void RhythmTimelineWidget::setPlayheadForRender(qint64 position_ms) {
    // Export calls QWidget::render() synchronously immediately afterwards, so
    // queueing a second asynchronous repaint for every frame is unnecessary.
    playhead_ms_ = std::max<qint64>(0, position_ms);
}

void RhythmTimelineWidget::setSelectionHandler(std::function<void(int, int)> handler) {
    selection_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setInsertAtHandler(std::function<void(int, qint64)> handler) {
    insert_at_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setResizeHandler(std::function<void(int, int, qint64)> handler) {
    resize_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setResizeTransactionHandler(std::function<void(bool)> handler) {
    resize_transaction_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setPlayheadHandler(std::function<void(qint64)> handler) {
    playhead_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setPlayheadDragHandler(std::function<void(bool)> handler) {
    playhead_drag_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setTextFont(const QFont& font) {
    text_font_ = font;
    for (auto& line : cached_lines_)
        for (auto& cue : line.cues) cue.text_widths_by_pixel_size.clear();
    update();
}

void RhythmTimelineWidget::setCleanPresentation(bool enabled) {
    clean_presentation_ = enabled;
    update();
}

void RhythmTimelineWidget::setSplitHandler(std::function<void(int)> handler) {
    split_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setEditHandler(std::function<void(int)> handler) {
    edit_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setDeleteHandler(std::function<void(int)> handler) {
    delete_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setSelection(int line_index, int track) {
    selected_line_ = line_index;
    selected_track_ = std::clamp(track, 0, 3);
    update();
}

void RhythmTimelineWidget::copyDisplayStateFrom(const RhythmTimelineWidget& other) {
    visible_duration_ms_ = other.visible_duration_ms_;
    selected_line_ = other.selected_line_;
    selected_track_ = other.selected_track_;
    text_font_ = other.text_font_;
    update();
}

qint64 RhythmTimelineWidget::positionForTimecode(const Timecode& timecode) const {
    try {
        const auto event_frames = timecode.to_frames(timecode_fps_);
        const auto base_frames = video_start_.to_frames(timecode_fps_);
        const long double difference = static_cast<long double>(event_frames - base_frames);
        return static_cast<qint64>(std::llround(difference * 1000.0L / timecode_fps_));
    } catch (...) {
        return 0;
    }
}

qint64 RhythmTimelineWidget::positionForX(int x) const {
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));
    return playhead_ms_ - qRound64(visible_duration_ms_ * cursor_fraction) +
        qRound64(static_cast<double>(x - cursor_x) * visible_duration_ms_ / plot_width);
}

int RhythmTimelineWidget::xForPosition(qint64 position_ms) const {
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const qint64 visible_start = playhead_ms_ - qRound64(visible_duration_ms_ * cursor_fraction);
    return left_margin + static_cast<int>(std::llround(
        static_cast<long double>(position_ms - visible_start) * plot_width / visible_duration_ms_));
}

qint64 RhythmTimelineWidget::effectiveDuration() const {
    if (duration_ms_ > 0) return duration_ms_;
    qint64 inferred = 0;
    if (document_) {
        for (const auto& line : document_->lines()) {
            for (const auto& segment : line.text_segments) {
                if (segment.end) inferred = std::max(inferred, positionForTimecode(*segment.end));
            }
            if (!line.text_segments.empty()) continue;
            for (const auto& sign : line.lipsyncs) inferred = std::max(inferred, positionForTimecode(sign.timecode));
        }
    }
    return std::max<qint64>(10000, inferred + 5000);
}

void RhythmTimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#0C131B")));

    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));
    const qint64 cursor_offset_ms = qRound64(visible_duration_ms_ * cursor_fraction);
    const qint64 visible_start_ms = playhead_ms_ - cursor_offset_ms;
    const qint64 visible_end_ms = visible_start_ms + visible_duration_ms_;
    const auto x_for_ms = [&](qint64 ms) {
        const long double fraction = static_cast<long double>(ms - visible_start_ms) / visible_duration_ms_;
        return left_margin + static_cast<int>(std::llround(fraction * plot_width));
    };
    const auto tc_for_ms = [&](qint64 ms) {
        const auto base = video_start_.to_frames(timecode_fps_);
        const auto offset = static_cast<qint64>(std::llround(
            static_cast<long double>(ms) * timecode_fps_ / 1000.0L));
        return Timecode::from_frames(std::max<std::int64_t>(0, base + offset), timecode_fps_);
    };

    // Fixed ruler and time scale; the DetX text segments pass underneath the red cue line.
    painter.fillRect(QRect(0, 0, width(), ruler_height), QColor(QStringLiteral("#16212C")));
    constexpr qint64 tick_ms = 1000;
    const qint64 first_tick = static_cast<qint64>(std::floor(static_cast<double>(visible_start_ms) / tick_ms)) * tick_ms;
    for (qint64 ms = first_tick; ms <= visible_end_ms + tick_ms; ms += tick_ms) {
        const int x = x_for_ms(ms);
        if (x < left_margin - 41 || x > width() - right_margin) continue;
        if (x >= left_margin) {
            painter.setPen(QColor(QStringLiteral("#293846")));
            painter.drawLine(x, ruler_height, x, height());
        }
        painter.setPen(QColor(QStringLiteral("#B5C4D2")));
        const auto label = to_qstring(tc_for_ms(ms).format());
        painter.drawText(QRect(x - 41, 2, 82, ruler_height - 4), Qt::AlignHCenter | Qt::AlignVCenter, label);
    }
    // The left title panel fades over the ruler labels instead of cutting them
    // off at the lane boundary. This keeps timecodes readable as they pass
    // beneath the fixed RYTHMO label, including on narrow timeline layouts.
    const int fade_end = left_margin + 112;
    QLinearGradient ruler_fade(0, 0, fade_end, 0);
    const QColor ruler_background(QStringLiteral("#16212C"));
    ruler_fade.setColorAt(0.0, ruler_background);
    ruler_fade.setColorAt(static_cast<qreal>(left_margin) / fade_end, ruler_background);
    ruler_fade.setColorAt(1.0, QColor(22, 33, 44, 0));
    painter.fillRect(QRect(0, 0, fade_end, ruler_height), ruler_fade);
    if (!clean_presentation_) {
        painter.setPen(QColor(QStringLiteral("#D8E4ED")));
        painter.drawText(QRect(8, 0, left_margin - 16, ruler_height), Qt::AlignVCenter | Qt::AlignLeft,
                         QStringLiteral("RYTHMO"));
    }

    for (int track = 0; track < 4; ++track) {
        const int y = ruler_height + track * lane_height;
        const bool selected = track == selected_track_;
        painter.fillRect(QRect(0, y, width(), lane_height), selected
            ? QColor(QStringLiteral("#1A3140"))
            : (track % 2 ? QColor(QStringLiteral("#121B25")) : QColor(QStringLiteral("#151F29"))));
        painter.setPen(QColor(QStringLiteral("#2B3A48")));
        painter.drawLine(left_margin, y + lane_height, width() - right_margin, y + lane_height);
    }

    if (document_) {
        for (int line_index = 0; line_index < static_cast<int>(cached_lines_.size()); ++line_index) {
            auto& line = cached_lines_[static_cast<std::size_t>(line_index)];
            if (line.track < 0 || line.track > 3) continue;
            const int lane_y = ruler_height + line.track * lane_height;
            const bool selected = line_index == selected_line_;
            const int block_y = lane_y + 8;
            const int block_height = std::max(18, lane_height - 16);
            painter.save();
            painter.setClipRect(QRect(left_margin, lane_y, plot_width, lane_height));

            for (auto& cue : line.cues) {
                const qint64 start_ms = cue.start_follows_playhead ? playhead_ms_ : cue.start_ms;
                qint64 end_ms = cue.end_follows_playhead ? start_ms + 1500 : cue.end_ms;
                if (end_ms <= start_ms) end_ms = start_ms + 180;
                if (end_ms < visible_start_ms || start_ms > visible_end_ms) continue;

                // Keep the line's full time-space extent. Clamping both ends to the
                // widget first makes off-screen text compress against the window edge.
                const int x1 = x_for_ms(start_ms);
                int x2 = x_for_ms(end_ms);
                if (x2 <= x1) x2 = x1 + 8;
                const QRect block(x1, block_y, std::max(8, x2 - x1), block_height);
                const QColor fill = selected ? QColor(QStringLiteral("#1D3849")) : QColor(QStringLiteral("#182633"));
                painter.setPen(QPen(selected ? QColor(QStringLiteral("#67C9EF")) : line.role_color.lighter(165), selected ? 2 : 1));
                painter.setBrush(fill);
                painter.drawRoundedRect(block, 5, 5);
                painter.fillRect(QRect(block.left() + 1, block.top() + 4, 3, block.height() - 8), line.role_color.lighter(135));

                painter.save();
                painter.setPen(QColor(QStringLiteral("#E6EFFB")));
                painter.setClipRect(block.adjusted(3, 1, -3, -1));
                QFont role_font = text_font_;
                role_font.setPixelSize(10);
                role_font.setBold(false);
                painter.setFont(role_font);
                painter.setPen(QColor(QStringLiteral("#AFC4D3")));
                painter.drawText(QRect(block.left() + 8, block.top() + 1, block.width() - 12, 13),
                                 Qt::AlignLeft | Qt::AlignVCenter, line.role);
                const QRect text_rect = block.adjusted(8, 14, -6, -3);
                QFont text_font = text_font_;
                text_font.setBold(true);
                text_font.setHintingPreference(QFont::PreferFullHinting);
                if (!cue.rows.isEmpty() && text_rect.width() > 0 && text_rect.height() > 0) {
                    // A band block represents one time interval. Never wrap its words:
                    // wrapping makes a short interval look like overlapping text.
                    const int row_count = std::max(1, static_cast<int>(cue.rows.size()));
                    const qreal row_height = text_rect.height() / static_cast<qreal>(row_count);
                    const int max_pixel_size = std::max(8, qFloor(row_height * 0.9));
                    const int pixel_size = std::clamp(
                        qRound(row_height * 0.78), 8, max_pixel_size);
                    text_font.setPixelSize(pixel_size);
                    painter.setFont(text_font);
                    painter.setClipRect(block.adjusted(4, 2, -4, -2));
                    const QFontMetricsF metrics(text_font);
                    auto widths = cue.text_widths_by_pixel_size.find(pixel_size);
                    if (widths == cue.text_widths_by_pixel_size.end()) {
                        std::vector<qreal> measured;
                        measured.reserve(static_cast<std::size_t>(cue.rows.size()));
                        for (const auto& value : cue.rows)
                            measured.push_back(std::max<qreal>(1.0, metrics.horizontalAdvance(value)));
                        widths = cue.text_widths_by_pixel_size.emplace(pixel_size, std::move(measured)).first;
                    }
                    for (int row = 0; row < cue.rows.size(); ++row) {
                        const QString& value = cue.rows[row];
                        if (value.isEmpty()) continue;
                        const qreal natural_width = widths->second[static_cast<std::size_t>(row)];
                        // Stretch each line into its own time block. Clipping is a final
                        // guard for extremely short cues; neighboring words cannot spill
                        // into an adjacent cue.
                        const qreal stretch_x = text_rect.width() / natural_width;
                        painter.save();
                        painter.translate(text_rect.left(), text_rect.top() + row * row_height +
                                          (row_height - metrics.height()) / 2.0 + metrics.ascent());
                        painter.scale(stretch_x, 1.0);
                        painter.drawText(QPointF(0, 0), value);
                        painter.restore();
                    }
                }
                painter.restore();
            }

            for (int sign_index = 0; sign_index < static_cast<int>(line.signs.size()); ++sign_index) {
                const auto& sign = line.signs[static_cast<std::size_t>(sign_index)];
                const qint64 sign_ms = sign.position_ms;
                if (sign_ms < visible_start_ms || sign_ms > visible_end_ms) continue;
                const int x = x_for_ms(sign_ms);
                painter.setPen(QPen(sign.color, selected ? 2 : 1));
                painter.drawLine(x, block_y + 1, x, block_y + block_height - 1);
            }
            if (selected) {
                painter.setPen(QPen(QColor(QStringLiteral("#EAF7FF")), 1));
                painter.setBrush(QColor(QStringLiteral("#55C5FF")));
                for (int sign_index = 0; sign_index < static_cast<int>(line.signs.size()); ++sign_index) {
                    const auto& sign = line.signs[static_cast<std::size_t>(sign_index)];
                    const int x = x_for_ms(sign.position_ms);
                    const bool active_edge = selected_edge_line_ == line_index &&
                                             selected_edge_sign_ == sign_index;
                    painter.setPen(QPen(active_edge ? QColor(QStringLiteral("#111111"))
                                                    : QColor(QStringLiteral("#EAF7FF")), 1));
                    painter.setBrush(active_edge ? QColor(QStringLiteral("#F4B860"))
                                                 : QColor(QStringLiteral("#55C5FF")));
                    painter.drawRoundedRect(QRect(x - 4, block_y + block_height / 2 - 9, 8, 18), 2, 2);
                }
            }
            painter.restore();
        }

        // Loop and shot markers stay in the time scale and travel with the bands.
        for (const auto& marker : cached_markers_) {
            const qint64 marker_ms = marker.position_ms;
            if (marker_ms < visible_start_ms || marker_ms > visible_end_ms) continue;
            const int x = x_for_ms(marker_ms);
            painter.setPen(QPen(marker.loop ? QColor(QStringLiteral("#A79AF0"))
                                             : QColor(QStringLiteral("#65C9EF")), 1, Qt::DashLine));
            painter.drawLine(x, ruler_height + 2, x, height() - 2);
        }
    }

    // Paint the fixed track-label column last, above the complete cue artwork.
    // This masks both the cue frame and its text if a long glyph or a transformed
    // text row reaches the lane-name area, while leaving the timeline content intact.
    for (int track = 0; track < 4; ++track) {
        const int y = ruler_height + track * lane_height;
        const bool selected = track == selected_track_;
        painter.fillRect(QRect(0, y, left_margin, lane_height), selected
            ? QColor(QStringLiteral("#1A3140"))
            : (track % 2 ? QColor(QStringLiteral("#121B25")) : QColor(QStringLiteral("#151F29"))));
        painter.setPen(selected ? QColor(QStringLiteral("#6ECFF2")) : QColor(QStringLiteral("#A8B7C5")));
        painter.drawText(QRect(10, y, left_margin - 18, lane_height), Qt::AlignVCenter | Qt::AlignLeft,
                         QStringLiteral("PISTE %1").arg(track + 1));
    }

    // The playhead is deliberately stationary; only the time-based DetX artwork scrolls.
    painter.setPen(QPen(QColor(QStringLiteral("#FF5364")), 2));
    painter.drawLine(cursor_x, ruler_height - 1, cursor_x, height());
    QPolygon cursor{QPoint(cursor_x - 6, ruler_height - 1), QPoint(cursor_x + 6, ruler_height - 1),
                    QPoint(cursor_x, ruler_height + 7)};
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#FF5364")));
    painter.drawPolygon(cursor);

    if (!clean_presentation_) {
        painter.setPen(QColor(QStringLiteral("#A9B8C6")));
        painter.drawText(QRect(left_margin, height() - 17, width() - left_margin - right_margin, 15),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 i/s · Molette à crans / + / − : zoom · Trackpad : navigation · Alt ou Maj + ← / → : image par image · Entrée : nouvelle réplique")
                             .arg(media_fps_, 0, 'f', 3));
    }
}

int RhythmTimelineWidget::lineAtPosition(int x, int y) const {
    if (!document_ || y < ruler_height || y >= height()) return -1;
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    const auto& lines = document_->lines();
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
        const auto& line = lines[static_cast<std::size_t>(i)];
        if (line.track != track) continue;
        const auto contains_interval = [this, x](qint64 start, qint64 end) {
            if (end <= start) end = start + 180;
            const int x1 = xForPosition(start);
            int x2 = xForPosition(end);
            if (x2 <= x1) x2 = x1 + 8;
            return x >= x1 && x <= x2;
        };
        for (const auto& segment : line.text_segments) {
            if (segment.text.empty()) continue;
            const qint64 start = segment.start ? positionForTimecode(*segment.start)
                : (!line.lipsyncs.empty() ? positionForTimecode(line.lipsyncs.front().timecode) : playhead_ms_);
            const qint64 end = segment.end ? positionForTimecode(*segment.end)
                : (!line.lipsyncs.empty() ? positionForTimecode(line.lipsyncs.back().timecode) : start + 1500);
            if (contains_interval(std::min(start, end), std::max(start, end))) return i;
        }
        if (line.text_segments.empty() && !line.text.empty()) {
            const qint64 start = line.lipsyncs.empty() ? playhead_ms_
                : positionForTimecode(line.lipsyncs.front().timecode);
            const qint64 end = line.lipsyncs.size() < 2 ? start + 1500
                : positionForTimecode(line.lipsyncs.back().timecode);
            if (contains_interval(std::min(start, end), std::max(start, end))) return i;
        }
    }
    return -1;
}

std::pair<int, int> RhythmTimelineWidget::edgeAtPosition(int x, int y) const {
    if (!document_ || y < ruler_height || y >= height()) return {-1, -1};
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    const auto& lines = document_->lines();
    int best_line = -1;
    int best_sign = -1;
    int best_distance = 10;
    const auto consider = [&](int line_index, const Line& line, const Timecode& edge) {
        for (int sign_index = 0; sign_index < static_cast<int>(line.lipsyncs.size()); ++sign_index) {
            if (line.lipsyncs[static_cast<std::size_t>(sign_index)].timecode != edge) continue;
            const int distance = std::abs(x - xForPosition(positionForTimecode(edge)));
            if (distance < best_distance) {
                best_distance = distance;
                best_line = line_index;
                best_sign = sign_index;
            }
        }
    };
    // Lines painted later are visually on top, so test them first and retain the
    // first match on equal distances. Only actual text-block boundaries resize.
    for (int line_index = static_cast<int>(lines.size()) - 1; line_index >= 0; --line_index) {
        const auto& line = lines[static_cast<std::size_t>(line_index)];
        if (line.track != track) continue;
        if (!line.text_segments.empty()) {
            for (const auto& segment : line.text_segments) {
                if (segment.text.empty()) continue;
                if (segment.start) consider(line_index, line, *segment.start);
                if (segment.end) consider(line_index, line, *segment.end);
            }
        } else if (!line.lipsyncs.empty()) {
            consider(line_index, line, line.lipsyncs.front().timecode);
            consider(line_index, line, line.lipsyncs.back().timecode);
        }
    }
    return {best_line, best_sign};
}

void RhythmTimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || width() <= left_margin + right_margin) return;
    setFocus(Qt::MouseFocusReason);
    const int y = static_cast<int>(event->position().y());
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));
    // Capture the playhead by its handle; during the drag the ruler and text
    // stay under the pointer instead of scrolling beneath a fixed red line.
    if (playhead_handler_ && y >= ruler_height - 12 && y <= ruler_height + 10 &&
        std::abs(static_cast<int>(event->position().x()) - cursor_x) <= 12) {
        scrubbing_playhead_ = true;
        scrub_start_x_ = cursor_x;
        scrub_start_position_ms_ = playhead_ms_;
        setCursor(Qt::SizeHorCursor);
        if (playhead_drag_handler_) playhead_drag_handler_(true);
        playhead_handler_(scrub_start_position_ms_);
        event->accept();
        return;
    }
    if (y < ruler_height || y >= height()) return;
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    const int clicked_x = static_cast<int>(event->position().x());
    selected_track_ = track;
    const auto edge = edgeAtPosition(clicked_x, y);
    selected_line_ = edge.first >= 0 ? edge.first : lineAtPosition(clicked_x, y);
    selected_edge_line_ = edge.first;
    selected_edge_sign_ = edge.second;
    if (edge.first >= 0 && resize_handler_) {
        resizing_line_ = edge.first;
        resizing_sign_ = edge.second;
        resize_start_x_ = clicked_x;
        resize_start_position_ms_ = positionForTimecode(document_->lines()[static_cast<std::size_t>(edge.first)]
            .lipsyncs[static_cast<std::size_t>(edge.second)].timecode);
        if (resize_transaction_handler_) resize_transaction_handler_(true);
        setCursor(Qt::SizeHorCursor);
    }
    update();
    if (selection_handler_) selection_handler_(selected_line_, selected_track_);
    event->accept();
}

void RhythmTimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && edit_handler_) {
        const int line_index = lineAtPosition(static_cast<int>(event->position().x()),
                                              static_cast<int>(event->position().y()));
        if (line_index >= 0) {
            selected_line_ = line_index;
            const int lane_height = std::max(1, (height() - ruler_height) / 4);
            selected_track_ = std::clamp((static_cast<int>(event->position().y()) - ruler_height) / lane_height, 0, 3);
            if (selection_handler_) selection_handler_(selected_line_, selected_track_);
            edit_handler_(line_index);
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void RhythmTimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (scrubbing_playhead_ && playhead_handler_) {
        const int plot_width = std::max(1, width() - left_margin - right_margin);
        const int pointer_x = std::clamp(static_cast<int>(event->position().x()), left_margin, width() - right_margin);
        const qint64 delta_ms = qRound64(static_cast<double>(pointer_x - scrub_start_x_) *
                                         visible_duration_ms_ / plot_width);
        playhead_handler_(scrub_start_position_ms_ + delta_ms);
        event->accept();
        return;
    }
    if (resizing_line_ >= 0 && document_ && resize_handler_ &&
        resizing_line_ < static_cast<int>(document_->lines().size())) {
        const auto& line = document_->lines()[static_cast<std::size_t>(resizing_line_)];
        if (resizing_sign_ >= 0 && resizing_sign_ < static_cast<int>(line.lipsyncs.size())) {
            const auto& signs = line.lipsyncs;
            const int pointer_x = std::clamp(static_cast<int>(event->position().x()), left_margin, width() - right_margin);
            const int delta_x = pointer_x - resize_start_x_;
            if (std::abs(delta_x) < 2) { event->accept(); return; }
            qint64 target = resize_start_position_ms_ + qRound64(
                static_cast<double>(delta_x) * visible_duration_ms_ /
                std::max(1, width() - left_margin - right_margin));
            const qint64 min_span = std::max<qint64>(1, qRound64(1000.0 / timecode_fps_));
            qint64 lower = std::numeric_limits<qint64>::min();
            qint64 upper = std::numeric_limits<qint64>::max();
            if (resizing_sign_ > 0) {
                const auto previous = positionForTimecode(signs[static_cast<std::size_t>(resizing_sign_ - 1)].timecode);
                lower = previous + min_span;
            }
            if (resizing_sign_ + 1 < static_cast<int>(signs.size())) {
                const auto next = positionForTimecode(signs[static_cast<std::size_t>(resizing_sign_ + 1)].timecode);
                upper = next - min_span;
            }
            if (lower <= upper) {
                if (lower != std::numeric_limits<qint64>::min()) target = std::max(target, lower);
                if (upper != std::numeric_limits<qint64>::max()) target = std::min(target, upper);
                resize_handler_(resizing_line_, resizing_sign_, target);
            }
        }
        event->accept();
        return;
    }
    if (document_) {
        const int x = static_cast<int>(event->position().x());
        const int y = static_cast<int>(event->position().y());
        const int plot_width = std::max(1, width() - left_margin - right_margin);
        const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));
        const bool near_playhead = playhead_handler_ && y >= ruler_height - 12 &&
            y <= ruler_height + 10 && std::abs(x - cursor_x) <= 12;
        const bool near_edge = edgeAtPosition(x, y).first >= 0;
        setCursor(near_edge || near_playhead ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void RhythmTimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && scrubbing_playhead_) {
        scrubbing_playhead_ = false;
        if (playhead_drag_handler_) playhead_drag_handler_(false);
        update();
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && resizing_line_ >= 0) {
        resizing_line_ = -1;
        resizing_sign_ = -1;
        if (resize_transaction_handler_) resize_transaction_handler_(false);
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void RhythmTimelineWidget::navigateToAdjacentCue(int direction) {
    if (direction == 0 || !playhead_handler_) return;
    std::vector<qint64> targets;
    for (const auto& line : cached_lines_) {
        for (const auto& cue : line.cues)
            if (!cue.start_follows_playhead) targets.push_back(cue.start_ms);
    }
    for (const auto& marker : cached_markers_)
        if (!marker.loop) targets.push_back(marker.position_ms);
    if (targets.empty()) return;

    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    const qint64 tolerance = std::max<qint64>(1, qRound64(500.0 / std::max(1.0, media_fps_)));
    if (direction > 0) {
        const auto next = std::upper_bound(targets.begin(), targets.end(), playhead_ms_ + tolerance);
        if (next != targets.end()) playhead_handler_(*next);
    } else {
        const auto previous = std::lower_bound(targets.begin(), targets.end(), playhead_ms_ - tolerance);
        if (previous != targets.begin()) playhead_handler_(*std::prev(previous));
    }
}

void RhythmTimelineWidget::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        navigateToAdjacentCue(event->key() == Qt::Key_Right ? 1 : -1);
        event->accept();
        return;
    }
    if ((event->modifiers().testFlag(Qt::ShiftModifier) || event->modifiers().testFlag(Qt::AltModifier)) &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        stepTimelineFrame(event->key() == Qt::Key_Right ? 1 : -1);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2 && selected_line_ >= 0 && edit_handler_) {
        edit_handler_(selected_line_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete && selected_line_ >= 0 && delete_handler_) {
        delete_handler_(selected_line_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Plus ||
        (event->key() == Qt::Key_Equal && event->modifiers().testFlag(Qt::ShiftModifier))) {
        adjustTimelineZoom(1.12);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore) {
        adjustTimelineZoom(1.0 / 1.12);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && insert_at_handler_) {
        insert_at_handler_(selected_track_, playhead_ms_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_K && event->modifiers() == Qt::ControlModifier &&
        selected_line_ >= 0 && split_handler_) {
        split_handler_(selected_line_);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RhythmTimelineWidget::wheelEvent(QWheelEvent* event) {
    const QPoint pixel_delta = event->pixelDelta();
    if (!pixel_delta.isNull() || event->phase() != Qt::NoScrollPhase) {
        // High-resolution pixel deltas are produced by trackpads. Move the time
        // under the fixed cue line; only discrete wheel detents change zoom.
        const QPoint angle_delta = event->angleDelta();
        const int horizontal_delta = pixel_delta.x() != 0 ? pixel_delta.x() : angle_delta.x() / 8;
        const int delta = horizontal_delta != 0 ? horizontal_delta
            : (pixel_delta.y() != 0 ? -pixel_delta.y() : -angle_delta.y() / 8);
        if (delta != 0) panTimelineByPixels(delta);
        event->accept();
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta != 0) {
        adjustTimelineZoom(std::pow(1.12, static_cast<qreal>(delta) / 120.0));
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void RhythmTimelineWidget::adjustTimelineZoom(qreal factor) {
    if (!std::isfinite(factor) || factor <= 0.0) return;
    const qint64 duration = std::clamp<qint64>(
        qRound64(visible_duration_ms_ / factor), 500, 120000);
    if (duration == visible_duration_ms_) return;
    visible_duration_ms_ = duration;
    update();
}

void RhythmTimelineWidget::panTimelineByPixels(int delta) {
    if (!playhead_handler_ || delta == 0) return;
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const qint64 delta_ms = qRound64(static_cast<double>(delta) * visible_duration_ms_ / plot_width);
    if (delta_ms != 0) playhead_handler_(std::max<qint64>(0, playhead_ms_ - delta_ms));
}

void RhythmTimelineWidget::stepTimelineFrame(int direction) {
    if (direction == 0) return;
    if (selected_edge_line_ >= 0 && selected_edge_line_ == selected_line_ && document_ && resize_handler_ &&
        selected_edge_line_ < static_cast<int>(document_->lines().size())) {
        const auto& line = document_->lines()[static_cast<std::size_t>(selected_edge_line_)];
        if (selected_edge_sign_ >= 0 && selected_edge_sign_ < static_cast<int>(line.lipsyncs.size())) {
            const auto& signs = line.lipsyncs;
            const auto& boundary = signs[static_cast<std::size_t>(selected_edge_sign_)];
            const auto current_frame = boundary.timecode.to_frames(timecode_fps_);
            const auto target_timecode = Timecode::from_frames(
                std::max<std::int64_t>(0, current_frame + direction), timecode_fps_);
            const qint64 target_ms = positionForTimecode(target_timecode);
            const qint64 min_span = std::max<qint64>(1, qRound64(1000.0 / timecode_fps_));
            qint64 lower = std::numeric_limits<qint64>::min();
            qint64 upper = std::numeric_limits<qint64>::max();
            if (selected_edge_sign_ > 0) {
                lower = positionForTimecode(signs[static_cast<std::size_t>(selected_edge_sign_ - 1)].timecode) + min_span;
            }
            if (selected_edge_sign_ + 1 < static_cast<int>(signs.size())) {
                upper = positionForTimecode(signs[static_cast<std::size_t>(selected_edge_sign_ + 1)].timecode) - min_span;
            }
            if (lower <= target_ms && target_ms <= upper)
                resize_handler_(selected_edge_line_, selected_edge_sign_, target_ms);
        }
        return;
    }
    if (!playhead_handler_) return;
    const qint64 fps_milli = std::max<qint64>(1, qRound64(media_fps_ * 1000.0));
    const qint64 current_frame = static_cast<qint64>(std::llround(
        static_cast<long double>(playhead_ms_) * fps_milli / 1000000.0L));
    const qint64 target_ms = static_cast<qint64>(std::llround(
        static_cast<long double>(current_frame + direction) * 1000000.0L / fps_milli));
    playhead_handler_(std::max<qint64>(0, target_ms));
}

void RhythmTimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int x = event->pos().x();
    const int y = event->pos().y();
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    if (y >= ruler_height && y < height()) {
        selected_track_ = std::clamp((y - ruler_height) / lane_height, 0, 3);
        selected_line_ = lineAtPosition(x, y);
        selected_edge_line_ = -1;
        selected_edge_sign_ = -1;
        if (selection_handler_) selection_handler_(selected_line_, selected_track_);
    }
    QMenu menu(this);
    if (selected_line_ >= 0 && edit_handler_) {
        auto* edit = menu.addAction(QStringLiteral("Modifier la réplique…"));
        connect(edit, &QAction::triggered, this, [this] { edit_handler_(selected_line_); });
        if (delete_handler_) {
            auto* remove = menu.addAction(QStringLiteral("Supprimer la réplique"));
            connect(remove, &QAction::triggered, this, [this] { delete_handler_(selected_line_); });
        }
    } else if (y >= ruler_height && y < height() && insert_at_handler_) {
        auto* insert = menu.addAction(QStringLiteral("Ajouter une réplique ici"));
        const int track = selected_track_;
        const qint64 position = positionForX(x);
        connect(insert, &QAction::triggered, this, [this, track, position] {
            insert_at_handler_(track, position);
        });
    }
    if (!menu.isEmpty()) menu.exec(event->globalPos());
}

} // namespace rythmo
