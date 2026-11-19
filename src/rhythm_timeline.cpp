#include "rythmo/rhythm_timeline.hpp"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QStringList>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>

namespace rythmo {
namespace {
constexpr int ruler_height = 34;
constexpr int left_margin = 88;
constexpr int right_margin = 16;
constexpr double cursor_fraction = 0.34;

QColor ensure_legible_role_color(QColor color) {
    if (!color.isValid() || color == QColor(Qt::black)) return QColor(QStringLiteral("#60A5FA"));
    float h = 0.0f, s = 0.0f, l = 0.0f, a = 1.0f;
    color.getHslF(&h, &s, &l, &a);
    if (l < 0.45f) {
        s = std::max(s, 0.40f);
        l = 0.58f;
        return QColor::fromHslF(h < 0.0f ? 0.58f : h, s, l, a);
    }
    return color;
}

QColor blend_color(const QColor& base, const QColor& tint, qreal alpha) {
    const qreal clamped = std::clamp(alpha, 0.0, 1.0);
    return QColor(
        qRound(base.red() * (1.0 - clamped) + tint.red() * clamped),
        qRound(base.green() * (1.0 - clamped) + tint.green() * clamped),
        qRound(base.blue() * (1.0 - clamped) + tint.blue() * clamped));
}

QColor role_color(const DetxDocument* document, const std::string& role_id) {
    if (document) {
        for (const auto& role : document->roles()) {
            if (role.id != role_id) continue;
            const QColor parsed(QString::fromUtf8(role.color.c_str()));
            if (parsed.isValid() && parsed != QColor(Qt::black)) return ensure_legible_role_color(parsed);
            break;
        }
    }
    return QColor(QStringLiteral("#60A5FA"));
}

QString role_name(const DetxDocument* document, const std::string& role_id) {
    if (document) {
        for (const auto& role : document->roles()) {
            if (role.id == role_id) return QString::fromUtf8(role.name.c_str());
        }
    }
    return role_id.empty() ? QStringLiteral("Sans rôle") : QString::fromUtf8(role_id.c_str());
}

qint64 choose_ruler_major_tick_ms(qint64 visible_duration_ms, int plot_width) {
    const double min_ms = static_cast<double>(std::max<qint64>(1, visible_duration_ms)) * 124.0 /
                          static_cast<double>(std::max(1, plot_width));
    constexpr std::array<qint64, 11> steps{
        100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000};
    for (const qint64 step : steps) {
        if (static_cast<double>(step) >= min_ms) return step;
    }
    return steps.back();
}

void format_tc_chars(const Timecode& tc, QChar out[11]) {
    const int h = std::clamp(tc.hours, 0, 99);
    const int m = std::clamp(tc.minutes, 0, 59);
    const int s = std::clamp(tc.seconds, 0, 59);
    const int f = std::clamp(tc.frames, 0, 99);
    out[0] = QLatin1Char('0' + (h / 10));
    out[1] = QLatin1Char('0' + (h % 10));
    out[2] = QLatin1Char(':');
    out[3] = QLatin1Char('0' + (m / 10));
    out[4] = QLatin1Char('0' + (m % 10));
    out[5] = QLatin1Char(':');
    out[6] = QLatin1Char('0' + (s / 10));
    out[7] = QLatin1Char('0' + (s % 10));
    out[8] = QLatin1Char(':');
    out[9] = QLatin1Char('0' + (f / 10));
    out[10] = QLatin1Char('0' + (f % 10));
}

bool is_word_char(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('\'') || c == QChar(0x2019);
}

int prev_word_boundary(const QString& text, int pos) {
    int i = std::clamp(pos, 0, static_cast<int>(text.size()));
    while (i > 0 && !is_word_char(text[i - 1])) --i;
    while (i > 0 && is_word_char(text[i - 1])) --i;
    return i;
}

int next_word_boundary(const QString& text, int pos) {
    const int n = static_cast<int>(text.size());
    int i = std::clamp(pos, 0, n);
    while (i < n && is_word_char(text[i])) ++i;
    while (i < n && !is_word_char(text[i])) ++i;
    return i;
}

std::shared_ptr<QTextLayout> build_shaped_line(const QString& text, const QFont& font) {
    auto tl = std::make_shared<QTextLayout>(text, font);
    tl->setCacheEnabled(true);
    QTextOption opt;
    opt.setWrapMode(QTextOption::NoWrap);
    tl->setTextOption(opt);
    tl->beginLayout();
    QTextLine line = tl->createLine();
    if (line.isValid()) {
        line.setPosition(QPointF(0.0, 0.0));
    }
    tl->endLayout();
    return tl;
}

} // namespace

RhythmTimelineWidget::RhythmTimelineWidget(QWidget* parent)
    : QWidget(parent),
      text_font_(font()),
      ruler_font_(QFontDatabase::systemFont(QFontDatabase::FixedFont)),
      role_font_(font()),
      track_font_(font()) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    text_font_.setBold(true);
    text_font_.setHintingPreference(QFont::PreferNoHinting);

    ruler_font_.setStyleHint(QFont::Monospace);
    ruler_font_.setPixelSize(11);

    role_font_.setPixelSize(12);
    role_font_.setWeight(QFont::DemiBold);

    track_font_.setPixelSize(12);
    track_font_.setWeight(QFont::DemiBold);

    caret_blink_timer_ = new QTimer(this);
    caret_blink_timer_->setInterval(520);
    connect(caret_blink_timer_, &QTimer::timeout, this, [this] {
        if (editing_line_ < 0) {
            caret_blink_timer_->stop();
            return;
        }
        caret_visible_ = !caret_visible_;
        updateEditingLane();
    });

    wheel_scrub_end_timer_ = new QTimer(this);
    wheel_scrub_end_timer_->setSingleShot(true);
    wheel_scrub_end_timer_->setInterval(150);
    connect(wheel_scrub_end_timer_, &QTimer::timeout, this, [this] {
        if (!wheel_scrub_active_) return;
        wheel_scrub_active_ = false;
        trackpad_pan_accum_ms_ = 0.0;
        if (playhead_drag_handler_) playhead_drag_handler_(false);
    });
}

void RhythmTimelineWidget::setDocument(const DetxDocument* document, Timecode video_start,
                                       int timecode_fps, double media_fps, qint64 duration_ms) {
    if (editing_line_ >= 0 && document != document_) {
        finishInlineEdit(false);
    }
    const bool ruler_basis_changed = !(video_start_ == video_start) ||
                                     (timecode_fps_ != std::max(1, timecode_fps));
    document_ = document;
    video_start_ = video_start;
    timecode_fps_ = std::max(1, timecode_fps);
    media_fps_ = media_fps > 0.0 ? media_fps : static_cast<double>(timecode_fps_);
    duration_ms_ = std::max<qint64>(0, duration_ms);
    if (ruler_basis_changed) ruler_label_cache_.clear();
    if (!document_ || selected_line_ >= static_cast<int>(document_->lines().size())) selected_line_ = -1;
    hovered_line_ = -1;
    hovered_edge_line_ = -1;
    hovered_edge_sign_ = -1;
    rebuildSceneCache();
    update();
}

RhythmTimelineWidget::CachedLine RhythmTimelineWidget::buildCachedLine(const Line& line) const {
    CachedLine cached;
    cached.track = line.track;
    cached.role = role_name(document_, line.role_id);
    cached.role_color = role_color(document_, line.role_id);
    cached.min_ms = std::numeric_limits<qint64>::max();
    cached.max_ms = std::numeric_limits<qint64>::min();

    auto add_cue = [&](const std::string& source_text, std::optional<Timecode> start_tc,
                       std::optional<Timecode> end_tc, std::size_t first_segment,
                       std::size_t segment_count, bool allow_empty) {
        const QString full = QString::fromUtf8(source_text.data(), static_cast<qsizetype>(source_text.size())).trimmed();
        if (full.isEmpty() && !allow_empty) return;
        if (!start_tc && !line.lipsyncs.empty()) start_tc = line.lipsyncs.front().timecode;
        if (!end_tc && !line.lipsyncs.empty()) end_tc = line.lipsyncs.back().timecode;

        CachedCue cue;
        cue.start_follows_playhead = !start_tc.has_value();
        cue.end_follows_playhead = !end_tc.has_value();
        cue.start_ms = start_tc ? positionForTimecode(*start_tc) : 0;
        cue.end_ms = end_tc ? positionForTimecode(*end_tc) : 0;
        if (!cue.start_follows_playhead && !cue.end_follows_playhead && cue.end_ms <= cue.start_ms) {
            cue.end_ms = cue.start_ms + 180;
        }
        cue.text = full;
        cue.first_segment = first_segment;
        cue.segment_count = segment_count;
        cue.rows = full.isEmpty() ? QStringList{QString{}} : full.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

        if (cue.start_follows_playhead || cue.end_follows_playhead) {
            cached.dynamic_bounds = true;
        } else {
            cached.min_ms = std::min(cached.min_ms, cue.start_ms);
            cached.max_ms = std::max(cached.max_ms, cue.end_ms);
        }
        cached.cues.push_back(std::move(cue));
    };

    if (!line.text_segments.empty()) {
        const bool single_segment = line.text_segments.size() == 1;
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
            add_cue(phrase, first.start, first.end, i, next - i, single_segment);
            i = next;
        }
        if (cached.cues.empty() && !line.lipsyncs.empty()) {
            add_cue(line.text, line.lipsyncs.front().timecode, line.lipsyncs.back().timecode,
                    0, line.text_segments.size(), true);
        }
    } else {
        const std::optional<Timecode> start = line.lipsyncs.empty()
            ? std::nullopt : std::optional<Timecode>(line.lipsyncs.front().timecode);
        const std::optional<Timecode> end = line.lipsyncs.empty()
            ? std::nullopt : std::optional<Timecode>(line.lipsyncs.back().timecode);
        add_cue(line.text, start, end, 0, 0, !line.lipsyncs.empty());
    }

    cached.signs.reserve(line.lipsyncs.size());
    for (const auto& sign : line.lipsyncs) {
        const QColor color = sign.kind.rfind("in_", 0) == 0 ? QColor(QStringLiteral("#34D399"))
            : (sign.kind.rfind("out_", 0) == 0 ? QColor(QStringLiteral("#F59E0B"))
                                                : QColor(QStringLiteral("#60A5FA")));
        const qint64 pos = positionForTimecode(sign.timecode);
        cached.min_ms = std::min(cached.min_ms, pos);
        cached.max_ms = std::max(cached.max_ms, pos);
        cached.signs.push_back({pos, color});
    }

    if (cached.min_ms > cached.max_ms) {
        cached.min_ms = 0;
        cached.max_ms = 0;
    }
    return cached;
}

