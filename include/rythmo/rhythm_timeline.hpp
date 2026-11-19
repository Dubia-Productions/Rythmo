#pragma once

#include "rythmo/detx.hpp"

#include <QWidget>
#include <QFont>
#include <QColor>
#include <QHash>
#include <QPixmap>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QInputMethodEvent;
class QFocusEvent;
class QContextMenuEvent;
class QWheelEvent;
class QResizeEvent;
class QTextLayout;
class QTimer;
class QEvent;

namespace rythmo {

class RhythmTimelineWidget final : public QWidget {
public:
    explicit RhythmTimelineWidget(QWidget* parent = nullptr);
    void setDocument(const DetxDocument* document, Timecode video_start, int timecode_fps,
                     double media_fps, qint64 duration_ms);
    void updateLineCache(int line_index);
    void setPlayhead(qint64 position_ms);
    void setPlayheadForRender(qint64 position_ms);
    void setSelectionHandler(std::function<void(int, int)> handler);
    void setInsertAtHandler(std::function<void(int, qint64)> handler);
    void setResizeHandler(std::function<void(int, int, qint64)> handler);
    void setResizeTransactionHandler(std::function<void(bool)> handler);
    void setSplitHandler(std::function<void(int)> handler);
    void setEditHandler(std::function<void(int, int)> handler);
    void setTextEditHandler(std::function<void(int, std::size_t, std::size_t, const QString&)> handler);
    void beginInlineEdit(int line_index, int cue_index = -1, int click_x = -1, int click_y = -1);
    void commitOrCancelEdit(bool commit);
    bool isEditingText() const noexcept { return editing_line_ >= 0; }
    void setDeleteHandler(std::function<void(int)> handler);
    void setSelection(int line_index, int track);
    void copyDisplayStateFrom(const RhythmTimelineWidget& other);
    void setPlayheadHandler(std::function<void(qint64)> handler);
    void setPlayheadDragHandler(std::function<void(bool)> handler);
    void setZoomChangedHandler(std::function<void(qint64)> handler);
    void setTextFont(const QFont& font);
    void setCleanPresentation(bool enabled);
    void navigateToAdjacentCue(int direction);
    void stepFrame(int direction);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setVisibleDurationMs(qint64 duration_ms);
    qint64 visibleDurationMs() const noexcept { return visible_duration_ms_; }
    int selectedTrack() const noexcept { return selected_track_; }
    int selectedLine() const noexcept { return selected_line_; }
    qint64 positionForTimecode(const Timecode& timecode) const;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct CachedShapedRow {
        qreal natural_width{1.0};
        qreal line_height{18.0};
        std::shared_ptr<QTextLayout> layout;
    };
    struct CachedCue {
        qint64 start_ms{};
        qint64 end_ms{};
        bool start_follows_playhead{};
        bool end_follows_playhead{};
        QStringList rows;
        QString text;
        std::size_t first_segment{};
        std::size_t segment_count{};
        int cached_natural_px{-1};
        qreal cached_ascent{};
        qreal cached_font_height{};
        std::vector<CachedShapedRow> shaped_rows;
        QPixmap cached_content_pixmap;
        QSize cached_block_size;
        qreal cached_pixmap_dpr{};
    };
    struct CachedSign {
        qint64 position_ms{};
        QColor color;
    };
    struct CachedLine {
        int track{-1};
        qint64 min_ms{};
        qint64 max_ms{};
        bool dynamic_bounds{};
        QString role;
        QColor role_color;
        std::vector<CachedCue> cues;
        std::vector<CachedSign> signs;
    };
    struct CachedMarker {
        qint64 position_ms{};
        bool loop{};
    };
    struct CueRowStretch {
        int pixel_size{20};
        qreal stretch_x{1.0};
        qreal line_height{20.0};
        const QTextLayout* layout{};
    };

