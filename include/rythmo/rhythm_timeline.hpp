#pragma once

#include "rythmo/detx.hpp"

#include <QWidget>
#include <QFont>
#include <QColor>
#include <QStringList>

#include <functional>
#include <map>
#include <utility>
#include <vector>

class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;
class QWheelEvent;

namespace rythmo {

class RhythmTimelineWidget final : public QWidget {
public:
    explicit RhythmTimelineWidget(QWidget* parent = nullptr);
    void setDocument(const DetxDocument* document, Timecode video_start, int timecode_fps,
                     double media_fps, qint64 duration_ms);
    void setPlayhead(qint64 position_ms);
    void setPlayheadForRender(qint64 position_ms);
    void setSelectionHandler(std::function<void(int, int)> handler);
    void setInsertAtHandler(std::function<void(int, qint64)> handler);
    void setResizeHandler(std::function<void(int, int, qint64)> handler);
    void setResizeTransactionHandler(std::function<void(bool)> handler);
    void setSplitHandler(std::function<void(int)> handler);
    void setEditHandler(std::function<void(int)> handler);
    void setDeleteHandler(std::function<void(int)> handler);
    void setSelection(int line_index, int track);
    void copyDisplayStateFrom(const RhythmTimelineWidget& other);
    void setPlayheadHandler(std::function<void(qint64)> handler);
    void setPlayheadDragHandler(std::function<void(bool)> handler);
    void setTextFont(const QFont& font);
    void setCleanPresentation(bool enabled);
    void navigateToAdjacentCue(int direction);
    qint64 positionForTimecode(const Timecode& timecode) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    struct CachedCue {
        qint64 start_ms{};
        qint64 end_ms{};
        bool start_follows_playhead{};
        bool end_follows_playhead{};
        QStringList rows;
        std::map<int, std::vector<qreal>> text_widths_by_pixel_size;
    };
    struct CachedSign {
        qint64 position_ms{};
        QColor color;
    };
    struct CachedLine {
        int track{-1};
        QString role;
        QColor role_color;
        std::vector<CachedCue> cues;
        std::vector<CachedSign> signs;
    };
    struct CachedMarker {
        qint64 position_ms{};
        bool loop{};
    };

    const DetxDocument* document_{};
    std::vector<CachedLine> cached_lines_;
    std::vector<CachedMarker> cached_markers_;
    Timecode video_start_{};
    int timecode_fps_{25};
    double media_fps_{25.0};
    qint64 duration_ms_{};
    qint64 playhead_ms_{};
    int selected_line_{-1};
    int selected_track_{};
    std::function<void(int, int)> selection_handler_;
    std::function<void(int, qint64)> insert_at_handler_;
    std::function<void(int, int, qint64)> resize_handler_;
    std::function<void(bool)> resize_transaction_handler_;
    std::function<void(qint64)> playhead_handler_;
    std::function<void(bool)> playhead_drag_handler_;
    std::function<void(int)> split_handler_;
    std::function<void(int)> edit_handler_;
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
    QFont text_font_{QStringLiteral("Montserrat")};

    qint64 effectiveDuration() const;
    void rebuildSceneCache();
    qint64 positionForX(int x) const;
    int xForPosition(qint64 position_ms) const;
    int lineAtPosition(int x, int y) const;
    std::pair<int, int> edgeAtPosition(int x, int y) const;
    void adjustTimelineZoom(qreal factor);
    void panTimelineByPixels(int delta);
    void stepTimelineFrame(int direction);
};

} // namespace rythmo