void RhythmTimelineWidget::rebuildNavTargets() {
    sorted_nav_targets_.clear();
    sorted_nav_targets_.reserve(cached_lines_.size() + cached_markers_.size());
    for (const auto& line : cached_lines_) {
        for (const auto& cue : line.cues) {
            if (!cue.start_follows_playhead) sorted_nav_targets_.push_back(cue.start_ms);
        }
    }
    for (const auto& marker : cached_markers_) {
        if (!marker.loop) sorted_nav_targets_.push_back(marker.position_ms);
    }
    std::sort(sorted_nav_targets_.begin(), sorted_nav_targets_.end());
    sorted_nav_targets_.erase(
        std::unique(sorted_nav_targets_.begin(), sorted_nav_targets_.end()),
        sorted_nav_targets_.end());
}

void RhythmTimelineWidget::evictDistantPixmaps(qint64 visible_start_ms, qint64 visible_end_ms) {
    // Keep total VRAM/RAM footprint strictly < ~10 MB on low-end 4GB/8GB machines!
    if (active_pixmap_cues_ <= 64) return;
    const qint64 keep_start = visible_start_ms - std::max<qint64>(20000, visible_duration_ms_ * 3);
    const qint64 keep_end = visible_end_ms + std::max<qint64>(20000, visible_duration_ms_ * 3);
    int remaining = 0;
    for (auto& line : cached_lines_) {
        for (auto& cue : line.cues) {
            if (cue.cached_content_pixmap.isNull()) continue;
            if (!line.dynamic_bounds && (cue.end_ms < keep_start || cue.start_ms > keep_end)) {
                cue.cached_content_pixmap = QPixmap{};
            } else {
                ++remaining;
            }
        }
    }
    active_pixmap_cues_ = remaining;
}

void RhythmTimelineWidget::updateLineCache(int line_index) {
    if (!document_ || line_index < 0 || line_index >= static_cast<int>(document_->lines().size())) return;
    if (cached_lines_.size() != document_->lines().size()) {
        rebuildSceneCache();
    } else {
        auto& old_line = cached_lines_[static_cast<std::size_t>(line_index)];
        for (const auto& c : old_line.cues) {
            if (!c.cached_content_pixmap.isNull() && active_pixmap_cues_ > 0) --active_pixmap_cues_;
        }
        old_line = buildCachedLine(document_->lines()[static_cast<std::size_t>(line_index)]);
        rebuildNavTargets();
    }
    update();
}

void RhythmTimelineWidget::rebuildSceneCache() {
    cached_lines_.clear();
    cached_markers_.clear();
    sorted_nav_targets_.clear();
    active_pixmap_cues_ = 0;
    if (!document_) return;

    const auto& lines = document_->lines();
    cached_lines_.reserve(lines.size());
    for (const auto& line : lines) {
        cached_lines_.push_back(buildCachedLine(line));
    }

    cached_markers_.reserve(document_->markers().size());
    for (const auto& marker : document_->markers()) {
        cached_markers_.push_back({positionForTimecode(marker.timecode), marker.kind == "loop"});
    }
    rebuildNavTargets();
}

void RhythmTimelineWidget::setPlayhead(qint64 position_ms) {
    const qint64 clamped = std::max<qint64>(0, position_ms);
    if (clamped == playhead_ms_) return;
    playhead_ms_ = clamped;
    update();
}