    const DetxDocument* document_{};
    std::vector<CachedLine> cached_lines_;
    std::vector<CachedMarker> cached_markers_;
    std::vector<qint64> sorted_nav_targets_;
    int active_pixmap_cues_{};
    Timecode video_start_{};
    int timecode_fps_{25};
    double media_fps_{25.0};
    qint64 duration_ms_{};
    qint64 playhead_ms_{};
    int selected_line_{-1};
    int selected_cue_{-1};
    int selected_track_{};
    int hovered_line_{-1};
    int hovered_edge_line_{-1};
    int hovered_edge_sign_{-1};
    bool hovered_playhead_{};
    std::function<void(int, int)> selection_handler_;
    std::function<void(int, qint64)> insert_at_handler_;
    std::function<void(int, int, qint64)> resize_handler_;
    std::function<void(bool)> resize_transaction_handler_;
    std::function<void(qint64)> playhead_handler_;
    std::function<void(bool)> playhead_drag_handler_;
    std::function<void(qint64)> zoom_changed_handler_;
    std::function<void(int)> split_handler_;
    std::function<void(int, int)> edit_handler_;
    std::function<void(int, std::size_t, std::size_t, const QString&)> text_edit_handler_;
    std::function<void(int)> delete_handler_;
    int resizing_line_{-1};
    int resizing_sign_{-1};
    int selected_edge_line_{-1};
    int selected_edge_sign_{-1};
    int resize_start_x_{};
    qint64 resize_start_position_ms_{};
    bool scrubbing_playhead_{};
    bool clean_presentation_{};
    int scrub_start_x_{};
    qint64 scrub_start_position_ms_{};
    qint64 visible_duration_ms_{8000};
    int pending_click_edit_line_{-1};
    int pending_click_edit_cue_{-1};
    QPoint pending_click_pos_;
    QFont text_font_;
    QFont ruler_font_;
    QFont role_font_;
    QFont track_font_;

    // Pre-rendered static UI chrome & ruler label caches
    QPixmap cached_lanes_pixmap_;
    QPixmap cached_sidebar_pixmap_;
    QSize cached_sidebar_size_;
    qreal cached_sidebar_dpr_{};
    int cached_sidebar_selected_track_{-2};
    bool cached_sidebar_clean_{};
    QHash<qint64, QPixmap> ruler_label_cache_;
    qreal ruler_cache_dpr_{};

    // Sub-pixel trackpad pan state
    QTimer* wheel_scrub_end_timer_{};
    double trackpad_pan_accum_ms_{};
    bool wheel_scrub_active_{};

    // Direct in-band canvas text editing state (zero popup QLineEdit)
    QTimer* caret_blink_timer_{};
    int editing_line_{-1};
    int editing_cue_{-1};
    std::size_t editing_first_segment_{};
    std::size_t editing_segment_count_{};
    int editing_track_{};
    QString edit_original_text_;
    QString edit_text_;
    QString preedit_text_;
    int caret_pos_{};
    int selection_anchor_{};
    bool caret_visible_{true};
    bool dragging_text_selection_{};
    int last_edit_click_x_{-1};
    int last_edit_click_y_{-1};

    CachedLine buildCachedLine(const Line& line) const;
    void rebuildNavTargets();
    void evictDistantPixmaps(qint64 visible_start_ms, qint64 visible_end_ms);
    qint64 effectiveDuration() const;
    void rebuildSceneCache();
    qint64 positionForX(int x) const;
    int xForPosition(qint64 position_ms) const;
    int lineAtPosition(int x, int y) const;
    std::pair<int, int> cueAtPosition(int x, int y) const;
    std::pair<int, int> edgeAtPosition(int x, int y) const;
    QRectF cueBlockRectF(const CachedCue& cue, int track) const;
    QRectF cueTextRectF(const QRectF& block) const;
    int uniformPixelSize(int block_height, int row_count) const;
    void ensureShapedMetrics(CachedCue& cue, int pixel_size);
    CueRowStretch computeRowStretch(CachedCue& cue, int row, int block_height, int row_count,
                                    qreal avail_width, bool is_editing);
    int hitTestEditCursor(int x, int y);
    void syncLiveEditToCache();
    void updateEditingLane();
    void finishInlineEdit(bool commit);
    void editAdjacentCue(int direction);
    void adjustTimelineZoom(qreal factor);
    void panTimelineByPixels(int delta);
    void stepTimelineFrame(int direction);
};

} // namespace rythmo
