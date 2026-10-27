#pragma once

#include "rythmo/detx.hpp"

#include <QMainWindow>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>

#include <memory>
#include <string>
#include <vector>

class QAudioOutput;
class QMediaPlayer;
class QVideoWidget;
class QTableWidget;
class QSlider;
class QLabel;
class QPushButton;
class QCloseEvent;
class QTimer;
class QVBoxLayout;
class QWidget;
class QProgressDialog;
class QStackedWidget;
class QListWidget;
class QEvent;
class QListWidgetItem;
class QFont;
class QPushButton;

namespace rythmo {
class RhythmTimelineWidget;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const QString& initial_document = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::unique_ptr<DetxDocument> document_;
    QString document_path_;
    QString video_path_;
    QMediaPlayer* player_{};
    QAudioOutput* audio_output_{};
    QVideoWidget* video_widget_{};
    QTableWidget* lines_table_{};
    RhythmTimelineWidget* timeline_{};
    QSlider* position_slider_{};
    QTimer* playback_visual_timer_{};
    QTimer* export_frame_timer_{};
    QProcess* export_process_{};
    QProcess* decode_fallback_process_{};
    QProcess* decode_encoder_probe_process_{};
    QProgressDialog* export_progress_{};
    RhythmTimelineWidget* export_timeline_{};
    QWidget* full_screen_host_{};
    QWidget* full_screen_video_canvas_{};
    QWidget* editor_widget_{};
    QWidget* home_widget_{};
    QWidget* home_recent_panel_{};
    QWidget* projects_widget_{};
    QStackedWidget* page_stack_{};
    QListWidget* recent_projects_list_{};
    QLabel* recent_projects_empty_{};
    QLabel* projects_title_{};
    QPushButton* project_continue_button_{};
    QVBoxLayout* full_screen_layout_{};
    QVBoxLayout* right_layout_{};
    QLabel* status_label_{};
    QLabel* timecode_label_{};
    QPushButton* play_button_{};
    bool populating_table_{};
    bool dirty_{};
    bool full_screen_active_{};
    bool export_active_{};
    bool export_pumping_{};
    bool export_cancelled_{};
    bool decode_fallback_attempted_{};
    bool decode_fallback_running_{};
    bool decode_proxy_active_{};
    bool resume_after_timeline_scrub_{};
    bool playback_seek_pending_{};
    qint64 export_frame_index_{};
    qint64 export_frame_count_{};
    qint64 playback_anchor_position_ms_{};
    qint64 current_position_ms_{};
    int export_frame_width_{};
    int export_video_height_{};
    int export_band_height_{};
    bool export_effects_enabled_{true};
    QImage export_frame_image_;
    double export_fps_{30.0};
    QString export_output_path_;
    QString export_temp_path_;
    QString export_backdrop_overlay_path_;
    QString export_log_tail_;
    QString export_encoder_;
    QString decode_proxy_path_;
    QString ffmpeg_path_;
    QString decode_log_tail_;
    QTemporaryDir decode_temp_dir_;
    QTemporaryDir export_temp_dir_;
    QElapsedTimer playback_clock_;
    QElapsedTimer export_progress_clock_;
    double media_fps_{};
    int timecode_fps_{25};
    int selected_timeline_track_{};
    Timecode video_start_{};
    QString active_project_module_;
    QString imported_timeline_font_path_;
    std::vector<std::string> undo_history_;
    std::vector<std::string> redo_history_;
    std::string current_document_snapshot_;
    std::string saved_document_snapshot_;
    bool undo_snapshot_initialized_{};
    bool restoring_history_{};
    bool resizing_timeline_edge_{};
    std::string timeline_resize_snapshot_;

    void createUi();
    void showHome();
    void showProjectPage(const QString& module);
    void openRecentProject(const QString& path);
    void refreshRecentProjects();
    void rememberRecentProject(const QString& path);
    void chooseTimelineFont();
    void showEditor();
    void openStudioModule(const QString& module);
    void openVoiceSeparation();
    void createMenus();
    void openDocument();
    void saveDocument();
    bool saveDocumentAs();
    bool confirmDiscardChanges();
    void loadDocument(const QString& path);
    void openVideo();
    void loadVideo(const QString& path, bool persist_reference = true);
    void startFfmpegDecodeFallback(const QString& qt_error);
    void startFfmpegDecodeProxy(const QString& encoders);
    void finishFfmpegDecodeFallback(int exit_code, QProcess::ExitStatus status);
    void refreshTable();
    void refreshTimeline();
    void onCellChanged(int row, int column);
    void addLine();
    void addLineAt(int track, qint64 position_ms);
    void editTimelineLine(int row);
    void splitSelectedLine();
    void addShotMarker();
    void toggleRhythmFullscreen();
    void exportVideoWithRhythm();
    void writeNextExportFrame();
    void finishVideoExport(int exit_code, QProcess::ExitStatus status);
    void removeSelectedLine();
    void addRole();
    void manageRoles();
    void updateMediaMetadata();
    void updatePosition(qint64 position_ms);
    void seekTimeline(qint64 position_ms);
    void seekFromTableRow(int row);
    Timecode currentTimecode() const;
    void setDirty(bool dirty);
    void resetUndoHistory(bool saved_state);
    void undoLastChange();
    void redoLastChange();
};

} // namespace rythmo