void RhythmTimelineWidget::setPlayheadForRender(qint64 position_ms) {
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

void RhythmTimelineWidget::setZoomChangedHandler(std::function<void(qint64)> handler) {
    zoom_changed_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setTextFont(const QFont& font) {
    text_font_ = font;
    text_font_.setBold(true);
    text_font_.setHintingPreference(QFont::PreferNoHinting);
    role_font_ = font;
    role_font_.setPixelSize(12);
    role_font_.setWeight(QFont::DemiBold);
    for (auto& line : cached_lines_) {
        for (auto& cue : line.cues) {
            cue.cached_natural_px = -1;
            cue.shaped_rows.clear();
            cue.cached_content_pixmap = QPixmap{};
        }
    }
    active_pixmap_cues_ = 0;
    update();
}

void RhythmTimelineWidget::setCleanPresentation(bool enabled) {
    if (enabled && editing_line_ >= 0) finishInlineEdit(true);
    if (clean_presentation_ == enabled) return;
    clean_presentation_ = enabled;
    cached_sidebar_pixmap_ = QPixmap{};
    update();
}

void RhythmTimelineWidget::setSplitHandler(std::function<void(int)> handler) {
    split_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setEditHandler(std::function<void(int, int)> handler) {
    edit_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setTextEditHandler(
    std::function<void(int, std::size_t, std::size_t, const QString&)> handler) {
    text_edit_handler_ = std::move(handler);
}

QRectF RhythmTimelineWidget::cueBlockRectF(const CachedCue& cue, int track) const {
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const qreal px_per_ms = static_cast<qreal>(plot_width) / static_cast<qreal>(std::max<qint64>(1, visible_duration_ms_));
    const qreal visible_start_ms = static_cast<qreal>(playhead_ms_) - visible_duration_ms_ * cursor_fraction;

    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int lane_y = ruler_height + std::clamp(track, 0, 3) * lane_height;
    const int block_y = lane_y + 4;
    const int block_height = std::max(22, lane_height - 8);

    const qint64 start_ms = cue.start_follows_playhead ? playhead_ms_ : cue.start_ms;
    qint64 end_ms = cue.end_follows_playhead ? start_ms + 1500 : cue.end_ms;
    if (end_ms <= start_ms) end_ms = start_ms + 180;

    const qreal x1 = left_margin + (static_cast<qreal>(start_ms) - visible_start_ms) * px_per_ms;
    const int block_w = std::max(10, qRound(static_cast<qreal>(end_ms - start_ms) * px_per_ms));
    return QRectF(x1, block_y, block_w, block_height);
}

QRectF RhythmTimelineWidget::cueTextRectF(const QRectF& block) const {
    const qreal role_band = block.height() >= 42.0 ? 16.0 : 12.0;
    return block.adjusted(10.0, role_band, -10.0, -4.0);
}

int RhythmTimelineWidget::uniformPixelSize(int block_height, int row_count) const {
    const int role_band = block_height >= 42 ? 16 : 12;
    const int single_h = std::max(8, block_height - role_band - 4);
    // Strict constant vertical font size across ALL cues on the band!
    const int uniform_px = std::clamp(qRound(single_h * 0.54), 12, 40);
    if (row_count <= 1) return uniform_px;
    const int multi_max = std::max(11, static_cast<int>(std::floor(single_h * 0.84 / row_count)));
    return std::min(uniform_px, multi_max);
}

void RhythmTimelineWidget::ensureShapedMetrics(CachedCue& cue, int pixel_size) {
    if (cue.cached_natural_px == pixel_size &&
        cue.shaped_rows.size() == static_cast<std::size_t>(cue.rows.size())) {
        return;
    }

    QFont base_font = text_font_;
    base_font.setBold(true);
    base_font.setHintingPreference(QFont::PreferNoHinting);
    base_font.setPixelSize(pixel_size);
    base_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    base_font.setWordSpacing(0.0);

    const QFontMetricsF fm(base_font);
    cue.cached_natural_px = pixel_size;
    cue.cached_ascent = fm.ascent();
    cue.cached_font_height = fm.height();
    cue.shaped_rows.clear();
    cue.shaped_rows.reserve(static_cast<std::size_t>(cue.rows.size()));

    for (const QString& row_str : cue.rows) {
        CachedShapedRow sr;
        sr.line_height = fm.height();
        if (!row_str.isEmpty()) {
            sr.layout = build_shaped_line(row_str, base_font);
            const QTextLine line = sr.layout->lineAt(0);
            if (line.isValid()) {
                sr.line_height = line.height();
                const qreal adv = line.naturalTextWidth();
                const qreal right_edge = line.naturalTextRect().right();
                sr.natural_width = std::max<qreal>({1.0, adv, right_edge}) + 1.5;
            }
        }
        cue.shaped_rows.push_back(std::move(sr));
    }
}

RhythmTimelineWidget::CueRowStretch RhythmTimelineWidget::computeRowStretch(
    CachedCue& cue, int row, int block_height, int row_count, qreal avail_width, bool is_editing) {
    CueRowStretch result;
    const int px = uniformPixelSize(block_height, row_count);
    ensureShapedMetrics(cue, px);

    result.pixel_size = px;
    result.line_height = cue.cached_font_height;
    result.stretch_x = 1.0;

    if (row < 0 || row >= static_cast<int>(cue.shaped_rows.size())) {
        return result;
    }

    const auto& sr = cue.shaped_rows[static_cast<std::size_t>(row)];
    result.line_height = sr.line_height;
    result.layout = sr.layout.get();
    if (!sr.layout || cue.rows[row].isEmpty()) {
        return result;
    }

    // Stretch or condense letters horizontally to match the cue's duration at constant font height!
    const qreal safe_avail = std::max<qreal>(6.0, avail_width);
    const qreal raw_stretch = safe_avail / std::max<qreal>(1.0, sr.natural_width);
    const qreal max_stretch = is_editing ? 1.45 : 2.35;
    result.stretch_x = std::clamp<qreal>(raw_stretch, 0.12, max_stretch);
    return result;
}

int RhythmTimelineWidget::hitTestEditCursor(int x, int y) {
    if (editing_line_ < 0 || editing_line_ >= static_cast<int>(cached_lines_.size())) return 0;
    auto& line = cached_lines_[static_cast<std::size_t>(editing_line_)];
    if (editing_cue_ < 0 || editing_cue_ >= static_cast<int>(line.cues.size())) return 0;
    auto& cue = line.cues[static_cast<std::size_t>(editing_cue_)];

    const QRectF block = cueBlockRectF(cue, editing_track_);
    const QRectF text_rect = cueTextRectF(block);
    const QStringList rows = edit_text_.isEmpty()
        ? QStringList{QString{}} : edit_text_.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const int row_count = std::max(1, static_cast<int>(rows.size()));
    const qreal row_height = std::max<qreal>(1.0, text_rect.height() / static_cast<qreal>(row_count));
    const int target_row = std::clamp(static_cast<int>((y - text_rect.top()) / row_height), 0, row_count - 1);

    int row_char_start = 0;
    for (int r = 0; r < target_row; ++r) {
        row_char_start += static_cast<int>(rows[r].size()) + 1;
    }

    const QString& row_str = rows[target_row];
    if (row_str.isEmpty()) return row_char_start;

    const CueRowStretch st = computeRowStretch(
        cue, target_row, qRound(block.height()), row_count, text_rect.width(), true);
    if (!st.layout || st.layout->lineCount() == 0) return row_char_start;
    const QTextLine tl_line = st.layout->lineAt(0);
    if (!tl_line.isValid()) return row_char_start;

    const qreal local_x = (static_cast<qreal>(x) - text_rect.left()) / std::max<qreal>(0.05, st.stretch_x);
    if (local_x <= 0.0) return row_char_start;
    const int col = std::clamp(tl_line.xToCursor(local_x, QTextLine::CursorBetweenCharacters),
                               0, static_cast<int>(row_str.size()));
    return row_char_start + col;
}

void RhythmTimelineWidget::syncLiveEditToCache() {
    if (editing_line_ < 0 || editing_line_ >= static_cast<int>(cached_lines_.size())) return;
    auto& line = cached_lines_[static_cast<std::size_t>(editing_line_)];
    if (editing_cue_ < 0 || editing_cue_ >= static_cast<int>(line.cues.size())) return;
    auto& cue = line.cues[static_cast<std::size_t>(editing_cue_)];

    QString combined = edit_text_;
    if (!preedit_text_.isEmpty()) {
        const int pos = std::clamp(caret_pos_, 0, static_cast<int>(combined.size()));
        combined.insert(pos, preedit_text_);
    }
    cue.text = combined;
    cue.rows = combined.isEmpty() ? QStringList{QString{}} : combined.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    cue.cached_natural_px = -1;
    cue.shaped_rows.clear();
    if (!cue.cached_content_pixmap.isNull()) {
        cue.cached_content_pixmap = QPixmap{};
        if (active_pixmap_cues_ > 0) --active_pixmap_cues_;
    }
}

void RhythmTimelineWidget::updateEditingLane() {
    if (editing_line_ < 0) return;
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int lane_y = ruler_height + std::clamp(editing_track_, 0, 3) * lane_height;
    update(QRect(left_margin, std::max(ruler_height, lane_y - 2),
                 std::max(1, width() - left_margin), lane_height + 4));
}

void RhythmTimelineWidget::beginInlineEdit(int line_index, int cue_index, int click_x, int click_y) {
    if (!document_ || line_index < 0 || line_index >= static_cast<int>(document_->lines().size())) return;
    if (cached_lines_.size() != document_->lines().size()) rebuildSceneCache();

    if (editing_line_ == line_index && (cue_index < 0 || editing_cue_ == cue_index)) {
        if (click_x >= 0 && click_y >= 0) {
            caret_pos_ = hitTestEditCursor(click_x, click_y);
            selection_anchor_ = caret_pos_;
            caret_visible_ = true;
            updateEditingLane();
        }
        return;
    }

    finishInlineEdit(true);
    if (!document_ || line_index >= static_cast<int>(cached_lines_.size())) return;

    auto& cached_line = cached_lines_[static_cast<std::size_t>(line_index)];
    if (cached_line.cues.empty()) {
        updateLineCache(line_index);
    }
    auto& line_after = cached_lines_[static_cast<std::size_t>(line_index)];
    if (line_after.cues.empty()) return;

    if (cue_index < 0 || cue_index >= static_cast<int>(line_after.cues.size())) {
        cue_index = 0;
        for (int i = 0; i < static_cast<int>(line_after.cues.size()); ++i) {
            const auto& c = line_after.cues[static_cast<std::size_t>(i)];
            const qint64 start = c.start_follows_playhead ? playhead_ms_ : c.start_ms;
            const qint64 end = c.end_follows_playhead ? start + 1500 : c.end_ms;
            if (playhead_ms_ >= std::min(start, end) && playhead_ms_ <= std::max(start, end)) {
                cue_index = i;
                break;
            }
        }
    }

    const auto& cue = line_after.cues[static_cast<std::size_t>(cue_index)];
    editing_line_ = line_index;
    editing_cue_ = cue_index;
    editing_first_segment_ = cue.first_segment;
    editing_segment_count_ = cue.segment_count;
    editing_track_ = std::clamp(line_after.track, 0, 3);
    edit_original_text_ = cue.text;
    edit_text_ = cue.text;
    preedit_text_.clear();

    const int effective_x = click_x >= 0 ? click_x : last_edit_click_x_;
    const int effective_y = click_y >= 0 ? click_y : last_edit_click_y_;
    last_edit_click_x_ = -1;
    last_edit_click_y_ = -1;

    if (effective_x >= 0 && effective_y >= 0 && !edit_text_.isEmpty()) {
        caret_pos_ = hitTestEditCursor(effective_x, effective_y);
        selection_anchor_ = caret_pos_;
    } else {
        selection_anchor_ = 0;
        caret_pos_ = static_cast<int>(edit_text_.size());
    }

    caret_visible_ = true;
    caret_blink_timer_->start();
    setFocus(Qt::OtherFocusReason);
    QApplication::inputMethod()->update(Qt::ImQueryAll);
    update();
}

void RhythmTimelineWidget::commitOrCancelEdit(bool commit) {
    finishInlineEdit(commit);
}

void RhythmTimelineWidget::finishInlineEdit(bool commit) {
    if (editing_line_ < 0) return;
    caret_blink_timer_->stop();
    dragging_text_selection_ = false;

    const int line_index = editing_line_;
    const int cue_index = editing_cue_;
    const std::size_t first = editing_first_segment_;
    const std::size_t count = editing_segment_count_;
    QString final_text = edit_text_;
    if (commit && !preedit_text_.isEmpty()) {
        const int pos = std::clamp(caret_pos_, 0, static_cast<int>(final_text.size()));
        final_text.insert(pos, preedit_text_);
    }
    const QString trimmed = final_text.trimmed();
    const QString original = edit_original_text_;

    editing_line_ = -1;
    editing_cue_ = -1;
    editing_first_segment_ = 0;
    editing_segment_count_ = 0;
    preedit_text_.clear();
    edit_text_.clear();
    edit_original_text_.clear();
    caret_pos_ = 0;
    selection_anchor_ = 0;

    if (!commit) {
        if (line_index >= 0 && line_index < static_cast<int>(cached_lines_.size())) {
            auto& cl = cached_lines_[static_cast<std::size_t>(line_index)];
            if (cue_index >= 0 && cue_index < static_cast<int>(cl.cues.size())) {
                auto& c = cl.cues[static_cast<std::size_t>(cue_index)];
                c.text = original;
                c.rows = original.isEmpty() ? QStringList{QString{}} : original.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
                c.cached_natural_px = -1;
                c.shaped_rows.clear();
                if (!c.cached_content_pixmap.isNull()) {
                    c.cached_content_pixmap = QPixmap{};
                    if (active_pixmap_cues_ > 0) --active_pixmap_cues_;
                }
            }
        }
        update();
        return;
    }

    if (trimmed != original && text_edit_handler_ && line_index >= 0) {
        text_edit_handler_(line_index, first, count, trimmed);
    } else if (line_index >= 0 && line_index < static_cast<int>(cached_lines_.size())) {
        updateLineCache(line_index);
    }
    update();
}

void RhythmTimelineWidget::editAdjacentCue(int direction) {
    if (!document_ || cached_lines_.empty() || direction == 0) return;
    struct CueRef {
        qint64 start_ms{};
        int track{};
        int line_idx{};
        int cue_idx{};
    };
    std::vector<CueRef> refs;
    refs.reserve(cached_lines_.size());
    for (int li = 0; li < static_cast<int>(cached_lines_.size()); ++li) {
        const auto& cl = cached_lines_[static_cast<std::size_t>(li)];
        for (int ci = 0; ci < static_cast<int>(cl.cues.size()); ++ci) {
            const auto& c = cl.cues[static_cast<std::size_t>(ci)];
            refs.push_back({c.start_ms, cl.track, li, ci});
        }
    }
    if (refs.empty()) return;
    std::sort(refs.begin(), refs.end(), [](const CueRef& a, const CueRef& b) {
        if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
        if (a.track != b.track) return a.track < b.track;
        if (a.line_idx != b.line_idx) return a.line_idx < b.line_idx;
        return a.cue_idx < b.cue_idx;
    });

    int cur = -1;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        if (refs[static_cast<std::size_t>(i)].line_idx == editing_line_ &&
            refs[static_cast<std::size_t>(i)].cue_idx == editing_cue_) {
            cur = i;
            break;
        }
    }
    finishInlineEdit(true);
    if (cur < 0) return;
    const int next_idx = std::clamp(cur + (direction > 0 ? 1 : -1), 0, static_cast<int>(refs.size()) - 1);
    if (next_idx == cur) return;

    const auto& target = refs[static_cast<std::size_t>(next_idx)];
    selected_line_ = target.line_idx;
    selected_cue_ = target.cue_idx;
    selected_track_ = std::clamp(target.track, 0, 3);
    if (selection_handler_) selection_handler_(selected_line_, selected_track_);

    const qint64 cursor_offset_ms = qRound64(visible_duration_ms_ * cursor_fraction);
    const qint64 vis_start = playhead_ms_ - cursor_offset_ms;
    const qint64 vis_end = vis_start + visible_duration_ms_;
    if (playhead_handler_ && (target.start_ms < vis_start + 300 || target.start_ms > vis_end - 800)) {
        playhead_handler_(std::max<qint64>(0, target.start_ms));
    }

    last_edit_click_x_ = -1;
    last_edit_click_y_ = -1;
    if (edit_handler_) edit_handler_(target.line_idx, target.cue_idx);
    else beginInlineEdit(target.line_idx, target.cue_idx);
}

QVariant RhythmTimelineWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (editing_line_ >= 0 && editing_line_ < static_cast<int>(cached_lines_.size())) {
        const auto& cl = cached_lines_[static_cast<std::size_t>(editing_line_)];
        if (editing_cue_ >= 0 && editing_cue_ < static_cast<int>(cl.cues.size())) {
            const QRect rect = cueTextRectF(cueBlockRectF(cl.cues[static_cast<std::size_t>(editing_cue_)], editing_track_)).toRect();
            switch (query) {
            case Qt::ImEnabled:
                return true;
            case Qt::ImCursorRectangle:
                return QRect(rect.left(), rect.top(), 2, rect.height());
            case Qt::ImFont:
                return text_font_;
            case Qt::ImSurroundingText:
                return edit_text_;
            case Qt::ImCursorPosition:
                return caret_pos_;
            case Qt::ImAnchorPosition:
                return selection_anchor_;
            default:
                break;
            }
        }
    }
    return QWidget::inputMethodQuery(query);
}

void RhythmTimelineWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (editing_line_ < 0) {
        QWidget::inputMethodEvent(event);
        return;
    }
    if (!event->commitString().isEmpty() || event->replacementLength() != 0) {
        if (caret_pos_ != selection_anchor_) {
            const int s = std::min(caret_pos_, selection_anchor_);
            const int len = std::abs(caret_pos_ - selection_anchor_);
            edit_text_.remove(s, len);
            caret_pos_ = s;
            selection_anchor_ = s;
        }
        if (event->replacementLength() > 0) {
            const int rep_start = std::clamp(caret_pos_ + event->replacementStart(), 0, static_cast<int>(edit_text_.size()));
            edit_text_.remove(rep_start, event->replacementLength());
            caret_pos_ = rep_start;
        }
        const QString committed = event->commitString();
        edit_text_.insert(caret_pos_, committed);
        caret_pos_ += static_cast<int>(committed.size());
        selection_anchor_ = caret_pos_;
    }
    preedit_text_ = event->preeditString();
    caret_visible_ = true;
    caret_blink_timer_->start();
    syncLiveEditToCache();
    updateEditingLane();
    event->accept();
}

bool RhythmTimelineWidget::event(QEvent* event) {
    if (event->type() == QEvent::NativeGesture) {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            const qreal val = gesture->value();
            if (std::isfinite(val) && std::abs(val) > 0.0001) {
                adjustTimelineZoom(std::clamp(1.0 + val, 0.75, 1.35));
                event->accept();
                return true;
            }
        }
    }
    return QWidget::event(event);
}

