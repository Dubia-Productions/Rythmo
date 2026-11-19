#pragma once

#include "rythmo/detx.hpp"

#include <QMainWindow>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>

#include <array>
#include <memory>
#include <string>
#include <vector>

class QAudioOutput;
class QMediaPlayer;
class QVideoWidget;
class QTableWidget;
class QSlider;
class QLabel;
class QLineEdit;
class QPushButton;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QTimer;
class QVBoxLayout;
class QHBoxLayout;
class QSpacerItem;
class QWidget;
class QProgressDialog;
class QStackedWidget;
class QListWidget;
class QEvent;
class QListWidgetItem;
class QFont;

namespace rythmo {
class RhythmTimelineWidget;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const QString& initial_document = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::unique_ptr<DetxDocument> document_;
    QString document_path_;
    QString video_path_;
    QMediaPlayer* player_{};
    QAudioOutput* audio_output_{};
    QVideoWidget* video_widget_{};
    QWidget* empty_video_page_{};
    QWidget* video_host_{};
    QVBoxLayout* video_host_layout_{};
    QWidget* timeline_host_{};
    QVBoxLayout* timeline_host_layout_{};
    QStackedWidget* table_stack_{};
    QTableWidget* lines_table_{};
    QLineEdit* lines_search_edit_{};
    QLabel* lines_empty_title_{};
    QLabel* lines_empty_subtitle_{};
    RhythmTimelineWidget* timeline_{};
    QSlider* position_slider_{};
    QSlider* volume_slider_{};
    QPushButton* mute_button_{};
    QPushButton* video_source_button_{};
    QPushButton* roles_button_{};
    QPushButton* zoom_reset_button_{};
    QPushButton* home_resume_button_{};
    std::array<QPushButton*, 4> track_buttons_{};
    QLabel* document_title_label_{};
    QLabel* project_meta_label_{};
    QLabel* lines_count_badge_{};
    QTimer* playback_visual_timer_{};
    QTimer* seek_coalesce_timer_{};
    QTimer* video_decode_watchdog_timer_{};
    QTimer* export_frame_timer_{};
    QProcess* export_process_{};
    QProcess* decode_fallback_process_{};
    QProcess* decode_encoder_probe_process_{};
    QProcess* poster_frame_process_{};
    QProgressDialog* export_progress_{};
    RhythmTimelineWidget* export_timeline_{};
    QWidget* full_screen_host_{};
    QWidget* full_screen_video_canvas_{};
    QPushButton* full_screen_exit_button_{};
    QTimer* full_screen_hud_timer_{};
    QPoint last_fullscreen_mouse_pos_{-1, -1};
    int last_slider_permille_{-1};
    Timecode last_displayed_timecode_{-1, -1, -1, -1};
    QWidget* editor_widget_{};
    QWidget* home_widget_{};
    QVBoxLayout* home_outer_layout_{};
    QHBoxLayout* home_columns_layout_{};
    QWidget* home_welcome_frame_{};
    QWidget* home_left_col_{};
    QLabel* home_logo_label_{};
    QLabel* home_wordmark_label_{};
    QLabel* home_version_label_{};
    QSpacerItem* home_brand_spacer_{};
    QPushButton* home_new_btn_{};
    QPushButton* home_open_btn_{};
    QPushButton* home_voice_btn_{};
    QLabel* home_recent_heading_{};
    QPushButton* home_view_all_btn_{};
    QWidget* home_recent_panel_{};
    qreal last_home_scale_{-1.0};
    QWidget* projects_widget_{};
    QWidget* projects_container_{};
    QStackedWidget* page_stack_{};
    QListWidget* recent_projects_list_{};
    QLineEdit* projects_search_edit_{};
    QLabel* recent_projects_empty_{};
    QLabel* projects_title_{};
    QPushButton* project_continue_button_{};
    QVBoxLayout* full_screen_layout_{};
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
    bool media_seek_in_flight_{};
    bool flush_seek_scheduled_{};
    bool fallback_resume_playing_{};
    bool fallback_resume_pending_{};
    bool decode_proxy_is_persistent_{};
    qint64 received_video_frames_{};
    qint64 fallback_resume_pos_ms_{};
    qint64 last_dispatched_seek_ms_{-1};
    qint64 last_dispatched_seek_frame_{-1};
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
    QString decode_proxy_final_path_;
    QStringList decode_proxy_candidates_;
    QString decode_proxy_audio_encoder_;
    QString ffmpeg_path_;
    QString decode_log_tail_;
    QTemporaryDir decode_temp_dir_;
    QTemporaryDir export_temp_dir_;
    QElapsedTimer playback_clock_;
    QElapsedTimer last_media_seek_clock_;
    QElapsedTimer media_seek_flight_clock_;
    QElapsedTimer export_progress_clock_;
    qint64 pending_media_seek_ms_{-1};
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
    void createNewDocument(bool prompt_video = true);
    void updateEditorHeader();
    void updateTrackPills();
    void applyLinesFilter();
    void showHome();
    void rescaleHomePage();
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
    void requestInstantPosterFrame(const QString& path, qint64 position_ms);
    void startFfmpegDecodeFallback(const QString& qt_error);
    void startFfmpegDecodeProxy(const QString& encoders);
    void launchNextDecodeProxyCandidate();
    void finishFfmpegDecodeFallback(int exit_code, QProcess::ExitStatus status);
    void dispatchMediaSeek(qint64 target_ms, bool force_exact);
    void flushPendingMediaSeek();
    void refreshTable();
    void updateTableRow(int row);
    void positionFullscreenHud();
    void refreshTimeline();
    void onCellChanged(int row, int column);
    void addLine();
    void addLineAt(int track, qint64 position_ms);
    void editTimelineLine(int row, int cue_index = -1);
    void splitSelectedLine();
    void addShotMarker();
    void shiftDocumentTimecodes();
    void showDocumentDiagnostics();
    void showShortcutsDialog();
    void toggleRhythmFullscreen();
    void exportVideoWithRhythm();
    void writeNextExportFrame();
    void finishVideoExport(int exit_code, QProcess::ExitStatus status);
    void removeSelectedLine();
    void addRole();
    void manageRoles();
    void updateMediaMetadata();
    void togglePlayPause();
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