void RhythmTimelineWidget::focusOutEvent(QFocusEvent* event) {
    if (editing_line_ >= 0 && event->reason() != Qt::PopupFocusReason) {
        finishInlineEdit(true);
    }
    QWidget::focusOutEvent(event);
}

void RhythmTimelineWidget::resizeEvent(QResizeEvent* event) {
    cached_sidebar_pixmap_ = QPixmap{};
    QWidget::resizeEvent(event);
}

void RhythmTimelineWidget::leaveEvent(QEvent* event) {
    if (hovered_line_ != -1 || hovered_edge_line_ != -1 || hovered_playhead_) {
        hovered_line_ = -1;
        hovered_edge_line_ = -1;
        hovered_edge_sign_ = -1;
        hovered_playhead_ = false;
        update();
    }
    QWidget::leaveEvent(event);
}

void RhythmTimelineWidget::setDeleteHandler(std::function<void(int)> handler) {
    delete_handler_ = std::move(handler);
}

void RhythmTimelineWidget::setSelection(int line_index, int track) {
    const int clamped_track = std::clamp(track, 0, 3);
    if (selected_line_ == line_index && selected_track_ == clamped_track) return;
    selected_line_ = line_index;
    selected_track_ = clamped_track;
    update();
}

void RhythmTimelineWidget::copyDisplayStateFrom(const RhythmTimelineWidget& other) {
    visible_duration_ms_ = other.visible_duration_ms_;
    selected_line_ = other.selected_line_;
    selected_track_ = other.selected_track_;
    setTextFont(other.text_font_);
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
    for (const auto& line : cached_lines_) {
        inferred = std::max(inferred, line.max_ms);
    }
    return std::max<qint64>(10000, inferred + 5000);
}

void RhythmTimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Keep Antialiasing & SmoothPixmapTransform OFF on the hot path so straight lines and 1:1 pixmap blits
    // hit QRasterPaintEngine's SIMD memcpy / integer Bresenham fast paths across Windows, Linux, and macOS!
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    const QColor canvas_bg(QStringLiteral("#111113"));
    const QColor ruler_bg(QStringLiteral("#18181B"));

    const int w = width();
    const int h = height();
    const int plot_width = std::max(1, w - left_margin - right_margin);
    const int lane_height = std::max(1, (h - ruler_height) / 4);
    const qreal cursor_x = left_margin + plot_width * cursor_fraction;
    const qreal px_per_ms = static_cast<qreal>(plot_width) / static_cast<qreal>(std::max<qint64>(1, visible_duration_ms_));
    const qreal visible_start_ms_f = static_cast<qreal>(playhead_ms_) - visible_duration_ms_ * cursor_fraction;
    const qint64 visible_start_ms = static_cast<qint64>(std::floor(visible_start_ms_f)) - 50;
    const qint64 visible_end_ms = visible_start_ms + visible_duration_ms_ + 100;
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    const int effective_sel_track = clean_presentation_ ? -1 : selected_track_;

    evictDistantPixmaps(visible_start_ms, visible_end_ms);

    const auto x_for_ms_f = [&](qint64 ms) -> qreal {
        return left_margin + (static_cast<qreal>(ms) - visible_start_ms_f) * px_per_ms;
    };
    const auto snap_device_x = [dpr](qreal x) -> qreal {
        return std::round(x * dpr) / dpr;
    };
    const auto base_frames = video_start_.to_frames(timecode_fps_);
    const auto tc_for_ms = [&](qint64 ms) {
        const auto offset = static_cast<qint64>(std::llround(
            static_cast<long double>(ms) * timecode_fps_ / 1000.0L));
        return Timecode::from_frames(std::max<std::int64_t>(0, base_frames + offset), timecode_fps_);
    };

    // 1. Pre-rendered static matte track lanes + sidebar chrome (rebuilt ONLY on resize / track switch)
    const QSize full_logical(w, h);
    if (cached_lanes_pixmap_.isNull() ||
        cached_sidebar_pixmap_.isNull() ||
        cached_sidebar_size_ != full_logical ||
        std::abs(cached_sidebar_dpr_ - dpr) > 0.01 ||
        cached_sidebar_selected_track_ != effective_sel_track ||
        cached_sidebar_clean_ != clean_presentation_) {
        QPixmap lanes_pm(
            std::max(1, qCeil(w * dpr)),
            std::max(1, qCeil(h * dpr)));
        lanes_pm.setDevicePixelRatio(dpr);
        lanes_pm.fill(canvas_bg);
        {
            QPainter lp(&lanes_pm);
            lp.setRenderHint(QPainter::Antialiasing, false);
            lp.fillRect(QRect(left_margin, 0, w - left_margin, ruler_height), ruler_bg);
            for (int track = 0; track < 4; ++track) {
                const int y = ruler_height + track * lane_height;
                const bool selected = track == effective_sel_track;
                if (selected || (track % 2 == 0)) {
                    lp.fillRect(QRect(left_margin, y, w - left_margin, lane_height),
                                selected ? QColor(QStringLiteral("#18181C")) : QColor(QStringLiteral("#141417")));
                }
                lp.setPen(QColor(QStringLiteral("#222226")));
                lp.drawLine(left_margin, y + lane_height, w - right_margin, y + lane_height);
            }
            lp.setPen(QColor(QStringLiteral("#26262C")));
            lp.drawLine(left_margin, ruler_height - 1, w, ruler_height - 1);
        }

        const QSize sidebar_logical(left_margin + 1, h);
        QPixmap sb(
            std::max(1, qCeil(sidebar_logical.width() * dpr)),
            std::max(1, qCeil(sidebar_logical.height() * dpr)));
        sb.setDevicePixelRatio(dpr);
        sb.fill(ruler_bg);
        {
            QPainter sp(&sb);
            sp.setRenderHint(QPainter::TextAntialiasing, true);
            if (!clean_presentation_) {
                QFont tc_hdr = track_font_;
                tc_hdr.setPixelSize(11);
                tc_hdr.setWeight(QFont::Medium);
                sp.setFont(tc_hdr);
                sp.setPen(QColor(QStringLiteral("#82828C")));
                sp.drawText(QRect(12, 0, left_margin - 16, ruler_height),
                            Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Timecode"));
            }
            sp.setPen(QColor(QStringLiteral("#26262C")));
            sp.drawLine(0, ruler_height - 1, left_margin, ruler_height - 1);

            static const std::array<QString, 4> kTrackTitles{
                QStringLiteral("Piste 1"), QStringLiteral("Piste 2"),
                QStringLiteral("Piste 3"), QStringLiteral("Piste 4")};
            sp.setFont(track_font_);
            for (int track = 0; track < 4; ++track) {
                const int y = ruler_height + track * lane_height;
                const bool selected = track == effective_sel_track;
                const QColor header_bg = selected
                    ? QColor(QStringLiteral("#1E1E23"))
                    : (track % 2 ? QColor(QStringLiteral("#141417")) : QColor(QStringLiteral("#16161A")));
                sp.fillRect(QRect(0, y, left_margin, lane_height), header_bg);
                if (selected) {
                    sp.fillRect(QRect(0, y, 2, lane_height), QColor(QStringLiteral("#E4E4E7")));
                }
                sp.setPen(QColor(QStringLiteral("#222226")));
                sp.drawLine(0, y + lane_height, left_margin, y + lane_height);
                sp.setPen(selected ? QColor(QStringLiteral("#F4F4F6"))
                                   : (clean_presentation_ ? QColor(QStringLiteral("#A8A8B2"))
                                                          : QColor(QStringLiteral("#84848E"))));
                sp.drawText(QRect(12, y, left_margin - 16, lane_height),
                            Qt::AlignVCenter | Qt::AlignLeft,
                            kTrackTitles[static_cast<std::size_t>(track)]);
            }
            if (ruler_height + 4 * lane_height < h) {
                sp.fillRect(QRect(0, ruler_height + 4 * lane_height + 1, left_margin,
                                  h - (ruler_height + 4 * lane_height + 1)),
                            QColor(QStringLiteral("#141417")));
            }
            sp.setPen(QColor(QStringLiteral("#26262B")));
            sp.drawLine(left_margin, 0, left_margin, h);
        }

        cached_lanes_pixmap_ = std::move(lanes_pm);
        cached_sidebar_pixmap_ = std::move(sb);
        cached_sidebar_size_ = full_logical;
        cached_sidebar_dpr_ = dpr;
        cached_sidebar_selected_track_ = effective_sel_track;
        cached_sidebar_clean_ = clean_presentation_;
    }
    painter.drawPixmap(0, 0, cached_lanes_pixmap_);

    // 2. Adaptive Timecode Ruler (cached glyph pixmaps + integer straight lines = zero text shaping per frame)
    if (std::abs(ruler_cache_dpr_ - dpr) > 0.01) {
        ruler_label_cache_.clear();
        ruler_cache_dpr_ = dpr;
    } else if (ruler_label_cache_.size() > 96) {
        ruler_label_cache_.clear();
    }
    const qint64 major_tick_ms = choose_ruler_major_tick_ms(visible_duration_ms_, plot_width);
    const qint64 minor_tick_ms = std::max<qint64>(20, major_tick_ms / 5);
    const qint64 first_minor = static_cast<qint64>(
        std::floor(visible_start_ms_f / static_cast<double>(minor_tick_ms))) * minor_tick_ms;

    QChar tc_chars[11];
    constexpr int kLabelW = 92;
    constexpr int kLabelH = ruler_height - 6;

    for (qint64 ms = first_minor; ms <= visible_end_ms + major_tick_ms; ms += minor_tick_ms) {
        const int xi = qRound(x_for_ms_f(ms));
        if (xi < left_margin - 50 || xi > w - right_margin) continue;
        const qint64 rem = ((ms % major_tick_ms) + major_tick_ms) % major_tick_ms;
        const bool is_major = rem == 0;
        if (xi >= left_margin) {
            painter.setPen(is_major ? QColor(QStringLiteral("#3A3A42")) : QColor(QStringLiteral("#26262C")));
            painter.drawLine(xi, ruler_height - (is_major ? 7 : 3), xi, ruler_height - 2);
            if (is_major) {
                painter.setPen(QColor(255, 255, 255, 10));
                painter.drawLine(xi, ruler_height, xi, h);
            }
        }
        if (is_major) {
            auto it = ruler_label_cache_.find(ms);
            if (it == ruler_label_cache_.end()) {
                QPixmap lbl_pm(std::max(1, qCeil(kLabelW * dpr)), std::max(1, qCeil(kLabelH * dpr)));
                lbl_pm.setDevicePixelRatio(dpr);
                lbl_pm.fill(Qt::transparent);
                QPainter lp(&lbl_pm);
                lp.setRenderHint(QPainter::TextAntialiasing, true);
                lp.setFont(ruler_font_);
                lp.setPen(QColor(QStringLiteral("#9A9AA4")));
                format_tc_chars(tc_for_ms(ms), tc_chars);
                lp.drawText(QRect(0, 0, kLabelW, kLabelH),
                            Qt::AlignHCenter | Qt::AlignVCenter,
                            QString::fromRawData(tc_chars, 11));
                lp.end();
                it = ruler_label_cache_.insert(ms, std::move(lbl_pm));
            }
            painter.drawPixmap(xi - kLabelW / 2, 0, it.value());
        }
    }

    // 3. Cues & Direct In-Band Editable Script
    if (document_) {
        const QRect plot_clip(left_margin, ruler_height, plot_width, h - ruler_height);
        painter.save();
        painter.setClipRect(plot_clip);

        for (int line_index = 0; line_index < static_cast<int>(cached_lines_.size()); ++line_index) {
            auto& line = cached_lines_[static_cast<std::size_t>(line_index)];
            if (line.track < 0 || line.track > 3) continue;
            if (!line.dynamic_bounds && (line.max_ms < visible_start_ms || line.min_ms > visible_end_ms)) {
                continue;
            }

            const int lane_y = ruler_height + line.track * lane_height;
            const bool selected = !clean_presentation_ && line_index == selected_line_;
            const bool hovered = !clean_presentation_ && line_index == hovered_line_;
            const int block_y = lane_y + 4;
            const int block_height = std::max(22, lane_height - 8);

            for (int cue_index = 0; cue_index < static_cast<int>(line.cues.size()); ++cue_index) {
                auto& cue = line.cues[static_cast<std::size_t>(cue_index)];
                const qint64 start_ms = cue.start_follows_playhead ? playhead_ms_ : cue.start_ms;
                qint64 end_ms = cue.end_follows_playhead ? start_ms + 1500 : cue.end_ms;
                if (end_ms <= start_ms) end_ms = start_ms + 180;
                if (end_ms < visible_start_ms || start_ms > visible_end_ms) continue;

                const bool is_editing = !clean_presentation_ &&
                                        editing_line_ == line_index &&
                                        editing_cue_ == cue_index;
                const bool active_at_playhead = playhead_ms_ >= start_ms && playhead_ms_ <= end_ms;

                const qreal block_x = snap_device_x(x_for_ms_f(start_ms));
                const int block_w = std::max(10, qRound(static_cast<qreal>(end_ms - start_ms) * px_per_ms));
                const QRectF block(block_x, block_y, block_w, block_height);
                const QSize target_block_size(block_w, block_height);

                if (!is_editing) {
                    const bool can_cache = block_w <= 3072 && block_height <= 512;
                    if (can_cache) {
                        if (cue.cached_content_pixmap.isNull() ||
                            cue.cached_block_size != target_block_size ||
                            std::abs(cue.cached_pixmap_dpr - dpr) > 0.01) {
                            const bool was_null = cue.cached_content_pixmap.isNull();
                            const QSize px_size(
                                std::max(1, qCeil(block_w * dpr)),
                                std::max(1, qCeil(block_height * dpr)));
                            QPixmap pm(px_size);
                            pm.setDevicePixelRatio(dpr);
                            pm.fill(Qt::transparent);

                            QPainter pm_p(&pm);
                            pm_p.setRenderHint(QPainter::Antialiasing, true);
                            pm_p.setRenderHint(QPainter::TextAntialiasing, true);

                            // Bake full normal card background, border, and role accent bar directly into the cached QPixmap!
                            const QColor fill = blend_color(QColor(QStringLiteral("#1B1B20")), line.role_color, 0.08);
                            pm_p.setPen(QPen(QColor(QStringLiteral("#2E2E36")), 1.0));
                            pm_p.setBrush(fill);
                            pm_p.drawRoundedRect(QRectF(0.5, 0.5, block_w - 1.0, block_height - 1.0), 5.0, 5.0);

                            if (block_height > 12) {
                                pm_p.setPen(Qt::NoPen);
                                pm_p.setBrush(line.role_color);
                                pm_p.drawRoundedRect(
                                    QRectF(2.5, 3.5, 3.0, block_height - 7.0), 1.5, 1.5);
                            }

                            pm_p.setClipRect(QRectF(6.0, 1.0, std::max(1, block_w - 10), std::max(1, block_height - 3)));
                            if (block_w >= 36) {
                                pm_p.setFont(role_font_);
                                pm_p.setPen(blend_color(QColor(QStringLiteral("#B4B4BE")), line.role_color, 0.35));
                                const qreal role_h = block_height >= 42 ? 16.0 : 12.0;
                                pm_p.drawText(QRectF(10.0, 1.0, block_w - 14.0, role_h),
                                              Qt::AlignLeft | Qt::AlignVCenter, line.role);
                            }

                            const QRectF local_text = cueTextRectF(QRectF(0.0, 0.0, block_w, block_height));
                            if (!cue.text.isEmpty() && local_text.width() > 2.0 && local_text.height() > 4.0) {
                                const int row_count = std::max(1, static_cast<int>(cue.rows.size()));
                                const qreal row_height = local_text.height() / static_cast<qreal>(row_count);
                                pm_p.setPen(QColor(QStringLiteral("#F4F4F7")));

                                for (int row = 0; row < row_count; ++row) {
                                    if (cue.rows[row].isEmpty()) continue;
                                    const CueRowStretch st = computeRowStretch(
                                        cue, row, block_height, row_count, local_text.width(), false);
                                    if (!st.layout) continue;
                                    const qreal draw_y = local_text.top() + row * row_height + (row_height - st.line_height) * 0.5;

                                    pm_p.save();
                                    pm_p.translate(local_text.left(), draw_y);
                                    if (std::abs(st.stretch_x - 1.0) > 0.001) {
                                        pm_p.scale(st.stretch_x, 1.0);
                                    }
                                    st.layout->draw(&pm_p, QPointF(0.0, 0.0));
                                    pm_p.restore();
                                }
                            }
                            pm_p.end();
                            cue.cached_content_pixmap = std::move(pm);
                            cue.cached_block_size = target_block_size;
                            cue.cached_pixmap_dpr = dpr;
                            if (was_null) ++active_pixmap_cues_;
                        }
                        // Direct 1:1 opacity=1.0 SIMD SourceOver blit (zero per-pixel alpha multiplication!)
                        painter.drawPixmap(QPointF(block_x, block_y), cue.cached_content_pixmap);

                        if (selected || hovered || active_at_playhead) {
                            painter.setRenderHint(QPainter::Antialiasing, true);
                            if (active_at_playhead) {
                                QColor glow = line.role_color;
                                glow.setAlpha(18);
                                painter.setBrush(glow);
                            } else {
                                painter.setBrush(Qt::NoBrush);
                            }
                            const QColor hi_border = selected
                                ? QColor(QStringLiteral("#D4D4DC"))
                                : (hovered ? QColor(QStringLiteral("#52525E"))
                                           : blend_color(QColor(QStringLiteral("#3A3A44")), line.role_color, 0.45));
                            painter.setPen(QPen(hi_border, selected ? 1.4 : 1.0));
                            painter.drawRoundedRect(block.adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);
                            painter.setRenderHint(QPainter::Antialiasing, false);
                        }
                    } else {
                        painter.setRenderHint(QPainter::Antialiasing, true);
                        const QColor fill = blend_color(QColor(QStringLiteral("#1B1B20")), line.role_color,
                                                        active_at_playhead ? 0.12 : 0.07);
                        painter.setPen(QPen(selected ? QColor(QStringLiteral("#D4D4DC"))
                                                     : QColor(QStringLiteral("#2E2E36")),
                                            selected ? 1.4 : 1.0));
                        painter.setBrush(fill);
                        painter.drawRoundedRect(block.adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);
                        if (block_height > 12) {
                            painter.setPen(Qt::NoPen);
                            painter.setBrush(line.role_color);
                            painter.drawRoundedRect(
                                QRectF(block_x + 2.5, block_y + 3.5, 3.0, block_height - 7.0), 1.5, 1.5);
                        }
                        painter.save();
                        painter.setClipRect(block.adjusted(6.0, 1.0, -4.0, -2.0).intersected(QRectF(plot_clip)));
                        if (block_w >= 36) {
                            painter.setFont(role_font_);
                            painter.setPen(blend_color(QColor(QStringLiteral("#B4B4BE")), line.role_color, 0.35));
                            const qreal role_h = block_height >= 42 ? 16.0 : 12.0;
                            painter.drawText(QRectF(block_x + 10.0, block_y + 1.0, block_w - 14.0, role_h),
                                             Qt::AlignLeft | Qt::AlignVCenter, line.role);
                        }
                        const QRectF text_rect = cueTextRectF(block);
                        if (!cue.text.isEmpty() && text_rect.width() > 2.0) {
                            const int row_count = std::max(1, static_cast<int>(cue.rows.size()));
                            const qreal row_height = text_rect.height() / static_cast<qreal>(row_count);
                            painter.setPen(QColor(QStringLiteral("#F4F4F7")));
                            for (int row = 0; row < row_count; ++row) {
                                if (cue.rows[row].isEmpty()) continue;
                                const CueRowStretch st = computeRowStretch(
                                    cue, row, block_height, row_count, text_rect.width(), false);
                                if (!st.layout) continue;
                                const qreal draw_y = text_rect.top() + row * row_height + (row_height - st.line_height) * 0.5;
                                painter.save();
                                painter.translate(text_rect.left(), draw_y);
                                if (std::abs(st.stretch_x - 1.0) > 0.001) painter.scale(st.stretch_x, 1.0);
                                st.layout->draw(&painter, QPointF(0.0, 0.0));
                                painter.restore();
                            }
                        }
                        painter.restore();
                        painter.setRenderHint(QPainter::Antialiasing, false);
                    }
                } else {
                    painter.setRenderHint(QPainter::Antialiasing, true);
                    const QColor fill = blend_color(QColor(QStringLiteral("#26262D")), line.role_color, 0.12);
                    painter.setPen(QPen(QColor(QStringLiteral("#FFFFFF")), 1.8));
                    painter.setBrush(fill);
                    painter.drawRoundedRect(block.adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);
                    if (block_height > 12) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(line.role_color);
                        painter.drawRoundedRect(
                            QRectF(block_x + 2.5, block_y + 3.5, 3.0, block_height - 7.0), 1.5, 1.5);
                    }
                    painter.save();
                    const QRectF inner_clip = block.adjusted(6.0, 1.0, -4.0, -2.0).intersected(QRectF(plot_clip));
                    if (!inner_clip.isEmpty()) {
                        painter.setClipRect(inner_clip);
                        if (block_w >= 36) {
                            painter.setFont(role_font_);
                            painter.setPen(blend_color(QColor(QStringLiteral("#B4B4BE")), line.role_color, 0.35));
                            const qreal role_h = block_height >= 42 ? 16.0 : 12.0;
                            painter.drawText(QRectF(block_x + 10.0, block_y + 1.0, block_w - 14.0, role_h),
                                             Qt::AlignLeft | Qt::AlignVCenter, line.role);
                        }

                        const QRectF text_rect = cueTextRectF(block);
                        if (text_rect.width() > 2.0 && text_rect.height() > 4.0) {
                            const int row_count = std::max(1, static_cast<int>(cue.rows.size()));
                            const qreal row_height = text_rect.height() / static_cast<qreal>(row_count);
                            const QStringList edit_rows = edit_text_.isEmpty()
                                ? QStringList{QString{}} : edit_text_.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
                            const int sel_min = std::min(caret_pos_, selection_anchor_);
                            const int sel_max = std::max(caret_pos_, selection_anchor_);
                            int global_offset = 0;

                            if (edit_text_.isEmpty() && preedit_text_.isEmpty()) {
                                const int ph_px = uniformPixelSize(block_height, 1);
                                QFont ph_font = text_font_;
                                ph_font.setBold(false);
                                ph_font.setPixelSize(ph_px);
                                painter.setFont(ph_font);
                                painter.setPen(QColor(QStringLiteral("#6E6E78")));
                                painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                                                 QStringLiteral("Écrire la réplique…"));
                            }

                            for (int row = 0; row < row_count; ++row) {
                                const CueRowStretch st = computeRowStretch(
                                    cue, row, block_height, row_count, text_rect.width(), true);
                                const qreal row_top = text_rect.top() + row * row_height;
                                const qreal draw_y = row_top + (row_height - st.line_height) * 0.5;
                                const QString& displayed_row = cue.rows[row];
                                const QTextLine tl_line = (st.layout && st.layout->lineCount() > 0)
                                    ? st.layout->lineAt(0) : QTextLine{};

                                const QString raw_row = row < edit_rows.size() ? edit_rows[row] : QString{};
                                const int row_len = static_cast<int>(raw_row.size());
                                const int row_start = global_offset;
                                const int row_end = row_start + row_len;

                                if (tl_line.isValid() && sel_min != sel_max && sel_max > row_start && sel_min < row_end) {
                                    const int local_s = std::clamp(sel_min - row_start, 0, static_cast<int>(displayed_row.size()));
                                    const int local_e = std::clamp(sel_max - row_start, 0, static_cast<int>(displayed_row.size()));
                                    const qreal sx1 = text_rect.left() + tl_line.cursorToX(local_s) * st.stretch_x;
                                    const qreal sx2 = text_rect.left() + tl_line.cursorToX(local_e) * st.stretch_x;
                                    painter.fillRect(
                                        QRectF(std::min(sx1, sx2), row_top + 1.0,
                                               std::max<qreal>(2.0, std::abs(sx2 - sx1)), row_height - 2.0),
                                        QColor(96, 165, 250, 95));
                                }

                                if (st.layout && !displayed_row.isEmpty()) {
                                    painter.save();
                                    painter.setPen(QColor(QStringLiteral("#FFFFFF")));
                                    painter.translate(text_rect.left(), draw_y);
                                    if (std::abs(st.stretch_x - 1.0) > 0.001) {
                                        painter.scale(st.stretch_x, 1.0);
                                    }
                                    st.layout->draw(&painter, QPointF(0.0, 0.0));
                                    painter.restore();
                                }

                                if (caret_visible_ && caret_pos_ >= row_start && caret_pos_ <= row_end) {
                                    const int local_c = std::clamp(
                                        caret_pos_ - row_start + static_cast<int>(preedit_text_.size()),
                                        0, static_cast<int>(displayed_row.size()));
                                    qreal cx = text_rect.left() + (tl_line.isValid() ? tl_line.cursorToX(local_c) * st.stretch_x : 0.0);
                                    cx = std::clamp<qreal>(cx, text_rect.left(), std::max<qreal>(text_rect.left(), text_rect.right() - 1.5));
                                    painter.setPen(Qt::NoPen);
                                    painter.setBrush(QColor(QStringLiteral("#FEFEFE")));
                                    painter.drawRoundedRect(
                                        QRectF(cx - 1.0, row_top + 2.0, 2.2, std::max<qreal>(8.0, row_height - 4.0)),
                                        1.0, 1.0);
                                }

                                global_offset = row_end + 1;
                            }
                        }
                    }
                    painter.restore();
                    painter.setRenderHint(QPainter::Antialiasing, false);
                }
            }

            for (int sign_index = 0; sign_index < static_cast<int>(line.signs.size()); ++sign_index) {
                const auto& sign = line.signs[static_cast<std::size_t>(sign_index)];
                if (sign.position_ms < visible_start_ms || sign.position_ms > visible_end_ms) continue;
                const int xi = qRound(x_for_ms_f(sign.position_ms));
                QColor tick_color = sign.color;
                tick_color.setAlpha(selected ? 215 : 145);
                painter.setPen(QPen(tick_color, selected ? 1.5 : 1.0));
                painter.drawLine(xi, block_y + 2, xi, block_y + block_height - 2);
            }

            if (selected && editing_line_ != line_index) {
                painter.setRenderHint(QPainter::Antialiasing, true);
                for (int sign_index = 0; sign_index < static_cast<int>(line.signs.size()); ++sign_index) {
                    const auto& sign = line.signs[static_cast<std::size_t>(sign_index)];
                    if (sign.position_ms < visible_start_ms - 40 || sign.position_ms > visible_end_ms + 40) continue;
                    const qreal xf = snap_device_x(x_for_ms_f(sign.position_ms));
                    const bool active_edge = (selected_edge_line_ == line_index && selected_edge_sign_ == sign_index) ||
                                             (hovered_edge_line_ == line_index && hovered_edge_sign_ == sign_index);
                    painter.setPen(QPen(QColor(QStringLiteral("#18181B")), 1.0));
                    painter.setBrush(active_edge ? QColor(QStringLiteral("#FBBF24"))
                                                 : QColor(QStringLiteral("#E4E4E7")));
                    painter.drawRoundedRect(QRectF(xf - 3.0, block_y + block_height / 2.0 - 9.0, 6.0, 18.0), 2.0, 2.0);
                }
                painter.setRenderHint(QPainter::Antialiasing, false);
            }
        }
        painter.restore();

        for (const auto& marker : cached_markers_) {
            const qint64 marker_ms = marker.position_ms;
            if (marker_ms < visible_start_ms || marker_ms > visible_end_ms) continue;
            const int xi = qRound(x_for_ms_f(marker_ms));
            if (xi < left_margin || xi > w - right_margin) continue;
            const QColor marker_color = marker.loop
                ? QColor(QStringLiteral("#A1A1AA"))
                : QColor(QStringLiteral("#D4D4D8"));
            painter.setPen(QPen(marker_color, 1.0, Qt::DashLine));
            painter.drawLine(xi, ruler_height, xi, h - 1);
            painter.fillRect(QRect(xi - 2, ruler_height - 6, 5, 5), marker_color);
        }
    }

    // 4. Cached Track Header Sidebar (overlaps left edge cleanly)
    painter.drawPixmap(0, 0, cached_sidebar_pixmap_);

    // 5. Stationary Red Barre Rythmo (integer fillRect stem + anti-aliased cap triangle)
    const QColor playhead_color = (scrubbing_playhead_ || hovered_playhead_)
        ? QColor(QStringLiteral("#F2555A"))
        : QColor(QStringLiteral("#E5484D"));
    painter.fillRect(QRectF(snap_device_x(cursor_x - 1.0), ruler_height - 1.0, 2.0, h - ruler_height + 1.0),
                     playhead_color);

    painter.setRenderHint(QPainter::Antialiasing, true);
    QPolygonF cap;
    cap << QPointF(cursor_x - 5.5, ruler_height - 7.5)
        << QPointF(cursor_x + 5.5, ruler_height - 7.5)
        << QPointF(cursor_x, ruler_height + 2.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(playhead_color);
    painter.drawPolygon(cap);
}

int RhythmTimelineWidget::lineAtPosition(int x, int y) const {
    if (!document_ || y < ruler_height || y >= height()) return -1;
    const auto hit = cueAtPosition(x, y);
    if (hit.first >= 0) return hit.first;

    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    for (int i = static_cast<int>(cached_lines_.size()) - 1; i >= 0; --i) {
        const auto& line = cached_lines_[static_cast<std::size_t>(i)];
        if (line.track != track || line.signs.empty()) continue;
        const int x1 = xForPosition(line.min_ms);
        const int x2 = std::max(x1 + 10, xForPosition(line.max_ms));
        if (x >= x1 && x <= x2) return i;
    }
    return -1;
}

std::pair<int, int> RhythmTimelineWidget::cueAtPosition(int x, int y) const {
    if (!document_ || y < ruler_height || y >= height()) return {-1, -1};
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    for (int line_index = static_cast<int>(cached_lines_.size()) - 1; line_index >= 0; --line_index) {
        const auto& line = cached_lines_[static_cast<std::size_t>(line_index)];
        if (line.track != track) continue;
        for (int cue_index = static_cast<int>(line.cues.size()) - 1; cue_index >= 0; --cue_index) {
            const auto& cue = line.cues[static_cast<std::size_t>(cue_index)];
            const qint64 start = cue.start_follows_playhead ? playhead_ms_ : cue.start_ms;
            const qint64 end = cue.end_follows_playhead ? start + 1500 : cue.end_ms;
            const int left = xForPosition(std::min(start, end));
            const int right = std::max(left + 10, xForPosition(std::max(start, end)));
            if (x >= left && x <= right) return {line_index, cue_index};
        }
    }
    return {-1, -1};
}

std::pair<int, int> RhythmTimelineWidget::edgeAtPosition(int x, int y) const {
    if (!document_ || y < ruler_height || y >= height()) return {-1, -1};
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    int best_line = -1;
    int best_sign = -1;
    int best_distance = 9;
    for (int line_index = static_cast<int>(cached_lines_.size()) - 1; line_index >= 0; --line_index) {
        const auto& line = cached_lines_[static_cast<std::size_t>(line_index)];
        if (line.track != track || line.signs.empty()) continue;
        const int first_idx = 0;
        const int last_idx = static_cast<int>(line.signs.size()) - 1;
        for (int sign_index = 0; sign_index <= last_idx; ++sign_index) {
            if (line.cues.size() <= 1 && sign_index != first_idx && sign_index != last_idx) continue;
            const int distance = std::abs(x - xForPosition(line.signs[static_cast<std::size_t>(sign_index)].position_ms));
            if (distance < best_distance) {
                best_distance = distance;
                best_line = line_index;
                best_sign = sign_index;
            }
        }
    }
    return {best_line, best_sign};
}

void RhythmTimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || width() <= left_margin + right_margin) return;
    setFocus(Qt::MouseFocusReason);
    pending_click_edit_line_ = -1;
    pending_click_edit_cue_ = -1;

    const int y = static_cast<int>(event->position().y());
    const int clicked_x = static_cast<int>(event->position().x());
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));

    if (editing_line_ >= 0 && clicked_x >= left_margin && y >= ruler_height) {
        const auto edge = edgeAtPosition(clicked_x, y);
        const auto cue = cueAtPosition(clicked_x, y);
        if (edge.first < 0 && cue.first == editing_line_ && cue.second == editing_cue_) {
            const int pos = hitTestEditCursor(clicked_x, y);
            caret_pos_ = pos;
            if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
                selection_anchor_ = pos;
            }
            dragging_text_selection_ = true;
            caret_visible_ = true;
            caret_blink_timer_->start();
            updateEditingLane();
            event->accept();
            return;
        }
        finishInlineEdit(true);
    } else if (editing_line_ >= 0) {
        finishInlineEdit(true);
    }

    if (playhead_handler_ &&
        ((y >= 0 && y < ruler_height && clicked_x >= left_margin) ||
         (y >= ruler_height - 12 && y <= ruler_height + 10 && std::abs(clicked_x - cursor_x) <= 12))) {
        scrubbing_playhead_ = true;
        if (std::abs(clicked_x - cursor_x) <= 14) {
            scrub_start_x_ = cursor_x;
            scrub_start_position_ms_ = playhead_ms_;
        } else {
            const qint64 target_ms = std::max<qint64>(0, positionForX(clicked_x));
            scrub_start_x_ = clicked_x;
            scrub_start_position_ms_ = target_ms;
        }
        setCursor(Qt::SizeHorCursor);
        if (playhead_drag_handler_) playhead_drag_handler_(true);
        playhead_handler_(scrub_start_position_ms_);
        event->accept();
        return;
    }

    if (y < ruler_height || y >= height()) return;
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    const int track = std::clamp((y - ruler_height) / lane_height, 0, 3);
    selected_track_ = track;

    if (clicked_x < left_margin) {
        selected_line_ = -1;
        selected_cue_ = -1;
        selected_edge_line_ = -1;
        selected_edge_sign_ = -1;
        update();
        if (selection_handler_) selection_handler_(selected_line_, selected_track_);
        event->accept();
        return;
    }

    const int prev_selected_line = selected_line_;
    const auto edge = edgeAtPosition(clicked_x, y);
    const auto cue = cueAtPosition(clicked_x, y);
    selected_line_ = edge.first >= 0 ? edge.first : (cue.first >= 0 ? cue.first : lineAtPosition(clicked_x, y));
    selected_cue_ = cue.first == selected_line_ ? cue.second : -1;
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
    } else if (selected_line_ >= 0 && selected_line_ == prev_selected_line && !clean_presentation_) {
        pending_click_edit_line_ = selected_line_;
        pending_click_edit_cue_ = selected_cue_;
        pending_click_pos_ = event->position().toPoint();
    }

    update();
    if (selection_handler_) selection_handler_(selected_line_, selected_track_);
    event->accept();
}

void RhythmTimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !clean_presentation_) {
        pending_click_edit_line_ = -1;
        const int x = static_cast<int>(event->position().x());
        const int y = static_cast<int>(event->position().y());
        if (x >= left_margin && y >= ruler_height) {
            if (editing_line_ >= 0) {
                const auto cue = cueAtPosition(x, y);
                if (cue.first == editing_line_ && cue.second == editing_cue_) {
                    const int pos = hitTestEditCursor(x, y);
                    int w_start = pos;
                    int w_end = pos;
                    while (w_start > 0 && is_word_char(edit_text_[w_start - 1])) --w_start;
                    while (w_end < edit_text_.size() && is_word_char(edit_text_[w_end])) ++w_end;
                    selection_anchor_ = w_start;
                    caret_pos_ = w_end;
                    dragging_text_selection_ = false;
                    caret_visible_ = true;
                    updateEditingLane();
                    event->accept();
                    return;
                }
            }

            const auto cue = cueAtPosition(x, y);
            const int line_index = cue.first >= 0 ? cue.first : lineAtPosition(x, y);
            if (line_index >= 0) {
                selected_line_ = line_index;
                selected_cue_ = cue.first == line_index ? cue.second : -1;
                const int lane_height = std::max(1, (height() - ruler_height) / 4);
                selected_track_ = std::clamp((y - ruler_height) / lane_height, 0, 3);
                last_edit_click_x_ = x;
                last_edit_click_y_ = y;
                if (selection_handler_) selection_handler_(selected_line_, selected_track_);
                if (edit_handler_) edit_handler_(line_index, selected_cue_);
                else beginInlineEdit(line_index, selected_cue_, x, y);
                event->accept();
                return;
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void RhythmTimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_text_selection_ && editing_line_ >= 0) {
        const int next_caret = hitTestEditCursor(
            static_cast<int>(event->position().x()),
            static_cast<int>(event->position().y()));
        if (next_caret != caret_pos_) {
            caret_pos_ = next_caret;
            caret_visible_ = true;
            updateEditingLane();
        }
        event->accept();
        return;
    }

    if (pending_click_edit_line_ >= 0 &&
        (event->position().toPoint() - pending_click_pos_).manhattanLength() > 5) {
        pending_click_edit_line_ = -1;
    }

    if (scrubbing_playhead_ && playhead_handler_) {
        const int plot_width = std::max(1, width() - left_margin - right_margin);
        const int pointer_x = std::clamp(static_cast<int>(event->position().x()), left_margin, width() - right_margin);
        const qint64 delta_ms = qRound64(static_cast<double>(pointer_x - scrub_start_x_) *
                                         visible_duration_ms_ / plot_width);
        playhead_handler_(std::max<qint64>(0, scrub_start_position_ms_ + delta_ms));
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

    const int x = static_cast<int>(event->position().x());
    const int y = static_cast<int>(event->position().y());
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const int cursor_x = left_margin + static_cast<int>(std::lround(plot_width * cursor_fraction));
    const bool near_playhead = playhead_handler_ && y >= ruler_height - 12 &&
        y <= ruler_height + 10 && std::abs(x - cursor_x) <= 12;
    const bool in_ruler = playhead_handler_ && y >= 0 && y < ruler_height && x >= left_margin;
    const auto edge = (document_ && x >= left_margin) ? edgeAtPosition(x, y) : std::pair<int, int>{-1, -1};
    const auto cue = (document_ && x >= left_margin) ? cueAtPosition(x, y) : std::pair<int, int>{-1, -1};
    const int next_hovered_line = edge.first >= 0 ? edge.first
        : (cue.first >= 0 ? cue.first : ((document_ && x >= left_margin) ? lineAtPosition(x, y) : -1));

    if (hovered_playhead_ != near_playhead ||
        hovered_line_ != next_hovered_line ||
        hovered_edge_line_ != edge.first ||
        hovered_edge_sign_ != edge.second) {
        hovered_playhead_ = near_playhead;
        hovered_line_ = next_hovered_line;
        hovered_edge_line_ = edge.first;
        hovered_edge_sign_ = edge.second;
        update();
    }

    if (edge.first >= 0 || near_playhead) {
        setCursor(Qt::SizeHorCursor);
    } else if (in_ruler) {
        setCursor(Qt::PointingHandCursor);
    } else if (editing_line_ >= 0 && cue.first == editing_line_ && cue.second == editing_cue_) {
        setCursor(Qt::IBeamCursor);
    } else if (!clean_presentation_ && cue.first >= 0 && cue.first == selected_line_) {
        setCursor(Qt::IBeamCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }

    QWidget::mouseMoveEvent(event);
}

void RhythmTimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_text_selection_) {
        dragging_text_selection_ = false;
        event->accept();
        return;
    }
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
    if (event->button() == Qt::LeftButton && pending_click_edit_line_ >= 0) {
        const int line_idx = pending_click_edit_line_;
        const int cue_idx = pending_click_edit_cue_;
        const QPoint click_pt = event->position().toPoint();
        pending_click_edit_line_ = -1;
        pending_click_edit_cue_ = -1;
        if ((click_pt - pending_click_pos_).manhattanLength() <= 5) {
            last_edit_click_x_ = click_pt.x();
            last_edit_click_y_ = click_pt.y();
            if (edit_handler_) edit_handler_(line_idx, cue_idx);
            else beginInlineEdit(line_idx, cue_idx, click_pt.x(), click_pt.y());
            event->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void RhythmTimelineWidget::navigateToAdjacentCue(int direction) {
    if (direction == 0 || !playhead_handler_ || sorted_nav_targets_.empty()) return;
    const qint64 tolerance = std::max<qint64>(1, qRound64(500.0 / std::max(1.0, media_fps_)));
    if (direction > 0) {
        const qint64 threshold = playhead_ms_ > std::numeric_limits<qint64>::max() - tolerance
            ? std::numeric_limits<qint64>::max() : playhead_ms_ + tolerance;
        const auto next = std::upper_bound(sorted_nav_targets_.begin(), sorted_nav_targets_.end(), threshold);
        if (next != sorted_nav_targets_.end()) playhead_handler_(*next);
    } else {
        const qint64 threshold = playhead_ms_ < std::numeric_limits<qint64>::min() + tolerance
            ? std::numeric_limits<qint64>::min() : playhead_ms_ - tolerance;
        const auto previous = std::lower_bound(sorted_nav_targets_.begin(), sorted_nav_targets_.end(), threshold);
        if (previous != sorted_nav_targets_.begin()) playhead_handler_(*std::prev(previous));
    }
}

void RhythmTimelineWidget::stepFrame(int direction) {
    stepTimelineFrame(direction);
}

void RhythmTimelineWidget::zoomIn() {
    adjustTimelineZoom(1.22);
}

void RhythmTimelineWidget::zoomOut() {
    adjustTimelineZoom(1.0 / 1.22);
}

void RhythmTimelineWidget::resetZoom() {
    setVisibleDurationMs(8000);
}

void RhythmTimelineWidget::setVisibleDurationMs(qint64 duration_ms) {
    const qint64 clamped = std::clamp<qint64>(duration_ms, 500, 120000);
    if (clamped == visible_duration_ms_) return;
    visible_duration_ms_ = clamped;
    if (zoom_changed_handler_) zoom_changed_handler_(visible_duration_ms_);
    update();
}

void RhythmTimelineWidget::keyPressEvent(QKeyEvent* event) {
    if (editing_line_ >= 0) {
        const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
        const bool alt = event->modifiers().testFlag(Qt::AltModifier);
        const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
        const bool has_selection = caret_pos_ != selection_anchor_;

        auto delete_selection = [&] {
            const int s = std::min(caret_pos_, selection_anchor_);
            const int len = std::abs(caret_pos_ - selection_anchor_);
            edit_text_.remove(s, len);
            caret_pos_ = s;
            selection_anchor_ = s;
        };
        auto touch_caret = [&] {
            caret_visible_ = true;
            caret_blink_timer_->start();
            syncLiveEditToCache();
            updateEditingLane();
        };

        if (event->key() == Qt::Key_Escape) {
            finishInlineEdit(false);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
            editAdjacentCue((event->key() == Qt::Key_Backtab || shift) ? -1 : 1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (shift || alt) {
                if (has_selection) delete_selection();
                edit_text_.insert(caret_pos_, QLatin1Char('\n'));
                ++caret_pos_;
                selection_anchor_ = caret_pos_;
                touch_caret();
            } else {
                finishInlineEdit(true);
            }
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::SelectAll)) {
            selection_anchor_ = 0;
            caret_pos_ = static_cast<int>(edit_text_.size());
            caret_visible_ = true;
            updateEditingLane();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Copy)) {
            if (has_selection) {
                const int s = std::min(caret_pos_, selection_anchor_);
                QApplication::clipboard()->setText(edit_text_.mid(s, std::abs(caret_pos_ - selection_anchor_)));
            }
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Cut)) {
            if (has_selection) {
                const int s = std::min(caret_pos_, selection_anchor_);
                QApplication::clipboard()->setText(edit_text_.mid(s, std::abs(caret_pos_ - selection_anchor_)));
                delete_selection();
                touch_caret();
            }
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            QString clip = QApplication::clipboard()->text();
            clip.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            clip.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            if (!clip.isEmpty()) {
                if (has_selection) delete_selection();
                edit_text_.insert(caret_pos_, clip);
                caret_pos_ += static_cast<int>(clip.size());
                selection_anchor_ = caret_pos_;
                touch_caret();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left) {
            if (ctrl) {
                caret_pos_ = 0;
            } else if (alt) {
                caret_pos_ = prev_word_boundary(edit_text_, caret_pos_);
            } else if (has_selection && !shift) {
                caret_pos_ = std::min(caret_pos_, selection_anchor_);
            } else if (caret_pos_ > 0) {
                --caret_pos_;
            }
            if (!shift) selection_anchor_ = caret_pos_;
            caret_visible_ = true;
            caret_blink_timer_->start();
            updateEditingLane();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            const int n = static_cast<int>(edit_text_.size());
            if (ctrl) {
                caret_pos_ = n;
            } else if (alt) {
                caret_pos_ = next_word_boundary(edit_text_, caret_pos_);
            } else if (has_selection && !shift) {
                caret_pos_ = std::max(caret_pos_, selection_anchor_);
            } else if (caret_pos_ < n) {
                ++caret_pos_;
            }
            if (!shift) selection_anchor_ = caret_pos_;
            caret_visible_ = true;
            caret_blink_timer_->start();
            updateEditingLane();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Home || event->key() == Qt::Key_Up) {
            caret_pos_ = 0;
            if (!shift) selection_anchor_ = 0;
            caret_visible_ = true;
            updateEditingLane();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_End || event->key() == Qt::Key_Down) {
            caret_pos_ = static_cast<int>(edit_text_.size());
            if (!shift) selection_anchor_ = caret_pos_;
            caret_visible_ = true;
            updateEditingLane();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            if (has_selection) {
                delete_selection();
                touch_caret();
            } else if (caret_pos_ > 0) {
                const int target = ctrl ? 0 : (alt ? prev_word_boundary(edit_text_, caret_pos_) : caret_pos_ - 1);
                edit_text_.remove(target, caret_pos_ - target);
                caret_pos_ = target;
                selection_anchor_ = target;
                touch_caret();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete) {
            const int n = static_cast<int>(edit_text_.size());
            if (has_selection) {
                delete_selection();
                touch_caret();
            } else if (caret_pos_ < n) {
                const int target = alt ? next_word_boundary(edit_text_, caret_pos_) : caret_pos_ + 1;
                edit_text_.remove(caret_pos_, target - caret_pos_);
                selection_anchor_ = caret_pos_;
                touch_caret();
            }
            event->accept();
            return;
        }

        const QString typed = event->text();
        if (!typed.isEmpty() && !ctrl && !event->modifiers().testFlag(Qt::MetaModifier)) {
            bool printable = true;
            for (const QChar c : typed) {
                if (c.unicode() < 0x20 && c != QLatin1Char('\t')) { printable = false; break; }
            }
            if (printable) {
                if (has_selection) delete_selection();
                edit_text_.insert(caret_pos_, typed);
                caret_pos_ += static_cast<int>(typed.size());
                selection_anchor_ = caret_pos_;
                touch_caret();
                event->accept();
                return;
            }
        }
        event->accept();
        return;
    }

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
    if (event->key() == Qt::Key_F2 && selected_line_ >= 0) {
        last_edit_click_x_ = -1;
        last_edit_click_y_ = -1;
        if (edit_handler_) edit_handler_(selected_line_, selected_cue_);
        else beginInlineEdit(selected_line_, selected_cue_);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        selected_line_ >= 0 && delete_handler_) {
        delete_handler_(selected_line_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Plus ||
        (event->key() == Qt::Key_Equal && (event->modifiers() == Qt::NoModifier || event->modifiers().testFlag(Qt::ShiftModifier)))) {
        adjustTimelineZoom(1.12);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore) {
        adjustTimelineZoom(1.0 / 1.12);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (selected_line_ >= 0) {
            last_edit_click_x_ = -1;
            last_edit_click_y_ = -1;
            if (edit_handler_) edit_handler_(selected_line_, selected_cue_);
            else beginInlineEdit(selected_line_, selected_cue_);
        } else if (insert_at_handler_) {
            insert_at_handler_(selected_track_, playhead_ms_);
        }
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
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const int dy = event->pixelDelta().y() != 0 ? event->pixelDelta().y() * 3 : event->angleDelta().y();
        if (dy != 0) {
            adjustTimelineZoom(std::pow(1.12, static_cast<qreal>(dy) / 120.0));
            event->accept();
            return;
        }
    }
    const QPoint pixel_delta = event->pixelDelta();
    if (!pixel_delta.isNull() || event->phase() != Qt::NoScrollPhase) {
        const QPoint angle_delta = event->angleDelta();
        const int horizontal_delta = pixel_delta.x() != 0 ? pixel_delta.x() : angle_delta.x() / 8;
        const int delta = horizontal_delta != 0 ? horizontal_delta
            : (pixel_delta.y() != 0 ? -pixel_delta.y() : -angle_delta.y() / 8);
        if (delta != 0) {
            if (!wheel_scrub_active_) {
                wheel_scrub_active_ = true;
                trackpad_pan_accum_ms_ = 0.0;
                if (playhead_drag_handler_) playhead_drag_handler_(true);
            }
            if (wheel_scrub_end_timer_) wheel_scrub_end_timer_->start();
            panTimelineByPixels(delta);
        } else if (event->phase() == Qt::ScrollEnd && wheel_scrub_active_) {
            if (wheel_scrub_end_timer_) wheel_scrub_end_timer_->start(60);
        }
        event->accept();
        return;
    }
    // Shift+Wheel on mouse also pans timeline & scrubs video across Windows/Linux!
    if (event->modifiers().testFlag(Qt::ShiftModifier) || event->angleDelta().x() != 0) {
        const int delta = event->angleDelta().x() != 0 ? event->angleDelta().x() / 3 : -event->angleDelta().y() / 3;
        if (delta != 0) {
            if (!wheel_scrub_active_) {
                wheel_scrub_active_ = true;
                trackpad_pan_accum_ms_ = 0.0;
                if (playhead_drag_handler_) playhead_drag_handler_(true);
            }
            if (wheel_scrub_end_timer_) wheel_scrub_end_timer_->start();
            panTimelineByPixels(delta);
        }
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
    setVisibleDurationMs(qRound64(visible_duration_ms_ / factor));
}

void RhythmTimelineWidget::panTimelineByPixels(int delta) {
    if (!playhead_handler_ || delta == 0) return;
    const int plot_width = std::max(1, width() - left_margin - right_margin);
    const double exact_delta_ms = static_cast<double>(delta) * visible_duration_ms_ / plot_width +
                                  trackpad_pan_accum_ms_;
    const qint64 step_ms = qRound64(exact_delta_ms);
    trackpad_pan_accum_ms_ = exact_delta_ms - static_cast<double>(step_ms);
    if (step_ms != 0) playhead_handler_(std::max<qint64>(0, playhead_ms_ - step_ms));
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
    if (editing_line_ >= 0) finishInlineEdit(true);
    const int x = event->pos().x();
    const int y = event->pos().y();
    const int lane_height = std::max(1, (height() - ruler_height) / 4);
    if (y >= ruler_height && y < height()) {
        selected_track_ = std::clamp((y - ruler_height) / lane_height, 0, 3);
        const auto cue = x >= left_margin ? cueAtPosition(x, y) : std::pair<int, int>{-1, -1};
        selected_line_ = cue.first >= 0 ? cue.first : (x >= left_margin ? lineAtPosition(x, y) : -1);
        selected_cue_ = cue.first == selected_line_ ? cue.second : -1;
        selected_edge_line_ = -1;
        selected_edge_sign_ = -1;
        if (selection_handler_) selection_handler_(selected_line_, selected_track_);
    }
    QMenu menu(this);
    if (selected_line_ >= 0) {
        auto* edit = menu.addAction(QStringLiteral("Modifier le texte sur la bande"));
        connect(edit, &QAction::triggered, this, [this] {
            last_edit_click_x_ = -1;
            last_edit_click_y_ = -1;
            if (edit_handler_) edit_handler_(selected_line_, selected_cue_);
            else beginInlineEdit(selected_line_, selected_cue_);
        });
        if (split_handler_) {
            auto* split = menu.addAction(QStringLiteral("Scinder la réplique…"));
            connect(split, &QAction::triggered, this, [this] { split_handler_(selected_line_); });
        }
        if (delete_handler_) {
            menu.addSeparator();
            auto* remove = menu.addAction(QStringLiteral("Supprimer la réplique"));
            connect(remove, &QAction::triggered, this, [this] { delete_handler_(selected_line_); });
        }
    } else if (y >= ruler_height && y < height() && x >= left_margin && insert_at_handler_) {
        auto* insert = menu.addAction(QStringLiteral("Ajouter une réplique ici"));
        const int track = selected_track_;
        const qint64 position_ms = std::max<qint64>(0, positionForX(x));
        connect(insert, &QAction::triggered, this, [this, track, position_ms] {
            insert_at_handler_(track, position_ms);
        });
    }
    if (!menu.isEmpty()) menu.exec(event->globalPos());
}

} // namespace rythmo
