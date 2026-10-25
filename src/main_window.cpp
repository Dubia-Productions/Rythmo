#include "rythmo/main_window.hpp"
#include "rythmo/rhythm_timeline.hpp"
#include "rythmo/video_export_filter.hpp"
#include "rythmo/video_backdrop.hpp"

#include <QAudioOutput>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QFont>
#include <QFontDatabase>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMap>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProgressDialog>
#include <QProgressBar>
#include <QResizeEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSize>
#include <QSettings>
#include <QStyleFactory>
#include <QSysInfo>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSlider>
#include <QStackedWidget>
#include <QShortcut>
#include <QStatusBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QToolBar>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <vector>

namespace rythmo {
namespace {

QString qstr(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QString nativeToolName(const QString& base) {
#ifdef Q_OS_WIN
    return base + QStringLiteral(".exe");
#else
    return base;
#endif
}
QString findBundledOrSystemTool(const QString& base) {
    const QString name = nativeToolName(base);
    const QString app_dir = QCoreApplication::applicationDirPath();
    QStringList candidates{QDir(app_dir).filePath(name), QDir(app_dir).filePath(QStringLiteral("bin/") + name)};
#ifdef Q_OS_MACOS
    // Finder-launched apps do not reliably inherit Homebrew's shell PATH.
    // Check the standard Apple Silicon and Intel prefixes explicitly so the
    // Homebrew FFmpeg installed by the build script is discoverable.
    candidates << QDir(app_dir).filePath(QStringLiteral("../Resources/bin/") + name)
               << QStringLiteral("/opt/homebrew/bin/") + name
               << QStringLiteral("/usr/local/bin/") + name
               << QStringLiteral("/opt/local/bin/") + name;
#endif
    for (const auto& candidate : candidates)
        if (QFileInfo(candidate).isFile() && QFileInfo(candidate).isExecutable()) return candidate;
    return QStandardPaths::findExecutable(name);
}
std::filesystem::path fs_path(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::filesystem::u8path(bytes.constData());
}

void collect_text(const XmlNode& node, std::string& output) {
    for (const auto& part : node.content) {
        if (part.kind == XmlPart::Kind::Text) output += part.data;
        else if (part.kind == XmlPart::Kind::Element) collect_text(*part.element, output);
    }
}

QString first_timecode(const Line& line, bool start) {
    if (line.lipsyncs.empty()) return {};
    if (start) {
        for (const auto& sign : line.lipsyncs) if (sign.kind.rfind("in_", 0) == 0) return qstr(sign.timecode.format());
        return qstr(line.lipsyncs.front().timecode.format());
    }
    for (auto it = line.lipsyncs.rbegin(); it != line.lipsyncs.rend(); ++it)
        if (it->kind.rfind("out_", 0) == 0) return qstr(it->timecode.format());
    return qstr(line.lipsyncs.back().timecode.format());
}

QString role_name(const DetxDocument& doc, const std::string& id) {
    for (const auto& role : doc.roles()) if (role.id == id) return qstr(role.name);
    return qstr(id);
}

std::optional<Timecode> video_start_timecode(const DetxDocument& doc) {
    const auto* header = doc.root().first_child("header");
    if (!header) return std::nullopt;
    const auto* video = header->first_child("videofile");
    if (!video) return std::nullopt;
    const auto raw = video->attribute("timestamp");
    if (!raw) return std::nullopt;
    try { return Timecode::parse(*raw); } catch (...) { return std::nullopt; }
}

QString video_path_from_document(const DetxDocument& doc) {
    const auto* header = doc.root().first_child("header");
    const auto* video = header ? header->first_child("videofile") : nullptr;
    if (!video) return {};
    std::string raw; collect_text(*video, raw);
    return qstr(raw).trimmed();
}

QString detect_graphics_adapters() {
#ifdef Q_OS_WIN
    const QString program = QStringLiteral("powershell.exe");
    const QStringList args{QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
        QStringLiteral("Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name")};
#elif defined(Q_OS_MACOS)
    const QString program = QStringLiteral("system_profiler");
    const QStringList args{QStringLiteral("SPDisplaysDataType")};
#else
    const QString program = QStringLiteral("lspci");
    const QStringList args{};
#endif
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(2000) || !process.waitForFinished(5000) || process.exitCode() != 0)
        return QStringLiteral("Modèle GPU non détecté automatiquement");
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    QStringList devices;
    for (const auto& line : output.split('\n')) {
        if (line.contains(QRegularExpression(QStringLiteral("VGA|3D controller|Display controller"),
                                             QRegularExpression::CaseInsensitiveOption)))
            devices.push_back(line.trimmed());
    }
    output = devices.join(QStringLiteral("\n"));
#endif
    return output.isEmpty() ? QStringLiteral("Modèle GPU non détecté automatiquement") : output;
}


class FullscreenVideoCanvas final : public QWidget {
public:
    explicit FullscreenVideoCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumHeight(200);
    }

    void setForegroundVideo(QVideoWidget* video) {
        if (frame_connection_) disconnect(frame_connection_);
        foreground_video_ = video;
        if (foreground_video_) {
            foreground_video_->setParent(this);
            foreground_video_->setAspectRatioMode(Qt::KeepAspectRatio);
            foreground_video_->show();
            layoutForeground();
            rebuildEffectOverlay();
            foreground_video_->raise();
            QVideoSink* sink = foreground_video_->videoSink();
            frame_connection_ = connect(sink, &QVideoSink::videoFrameChanged,
                                        this, [this](const QVideoFrame& frame) { updateFromFrame(frame); });
            const QVideoFrame current_frame = sink->videoFrame();
            if (current_frame.isValid()) updateFromFrame(current_frame);
        }
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        rebuildBackdrop();
        rebuildEffectOverlay();
        layoutForeground();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.fillRect(rect(), Qt::black);
        if (!blurred_frame_.isNull()) {
            painter.drawImage(rect(), blurred_frame_);
            if (!effect_overlay_.isNull()) painter.drawImage(rect(), effect_overlay_);
        }
    }

private:
    QMetaObject::Connection frame_connection_;
    QImage blurred_frame_;
    QImage effect_overlay_;
    QImage source_frame_;
    QSize source_size_{16, 9};
    QVideoWidget* foreground_video_{};
    QElapsedTimer backdrop_clock_;

    void updateFromFrame(const QVideoFrame& frame) {
#ifdef Q_OS_WIN
        // The fullscreen background is decorative. Do not convert and blur
        // every decoded frame on the GUI thread: doing so competes with the
        // foreground video and timeline, especially for 4K sources.
        if (backdrop_clock_.isValid() && backdrop_clock_.elapsed() < 90) return;
        backdrop_clock_.start();
#endif
        QImage image = frame.toImage();
        if (image.isNull()) return;
        const bool geometry_changed = source_size_ != image.size();
        source_size_ = image.size();
        source_frame_ = std::move(image);
        rebuildBackdrop();
        if (geometry_changed) {
            rebuildEffectOverlay();
            layoutForeground();
        }
        update();
    }

    void rebuildBackdrop() {
        if (source_frame_.isNull() || width() <= 0 || height() <= 0) return;
        // Work at one third of physical resolution, with a 1280-pixel ceiling.
        // A true blur hides no missing detail behind nearest-neighbor enlargement.
        const QSize target = fullscreenBackdropSize(size(), devicePixelRatioF());
        blurred_frame_ = softVideoBackdrop(source_frame_, target);
    }

    void rebuildEffectOverlay() {
        if (width() <= 0 || height() <= 0) return;
        effect_overlay_ = fullscreenVideoOverlay(size(), source_size_);
    }

    void layoutForeground() {
        if (!foreground_video_ || source_size_.isEmpty()) return;
        const qreal scale = std::min(width() / static_cast<qreal>(source_size_.width()),
                                     height() / static_cast<qreal>(source_size_.height()));
        const QSize fitted(qRound(source_size_.width() * scale), qRound(source_size_.height() * scale));
        const QRect geometry((width() - fitted.width()) / 2, (height() - fitted.height()) / 2,
                             fitted.width(), fitted.height());
        foreground_video_->setGeometry(geometry);
        foreground_video_->raise();
    }
};

} // namespace

MainWindow::MainWindow(const QString& initial_document, QWidget* parent) : QMainWindow(parent) {
    // Static Montserrat instances render consistently on Qt 6.5-6.6 Windows,
    // where variable-font axes are not handled by every font backend.
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-Bold.ttf"));
    QFont ui_font(QStringLiteral("Montserrat"), 10);
    ui_font.setStyleHint(QFont::SansSerif);
    ui_font.setHintingPreference(QFont::PreferFullHinting);
    qApp->setFont(ui_font);
    setStyleSheet(QStringLiteral(R"QSS(
        QWidget { background: #0B1118; color: #EEF3F8; font-family: "Montserrat"; font-size: 10pt; }
        QLabel { background: transparent; color: #EEF3F8; }
        QMainWindow, QDialog { background: #0B1118; }
        QMenuBar { background: #101821; border-bottom: 1px solid #263544; padding: 5px 10px; }
        QMenuBar::item { background: transparent; padding: 8px 12px; border-radius: 7px; }
        QMenuBar::item:selected { background: #1B2B39; color: #69CAF0; }
        QMenu { background: #151F29; border: 1px solid #304252; padding: 6px; }
        QMenu::item { padding: 8px 30px 8px 13px; border-radius: 6px; }
        QMenu::item:selected { background: #20384A; color: #F4FAFF; }
        QPushButton { background: #172330; border: 1px solid #304252; border-radius: 10px; padding: 9px 15px; color: #EEF3F8; font-weight: 600; }
        QPushButton:hover { background: #213243; border-color: #4AB7E5; }
        QPushButton:pressed { background: #121B24; }
        QPushButton:disabled { color: #78899A; background: #111922; border-color: #253341; }
        QTableWidget { background: #0F1720; alternate-background-color: #141E28; gridline-color: #263644; border: 1px solid #2A3A49; border-radius: 10px; selection-background-color: #1A3C51; selection-color: #F5FBFF; outline: 0; font-size: 10pt; }
        QTableWidget::item { padding: 8px; border: 0; }
        QTableWidget::item:hover { background: #1A2936; }
        QHeaderView::section { background: #16212C; color: #DCE7F0; border: 0; border-bottom: 1px solid #304252; padding: 10px 8px; font-weight: 700; }
        QLineEdit, QComboBox, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox { background: #121C26; border: 1px solid #304252; border-radius: 8px; padding: 8px; selection-background-color: #245B79; }
        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #4AB7E5; }
        QComboBox::drop-down { border: 0; width: 28px; }
        QComboBox QAbstractItemView { background: #151F29; color: #EEF3F8; border: 1px solid #304252; selection-background-color: #1A3C51; padding: 4px; }
        QGroupBox { border: 1px solid #2A3A49; border-radius: 11px; margin-top: 12px; padding: 12px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: #B9C8D5; }
        QTabWidget::pane { background: #111A24; border: 1px solid #2A3A49; border-radius: 9px; top: -1px; }
        QTabBar::tab { background: #121C26; color: #AEBCC9; border: 1px solid #2A3A49; padding: 8px 14px; margin-right: 4px; border-top-left-radius: 7px; border-top-right-radius: 7px; }
        QTabBar::tab:selected { background: #1A2B39; color: #69CAF0; border-bottom-color: #1A2B39; }
        QTabBar::tab:hover:!selected { background: #192735; }
        QCheckBox, QRadioButton { spacing: 8px; color: #DCE7F0; }
        QCheckBox::indicator, QRadioButton::indicator { width: 16px; height: 16px; }
        QCheckBox::indicator { border: 1px solid #506274; border-radius: 4px; background: #121C26; }
        QCheckBox::indicator:checked { background: #43B5E4; border-color: #43B5E4; }
        QRadioButton::indicator { border: 1px solid #506274; border-radius: 9px; background: #121C26; }
        QRadioButton::indicator:checked { background: #43B5E4; border: 4px solid #121C26; }
        QScrollBar:vertical { background: #101821; width: 12px; margin: 3px; border-radius: 6px; }
        QScrollBar::handle:vertical { background: #344656; min-height: 28px; border-radius: 5px; }
        QScrollBar::handle:vertical:hover { background: #4AB7E5; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: #101821; height: 12px; margin: 3px; border-radius: 6px; }
        QScrollBar::handle:horizontal { background: #344656; min-width: 28px; border-radius: 5px; }
        QScrollBar::handle:horizontal:hover { background: #4AB7E5; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QSlider::groove:horizontal { background: #334453; height: 5px; border-radius: 3px; }
        QSlider::sub-page:horizontal { background: #43B5E4; border-radius: 3px; }
        QSlider::handle:horizontal { background: #EAF8FF; border: 2px solid #318CB5; width: 14px; height: 14px; margin: -6px 0; border-radius: 8px; }
        QSplitter::handle { background: #1D2A36; width: 5px; }
        QSplitter::handle:hover { background: #3A9BC6; }
        QStatusBar { background: #101821; color: #A9B8C6; border-top: 1px solid #263544; }
        QLabel#timecodeBadge { color: #DDF6FF; background: #142C3A; border: 1px solid #347FA0; border-radius: 9px; padding: 7px 12px; font-weight: 700; }
        QToolTip { background: #172330; color: #EEF3F8; border: 1px solid #3B5365; border-radius: 6px; padding: 6px 8px; }
    )QSS"));
    createUi();
    createMenus();
    qApp->installEventFilter(this);
    setWindowTitle(QStringLiteral("Rythmo by Dubia Productions"));
    resize(1480, 900);
    if (!initial_document.isEmpty()) loadDocument(initial_document);
    else showHome();
}

MainWindow::~MainWindow() {
    if (decode_fallback_process_ && decode_fallback_process_->state() != QProcess::NotRunning) {
        decode_fallback_process_->kill();
        decode_fallback_process_->waitForFinished(2000);
    }
    if (player_) {
        player_->stop();
        player_->setSource(QUrl{});
    }
    if (!decode_proxy_path_.isEmpty()) QFile::remove(decode_proxy_path_);
}

void MainWindow::createUi() {
    player_ = new QMediaPlayer(this);
    audio_output_ = new QAudioOutput(this);
    playback_visual_timer_ = new QTimer(this);
    playback_visual_timer_->setInterval(16);
    playback_visual_timer_->setTimerType(Qt::PreciseTimer);
    player_->setAudioOutput(audio_output_);

    auto* root = new QWidget(this);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(6, 6, 6, 6);
    auto* actions = new QHBoxLayout;
    auto* add_line_button = new QPushButton("Ajouter une réplique", root);
    auto* delete_line_button = new QPushButton("Supprimer la sélection", root);
    auto* roles_button = new QPushButton("Voir les rôles", root);
    actions->addWidget(add_line_button);
    actions->addWidget(delete_line_button);
    actions->addWidget(roles_button);
    actions->addStretch(1);
    timecode_label_ = new QLabel("TC --:--:--:--", root);
    timecode_label_->setObjectName(QStringLiteral("timecodeBadge"));
    actions->addWidget(timecode_label_);
    root_layout->addLayout(actions);

    auto* main_splitter = new QSplitter(Qt::Horizontal, root);
    lines_table_ = new QTableWidget(0, 5, main_splitter);
    lines_table_->setHorizontalHeaderLabels({"Entrée", "Sortie", "Rôle", "Piste", "Texte"});
    lines_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lines_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Text is edited in its dedicated dialog, never in a small inline field that
    // covers the table contents. Single clicks remain selection-only.
    lines_table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    lines_table_->setTextElideMode(Qt::ElideRight);
    lines_table_->setAlternatingRowColors(true);
    lines_table_->verticalHeader()->setDefaultSectionSize(42);
    lines_table_->verticalHeader()->setVisible(false);
    lines_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    lines_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lines_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    lines_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    lines_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    lines_table_->setMinimumWidth(580);

    auto* right = new QWidget(main_splitter);
    right_layout_ = new QVBoxLayout(right);
    auto* right_layout = right_layout_;
    right_layout->setContentsMargins(0, 0, 0, 0);
    video_widget_ = new QVideoWidget(right);
    video_widget_->setMinimumHeight(250);
    video_widget_->setAspectRatioMode(Qt::KeepAspectRatio);
    player_->setVideoOutput(video_widget_);
    right_layout->addWidget(video_widget_, 3);

    timeline_ = new RhythmTimelineWidget(right);
    {
        QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
        imported_timeline_font_path_ = settings.value(QStringLiteral("timelineFontFile")).toString();
        if (!imported_timeline_font_path_.isEmpty() && QFileInfo::exists(imported_timeline_font_path_))
            QFontDatabase::addApplicationFont(imported_timeline_font_path_);
        QFont band_font(settings.value(QStringLiteral("timelineFontFamily"), QStringLiteral("Montserrat")).toString());
        band_font.setBold(true);
        timeline_->setTextFont(band_font);
    }
    right_layout->addWidget(timeline_, 2);
    auto* playback = new QHBoxLayout;
    play_button_ = new QPushButton("▶ Lire", right);
    play_button_->setFixedSize(112, 40);
    position_slider_ = new QSlider(Qt::Horizontal, right);
    position_slider_->setRange(0, 1000);
    auto* pick_video_button = new QPushButton("Ouvrir vidéo…", right);
    playback->addWidget(play_button_);
    playback->addWidget(position_slider_, 1);
    playback->addWidget(pick_video_button);
    right_layout->addLayout(playback);
    main_splitter->addWidget(lines_table_);
    main_splitter->addWidget(right);
    main_splitter->setStretchFactor(0, 5);
    main_splitter->setStretchFactor(1, 6);
    root_layout->addWidget(main_splitter, 1);
    editor_widget_ = root;
    page_stack_ = new QStackedWidget(this);
    page_stack_->addWidget(root);
    setCentralWidget(page_stack_);
    QTimer::singleShot(2500, this, [this] {
        if (QCoreApplication::applicationVersion().isEmpty()) return;
        if (!QNetworkInformation::loadDefaultBackend()) return;
        auto* network = QNetworkInformation::instance();
        if (!network || !network->supports(QNetworkInformation::Feature::Reachability)
            || network->reachability() != QNetworkInformation::Reachability::Online) return;
        auto* manager = new QNetworkAccessManager(this);
        QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/Dubia-Productions/Rythmo/releases/latest")));
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Rythmo/%1").arg(QCoreApplication::applicationVersion()));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setTransferTimeout(6000);
        auto* reply = manager->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, manager] {
            const auto network_error = reply->error();
            const QByteArray response = reply->readAll();
            reply->deleteLater();
            if (network_error != QNetworkReply::NoError) return;
            const auto latest = QJsonDocument::fromJson(response).object();
            const QString tag = latest.value(QStringLiteral("tag_name")).toString().trimmed();
            const auto assets = latest.value(QStringLiteral("assets")).toArray();
            auto parse_version = [](QString value) {
                value.remove(QRegularExpression(QStringLiteral("^[vV]")));
                const auto match = QRegularExpression(QStringLiteral("^(\\d+(?:\\.\\d+){1,3})")).match(value);
                return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber{};
            };
            const auto installed = parse_version(QCoreApplication::applicationVersion());
            const auto available = parse_version(tag);
            if (installed.isNull() || available.isNull() || QVersionNumber::compare(available, installed) <= 0) return;
            const QString running_app_path = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
            const QString platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
            const QString platform = QStringLiteral("macos");
#else
            const QString platform = QStringLiteral("linux");
#endif
            const QString architecture = QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"), Qt::CaseInsensitive)
                || QSysInfo::currentCpuArchitecture().contains(QStringLiteral("aarch64"), Qt::CaseInsensitive)
                ? QStringLiteral("arm64") : QStringLiteral("x64");
#ifdef Q_OS_LINUX
            if (!QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".rythmo-portable")))) {
                QMessageBox::information(this, QStringLiteral("Mise à jour"),
                    QStringLiteral("La mise à jour automatique est réservée à l’archive portable Rythmo. Installez cette mise à jour manuellement."));
                return;
            }
#endif
#ifdef Q_OS_LINUX
            const QString expected_asset = QStringLiteral("Rythmo-linux-%1-portable.tar.gz").arg(architecture);
#elif defined(Q_OS_MACOS)
            const QString expected_asset = QStringLiteral("Rythmo-macos-%1.zip").arg(architecture);
#else
            const QString expected_asset = QStringLiteral("Rythmo-windows-%1-portable.zip").arg(architecture);
#endif
            QString asset_url;
            QString asset_digest;
            for (const auto& value : assets) {
                const auto asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString() == expected_asset) {
                    asset_url = asset.value(QStringLiteral("browser_download_url")).toString();
                    asset_digest = asset.value(QStringLiteral("digest")).toString();
                    break;
                }
            }
            const QUrl download_url(asset_url);
            if (!download_url.isValid() || download_url.scheme() != QStringLiteral("https")
                || download_url.host() != QStringLiteral("github.com")) {
                QMessageBox::information(this, QStringLiteral("Mise à jour disponible"),
                    QStringLiteral("Rythmo %1 est disponible, mais le paquet de mise à jour pour %2 %3 n’est pas encore publié.")
                        .arg(tag, platform, architecture));
                return;
            }
            if (QMessageBox::question(this, QStringLiteral("Mise à jour disponible"),
                    QStringLiteral("Télécharger Rythmo %1 (%2), puis redémarrer l’application pour installer la mise à jour ?")
                        .arg(tag, expected_asset), QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) return;
            if (!confirmDiscardChanges()) return;

            QString app_data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (app_data_dir.isEmpty()) app_data_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            const QString update_dir = QDir(app_data_dir).filePath(QStringLiteral("updates"));
            if (app_data_dir.isEmpty() || !QDir().mkpath(update_dir)) {
                QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le dossier temporaire de mise à jour ne peut pas être créé."));
                return;
            }
            const QString package_path = QDir(update_dir).filePath(expected_asset);
            const QString partial_package_path = package_path + QStringLiteral(".download");
            QFile::remove(partial_package_path);
            auto* package = new QFile(partial_package_path, this);
            if (!package->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                delete package;
                QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le paquet ne peut pas être enregistré sur cet appareil."));
                return;
            }

            const Qt::WindowStates previous_window_state = windowState();
            auto* progress = new QProgressDialog(QStringLiteral("Téléchargement de Rythmo %1…").arg(tag),
                QStringLiteral("Annuler"), 0, 1000);
            progress->setWindowTitle(QStringLiteral("Mise à jour de Rythmo"));
            progress->setWindowModality(Qt::ApplicationModal);
            progress->setMinimumDuration(0);
            progress->setAutoClose(false);
            progress->setAutoReset(false);
            progress->setWindowFlag(Qt::WindowStaysOnTopHint, true);
            auto restore_main_window = [this, progress, previous_window_state] {
                progress->hide();
                progress->deleteLater();
                setWindowState(previous_window_state);
                show();
                raise();
                activateWindow();
            };
            QNetworkRequest package_request(download_url);
            package_request.setHeader(QNetworkRequest::UserAgentHeader,
                QStringLiteral("Rythmo/%1").arg(QCoreApplication::applicationVersion()));
            package_request.setTransferTimeout(300000);
            auto* package_reply = manager->get(package_request);
            auto download_cancelled = std::make_shared<bool>(false);
            connect(progress, &QProgressDialog::canceled, this, [package_reply, download_cancelled] {
                *download_cancelled = true;
                package_reply->abort();
            });
            connect(package_reply, &QNetworkReply::downloadProgress, progress,
                [progress, tag](qint64 received, qint64 total) {
                    if (total <= 0) {
                        progress->setRange(0, 0);
                        progress->setLabelText(QStringLiteral("Téléchargement de Rythmo %1… %2 Mo reçus")
                            .arg(tag).arg(received / (1024.0 * 1024.0), 0, 'f', 1));
                        return;
                    }
                    progress->setRange(0, 1000);
                    progress->setValue(static_cast<int>(std::clamp<qint64>(received * 1000 / total, 0, 1000)));
                    progress->setLabelText(QStringLiteral("Téléchargement de Rythmo %1… %2 / %3 Mo")
                        .arg(tag)
                        .arg(received / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(total / (1024.0 * 1024.0), 0, 'f', 1));
                });
            connect(package_reply, &QNetworkReply::readyRead, package_reply, [package_reply, package] {
                if (!package->isOpen()) { package_reply->abort(); return; }
                const QByteArray chunk = package_reply->readAll();
                if (package->write(chunk) != chunk.size()) package_reply->abort();
            });
            progress->show();
            hide();
            connect(package_reply, &QNetworkReply::finished, this,
                [this, package_reply, package, package_path, partial_package_path, update_dir,
                 asset_digest, running_app_path, tag, available, restore_main_window, progress, download_cancelled
                ] {
                    const QByteArray tail = package_reply->readAll();
                    const bool write_ok = package->write(tail) == tail.size() && package->flush();
                    const bool response_ok = package_reply->error() == QNetworkReply::NoError && write_ok;
                    const qint64 package_size = package->size();
                    package->close();
                    bool digest_ok = asset_digest.startsWith(QStringLiteral("sha256:"));
                    if (digest_ok) {
                        QFile verify_file(partial_package_path);
                        if (!verify_file.open(QIODevice::ReadOnly)) digest_ok = false;
                        else {
                            QCryptographicHash hash(QCryptographicHash::Sha256);
                            while (!verify_file.atEnd()) {
                                const QByteArray chunk = verify_file.read(1024 * 1024);
                                if (chunk.isEmpty() && verify_file.error() != QFile::NoError) { digest_ok = false; break; }
                                hash.addData(chunk);
                            }
                            if (digest_ok)
                                digest_ok = QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex()) == asset_digest;
                        }
                    }
                    const bool valid = response_ok && package_size > 1024 && digest_ok;
                    package->deleteLater();
                    package_reply->deleteLater();
                    if (*download_cancelled) {
                        QFile::remove(partial_package_path);
                        restore_main_window();
                        return;
                    }
                    if (!valid || (QFile::exists(package_path) && !QFile::remove(package_path))
                        || !QFile::rename(partial_package_path, package_path)) {
                        QFile::remove(partial_package_path);
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Téléchargement incomplet"),
                            QStringLiteral("Rythmo %1 n’a pas été téléchargé ou vérifié correctement. Réessayez avec une connexion Internet stable.")
                                .arg(tag));
                        return;
                    }

                    progress->setRange(0, 0);
                    progress->setCancelButton(nullptr);
                    progress->setLabelText(QStringLiteral("Préparation de l’installation de Rythmo %1…").arg(tag));
                    const QString script_path = QDir(update_dir).filePath(QStringLiteral("apply-update")
#ifdef Q_OS_WIN
                        + QStringLiteral(".ps1")
#else
                        + QStringLiteral(".sh")
#endif
                    );
                    const qint64 process_id = QCoreApplication::applicationPid();
#ifdef Q_OS_WIN
                    auto quote = [](QString value) { value.replace(QStringLiteral("'"), QStringLiteral("''")); return QStringLiteral("'") + value + QStringLiteral("'"); };
                    const QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Rythmo.exe"));
                    const QString script = QStringLiteral("$ErrorActionPreference='Stop'\n$pidToWait=%1\nwhile (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }\n$root=%2\n$parent=Split-Path -Parent $root\n$transaction=Join-Path $parent ('.Rythmo-update-' + [guid]::NewGuid().ToString('N'))\n$payload=Join-Path $transaction 'payload'\n$backup=Join-Path $transaction 'previous'\nNew-Item -ItemType Directory -Force -Path $payload | Out-Null\ntry {\n  Expand-Archive -LiteralPath %3 -DestinationPath $payload -Force\n  if (-not (Test-Path -LiteralPath (Join-Path $payload 'Rythmo.exe'))) { throw 'Rythmo.exe is missing from the package.' }\n  Move-Item -LiteralPath $root -Destination $backup\n  New-Item -ItemType Directory -Path $root | Out-Null\n  Copy-Item -Path (Join-Path $payload '*') -Destination $root -Recurse -Force\n  Start-Process -FilePath %4\n  Start-Sleep -Seconds 2\n  if (-not (Get-Process -Name 'Rythmo' -ErrorAction SilentlyContinue)) { throw 'Updated Rythmo did not start.' }\n  Remove-Item -LiteralPath $backup -Recurse -Force\n} catch {\n  if (Test-Path -LiteralPath $backup) {\n    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }\n    Move-Item -LiteralPath $backup -Destination $root\n    Start-Process -FilePath %4 -ErrorAction SilentlyContinue\n  }\n  throw\n} finally { Remove-Item -LiteralPath $transaction -Recurse -Force -ErrorAction SilentlyContinue }\n")
                        .arg(process_id).arg(quote(QCoreApplication::applicationDirPath()), quote(package_path), quote(executable));
                    if (!QFile::exists(executable)) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("L’exécutable Rythmo est introuvable dans le dossier de l’application."));
                        return;
                    }
                    QFile script_file(script_path);
                    if (!script_file.open(QIODevice::WriteOnly | QIODevice::Truncate) || script_file.write(script.toUtf8()) != script.toUtf8().size()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le programme d’installation différé n’a pas pu être préparé."));
                        return;
                    }
                    script_file.close();
                    const bool updater_started = QProcess::startDetached(QStringLiteral("powershell.exe"),
                        {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script_path});
#elif defined(Q_OS_MACOS)
                    QString bundle = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../.."));
                    bundle = QDir::cleanPath(bundle);
                    const QFileInfo bundle_info(bundle);
                    if (!bundle_info.isDir() || bundle_info.canonicalFilePath().isEmpty()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"),
                            QStringLiteral("Le paquet Rythmo.app courant est introuvable. Déplacez Rythmo dans Applications, puis réessayez."));
                        return;
                    }
                    bundle = bundle_info.canonicalFilePath();
                    const QString script = QStringLiteral(
                        "#!/bin/sh\n"
                        "set -eu\n"
                        "pid=\"$1\"; package=\"$2\"; old_app=\"$3\"; update_dir=\"$4\"; expected_version=\"$5\"\n"
                        "log=\"$update_dir/apply-update.log\"\n"
                        "/bin/mkdir -p \"$update_dir\"\n"
                        "exec >>\"$log\" 2>&1\n"
                        "backup=\"\"; transaction_dir=\"\"\n"
                        "on_exit() {\n"
                        "  result=$?\n"
                        "  if [ \"$result\" -ne 0 ]; then\n"
                        "    echo \"Update failed with status $result at $(/bin/date)\"\n"
                        "    if [ -n \"$backup\" ] && [ -d \"$backup\" ]; then\n"
                        "      if [ -e \"$old_app\" ]; then /bin/rm -rf \"$old_app\"; fi\n"
                        "      /bin/mv \"$backup\" \"$old_app\" || true\n"
                        "      /usr/bin/open -a \"$old_app\" || true\n"
                        "    elif [ -d \"$old_app\" ]; then\n"
                        "      /usr/bin/open -a \"$old_app\" || true\n"
                        "    fi\n"
                        "    if [ -n \"$transaction_dir\" ]; then echo \"Recovery files retained at $transaction_dir\"; fi\n"
                        "  fi\n"
                        "  exit \"$result\"\n"
                        "}\n"
                        "trap on_exit EXIT\n"
                        "while /bin/kill -0 \"$pid\" 2>/dev/null; do /bin/sleep 0.5; done\n"
                        "parent=$(/usr/bin/dirname \"$old_app\")\n"
                        "transaction_dir=$(/usr/bin/mktemp -d \"$parent/.Rythmo-update.XXXXXX\")\n"
                        "extract_dir=\"$transaction_dir/extracted\"\n"
                        "/bin/mkdir -p \"$extract_dir\"\n"
                        "/usr/bin/ditto -x -k \"$package\" \"$extract_dir\"\n"
                        "new_app=$(/usr/bin/find \"$extract_dir\" -type d -name Rythmo.app -print -quit)\n"
                        "[ -n \"$new_app\" ] && [ -x \"$new_app/Contents/MacOS/Rythmo\" ] || { echo \"Rythmo.app is missing from the update archive\"; exit 1; }\n"
                        "/usr/bin/xattr -dr com.apple.quarantine \"$new_app\" 2>/dev/null || true\n"
                        "/usr/bin/codesign --verify --deep --strict \"$new_app\"\n"
                        "version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \"$new_app/Contents/Info.plist\")\n"
                        "[ \"$version\" = \"$expected_version\" ] || { echo \"Expected Rythmo $expected_version but archive contains $version\"; exit 1; }\n"
                        "backup=\"$transaction_dir/previous.app\"\n"
                        "/bin/mv \"$old_app\" \"$backup\"\n"
                        "/bin/mv \"$new_app\" \"$old_app\"\n"
                        "/usr/bin/open -a \"$old_app\"\n"
                        "new_pid=\"\"\n"
                        "attempt=0\n"
                        "while [ \"$attempt\" -lt 30 ] && [ -z \"$new_pid\" ]; do\n"
                        "  new_pid=$(/usr/bin/pgrep -f \"$old_app/Contents/MacOS/Rythmo\" | /usr/bin/head -n 1 || true)\n"
                        "  [ -n \"$new_pid\" ] || { /bin/sleep 0.5; attempt=$((attempt + 1)); }\n"
                        "done\n"
                        "[ -n \"$new_pid\" ] && /usr/bin/kill -0 \"$new_pid\" 2>/dev/null || { echo \"Updated app did not launch\"; exit 1; }\n"
                        "/bin/rm -rf \"$backup\"\n"
                        "backup=\"\"\n"
                        "/bin/rm -rf \"$transaction_dir\"\n"
                        "transaction_dir=\"\"\n"
                        "trap - EXIT\n"
                    );
                    QFile script_file(script_path);
                    if (!script_file.open(QIODevice::WriteOnly | QIODevice::Truncate) || script_file.write(script.toUtf8()) != script.toUtf8().size()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le programme d’installation différé n’a pas pu être préparé."));
                        return;
                    }
                    script_file.close();
                    QTemporaryFile install_probe(QDir(bundle_info.absolutePath()).filePath(QStringLiteral(".rythmo-update-check-XXXXXX")));
                    install_probe.setAutoRemove(true);
                    const bool target_writable = install_probe.open();
                    if (target_writable) install_probe.close();
                    const bool needs_admin = !target_writable;
                    auto shell_quote = [](QString value) {
                        value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
                        return QStringLiteral("'") + value + QStringLiteral("'");
                    };
                    auto apple_script_quote = [](QString value) {
                        value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
                        value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
                        return QStringLiteral("\"") + value + QStringLiteral("\"");
                    };
                    const QString command = QStringLiteral("/bin/sh %1 %2 %3 %4 %5 %6")
                        .arg(shell_quote(script_path), shell_quote(QString::number(process_id)),
                             shell_quote(package_path), shell_quote(bundle), shell_quote(update_dir),
                             shell_quote(available.toString()));
                    const QString admin_suffix = needs_admin ? QStringLiteral(" with administrator privileges") : QString{};
                    const QString relaunch_old_app = QStringLiteral("/usr/bin/open -a %1").arg(shell_quote(bundle));
                    const QString apple_script = QStringLiteral(
                        "try\n"
                        "  do shell script %1%2\n"
                        "on error errorMessage number errorNumber\n"
                        "  try\n"
                        "    do shell script %3\n"
                        "  end try\n"
                        "  display dialog \"Rythmo n’a pas pu appliquer la mise à jour (\" & errorNumber & \"): \" & errorMessage buttons {\"OK\"} with icon caution\n"
                        "end try")
                        .arg(apple_script_quote(command), admin_suffix, apple_script_quote(relaunch_old_app));
                    progress->setLabelText(needs_admin
                        ? QStringLiteral("macOS va demander l’autorisation nécessaire, puis installer Rythmo %1…").arg(tag)
                        : QStringLiteral("Installation et redémarrage de Rythmo %1…").arg(tag));
                    const bool updater_started = QFileInfo::exists(QStringLiteral("/usr/bin/osascript"))
                        && QProcess::startDetached(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), apple_script});
#else
                    auto shell_quote = [](QString value) { value.replace(QStringLiteral("'"), QStringLiteral("'\\''")); return QStringLiteral("'") + value + QStringLiteral("'"); };
                    const QString script = QStringLiteral("#!/bin/sh\nset -eu\npid=%1\nroot=%2\npackage=%3\nparent=$(dirname \"$root\")\nwhile kill -0 \"$pid\" 2>/dev/null; do sleep 0.5; done\ntransaction=$(mktemp -d \"$parent/.Rythmo-update.XXXXXX\")\ntmp=\"$transaction/extracted\"\nbackup=\"$transaction/previous\"\nmkdir -p \"$tmp\"\ntar -xzf \"$package\" -C \"$tmp\"\nnew_root=$(find \"$tmp\" -mindepth 1 -maxdepth 1 -type d -name 'Rythmo-linux-*' -print -quit)\n[ -n \"$new_root\" ] && [ -x \"$new_root/Rythmo\" ] || exit 1\nrollback() { code=$?; if [ \"$code\" -ne 0 ] && [ -d \"$backup\" ]; then rm -rf \"$root\"; mv \"$backup\" \"$root\"; \"$root/Rythmo\" & fi; exit \"$code\"; }\ntrap rollback EXIT\nmv \"$root\" \"$backup\"\nmv \"$new_root\" \"$root\"\n\"$root/Rythmo\" &\nnew_pid=$!\nsleep 2\nkill -0 \"$new_pid\" >/dev/null 2>&1\nrm -rf \"$backup\" \"$transaction\"\ntrap - EXIT\n")
                        .arg(process_id).arg(shell_quote(QFileInfo(running_app_path).absolutePath()),
                                             shell_quote(package_path), shell_quote(update_dir));
                    QFile script_file(script_path);
                    if (!script_file.open(QIODevice::WriteOnly | QIODevice::Truncate) || script_file.write(script.toUtf8()) != script.toUtf8().size()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le programme d’installation différé n’a pas pu être préparé."));
                        return;
                    }
                    script_file.close();
                    const bool updater_started = QProcess::startDetached(QStringLiteral("/bin/sh"), {script_path});
#endif
                    if (!updater_started) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"),
                            QStringLiteral("Le programme d’installation ne peut pas démarrer. Le journal détaillé est conservé dans :\n%1")
                                .arg(QDir(update_dir).filePath(QStringLiteral("apply-update.log"))));
                        return;
                    }
                    progress->setLabelText(QStringLiteral("Rythmo %1 est prêt. Fermeture puis redémarrage…").arg(tag));
                    QTimer::singleShot(1200, progress, [progress] {
                        progress->close();
                        QCoreApplication::quit();
                    });
                });
            });    });
    auto* space_play = new QShortcut(QKeySequence(Qt::Key_Space), root);
    space_play->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space_play, &QShortcut::activated, this, [this] {
        if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
        else player_->play();
    });

    status_label_ = new QLabel("Ouvrez un fichier DetX pour commencer.", this);
    statusBar()->addWidget(status_label_, 1);
    timeline_->setSelectionHandler([this](int line_index, int track) {
        selected_timeline_track_ = std::clamp(track, 0, 3);
        if (line_index >= 0 && line_index < lines_table_->rowCount()) {
            lines_table_->selectRow(line_index);
            lines_table_->setCurrentCell(line_index, 4);
            lines_table_->scrollToItem(lines_table_->item(line_index, 4));
        } else {
            lines_table_->clearSelection();
            lines_table_->setCurrentCell(-1, -1);
        }
        timeline_->setSelection(line_index, selected_timeline_track_);
        status_label_->setText(QStringLiteral("Piste %1 sélectionnée. Entrée ajoute une réplique à la position rouge.")
                               .arg(selected_timeline_track_ + 1));
    });
    timeline_->setInsertAtHandler([this](int track, qint64 position_ms) {
        addLineAt(track, position_ms);
        timeline_->setFocus(Qt::OtherFocusReason);
    });
    timeline_->setEditHandler([this](int row) { editTimelineLine(row); });
    timeline_->setDeleteHandler([this](int row) {
        if (row >= 0 && row < lines_table_->rowCount()) lines_table_->selectRow(row);
        removeSelectedLine();
    });
    timeline_->setResizeHandler([this](int row, int sign_index, qint64 position_ms) {
        if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
        auto& line = document_->lines()[static_cast<std::size_t>(row)];
        if (sign_index < 0 || sign_index >= static_cast<int>(line.lipsyncs.size())) return;
        Lipsync& boundary = line.lipsyncs[static_cast<std::size_t>(sign_index)];
        const auto fps = document_->fps();
        const auto base = video_start_.to_frames(fps);
        const auto elapsed = static_cast<std::int64_t>(std::llround(
            static_cast<long double>(position_ms) * fps / 1000.0L));
        const auto frame = std::max<std::int64_t>(0, base + elapsed);
        const auto adjusted = Timecode::from_frames(frame, fps);
        if (boundary.timecode == adjusted) return;
        try {
            document_->set_lipsync_timecode(boundary, adjusted);
            setDirty(true);
            const QSignalBlocker blocker(lines_table_);
            if (boundary.kind.rfind("in_", 0) == 0)
                lines_table_->item(row, 0)->setText(qstr(adjusted.format()));
            if (boundary.kind.rfind("out_", 0) == 0)
                lines_table_->item(row, 1)->setText(qstr(adjusted.format()));
            refreshTimeline();
        } catch (const std::exception& error) {
            status_label_->setText("Redimensionnement impossible : " + QString::fromUtf8(error.what()));
        }
    });
    timeline_->setResizeTransactionHandler([this](bool active) {
        if (active) {
            resizing_timeline_edge_ = true;
            timeline_resize_snapshot_ = document_ ? document_->serialize() : std::string{};
            return;
        }
        if (!resizing_timeline_edge_) return;
        resizing_timeline_edge_ = false;
        if (!document_) { timeline_resize_snapshot_.clear(); return; }
        const std::string after = document_->serialize();
        if (!timeline_resize_snapshot_.empty() && after != timeline_resize_snapshot_) {
            undo_history_.push_back(std::move(timeline_resize_snapshot_));
            if (undo_history_.size() > 100) undo_history_.erase(undo_history_.begin());
            redo_history_.clear();
            current_document_snapshot_ = after;
            undo_snapshot_initialized_ = true;
        }
        timeline_resize_snapshot_.clear();
    });
    timeline_->setPlayheadHandler([this](qint64 position_ms) { seekTimeline(position_ms); });
    timeline_->setPlayheadDragHandler([this](bool dragging) {
        if (!player_) return;
        if (dragging) {
            resume_after_timeline_scrub_ = player_->playbackState() == QMediaPlayer::PlayingState;
            if (resume_after_timeline_scrub_) player_->pause();
        } else if (resume_after_timeline_scrub_) {
            resume_after_timeline_scrub_ = false;
            player_->play();
        }
    });
    timeline_->setSplitHandler([this](int) { splitSelectedLine(); });
    connect(add_line_button, &QPushButton::clicked, this, [this] { addLine(); });
    connect(delete_line_button, &QPushButton::clicked, this, [this] { removeSelectedLine(); });
    connect(roles_button, &QPushButton::clicked, this, [this] { manageRoles(); });
    connect(pick_video_button, &QPushButton::clicked, this, [this] { openVideo(); });
    connect(play_button_, &QPushButton::clicked, this, [this] {
        if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause(); else player_->play();
    });
    connect(position_slider_, &QSlider::sliderMoved, this, [this](int value) {
        if (player_->duration() > 0) seekTimeline(qRound64(static_cast<double>(value) * player_->duration() / 1000.0));
    });
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        const auto playback_state = player_->playbackState();
        const qint64 accepted_floor = playback_state == QMediaPlayer::PlayingState
            ? playback_anchor_position_ms_ : current_position_ms_;
        if ((playback_state == QMediaPlayer::PlayingState || playback_state == QMediaPlayer::PausedState) &&
            !playback_seek_pending_ && position < accepted_floor) {
            // Some Windows media backends emit stale position notifications
            // after a newer one. Keep the visual clock monotonic instead of
            // snapping the text and timecode backwards.
            return;
        }
        playback_seek_pending_ = false;
        playback_anchor_position_ms_ = position;
        if (player_->playbackState() == QMediaPlayer::PlayingState) {
            playback_clock_.start();
            // QMediaPlayer reports its clock in coarse, asynchronous steps. At
            // high display refresh rates the interpolated playhead can already
            // be a few milliseconds ahead of the next report. Re-anchor the
            // clock, but never paint that stale report over the newer playhead.
            if (position >= current_position_ms_) updatePosition(position);
        } else {
            updatePosition(position);
        }
    });
    connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64) { refreshTimeline(); });
    connect(player_, &QMediaPlayer::metaDataChanged, this, [this] { updateMediaMetadata(); });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        play_button_->setText(state == QMediaPlayer::PlayingState ? "❚❚ Pause" : "▶ Lire");
        if (state == QMediaPlayer::PlayingState) {
            playback_seek_pending_ = false;
            const qint64 media_position = player_->position();
            playback_anchor_position_ms_ = std::max(current_position_ms_, media_position);
            if (playback_anchor_position_ms_ > media_position + 2) {
                // Resume from the exact visual clock captured on pause so the
                // video and the band do not jump backwards on the first frame.
                playback_seek_pending_ = true;
                player_->setPosition(playback_anchor_position_ms_);
            }
            playback_clock_.start();
            const QScreen* active_screen = timeline_->screen();
            const qreal refresh_rate = active_screen ? active_screen->refreshRate() : 0.0;
            const int interval_ms = std::isfinite(refresh_rate) && refresh_rate > 0.0
                ? qBound(1, qRound(1000.0 / refresh_rate), 100)
                : 16; // QTimer cannot express 16.67 ms; 16 ms is the 60 Hz fallback.
            playback_visual_timer_->setInterval(interval_ms);
            playback_visual_timer_->start();
        } else {
            playback_visual_timer_->stop();
            if (state == QMediaPlayer::PausedState) {
                const qint64 media_position = player_->position();
                const qint64 frozen_position = std::max(current_position_ms_, media_position);
                if (frozen_position > media_position + 2) {
                    playback_seek_pending_ = true;
                    player_->setPosition(frozen_position);
                }
                playback_anchor_position_ms_ = frozen_position;
                updatePosition(frozen_position);
            } else {
                playback_seek_pending_ = false;
                updatePosition(player_->position());
            }
        }
    });
    connect(playback_visual_timer_, &QTimer::timeout, this, [this] {
        const qint64 elapsed_ms = playback_clock_.isValid()
            ? qRound64(playback_clock_.elapsed() * player_->playbackRate()) : 0;
        qint64 position = playback_anchor_position_ms_ + elapsed_ms;
        if (player_->duration() > 0) position = std::min(position, player_->duration());
        position = std::max(position, current_position_ms_);
        updatePosition(position);
    });
    connect(player_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& error) {
                if (!decode_proxy_active_) startFfmpegDecodeFallback(error);
                else status_label_->setText("Erreur de lecture de l’aperçu FFmpeg : " + error);
            });
    connect(lines_table_, &QTableWidget::cellChanged, this, [this](int row, int column) { onCellChanged(row, column); });
    connect(lines_table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
                selected_timeline_track_ = std::clamp(document_->lines()[static_cast<std::size_t>(row)].track, 0, 3);
                timeline_->setSelection(row, selected_timeline_track_);
            });
    connect(lines_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (column == 4) editTimelineLine(row);
        else seekFromTableRow(row);
    });
}

void MainWindow::showHome() {
    active_project_module_.clear();
    if (!home_widget_) {
        auto* page = new QWidget(this);
        page->setObjectName(QStringLiteral("homePage"));
        page->setStyleSheet(QStringLiteral(
            "QWidget#homePage { background:#0B1118; }"
            "QLabel#homeTitle { background:transparent; color:#F1F6FB; font-size:25pt; font-weight:700; }"
            "QLabel#homeSubtitle { background:transparent; color:#AEBCC9; font-size:10.5pt; }"
            "QLabel#homeSection { background:transparent; color:#AEBCC9; font-size:9pt; font-weight:600; }"
            "QPushButton#homeModule { text-align:left; padding:0; min-height:145px; border-radius:12px; background:#141E28; border:1px solid #2A3A49; }"
            "QPushButton#homeModule:hover { background:#192735; border-color:#46B5E3; }"));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(36, 28, 36, 28);
        layout->setSpacing(14);
        auto* top = new QHBoxLayout;
        auto* wordmark = new QLabel(QStringLiteral("RYTHMO"), page);
        wordmark->setStyleSheet(QStringLiteral("background:transparent;color:#69CAF0;font-size:22pt;font-weight:700;letter-spacing:2px;"));
        top->addWidget(wordmark, 0, Qt::AlignLeft | Qt::AlignVCenter);
        top->addStretch(1);
        layout->addLayout(top);
        auto* title = new QLabel(QStringLiteral("Vos outils de doublage"), page);
        title->setObjectName(QStringLiteral("homeTitle"));
        title->setWordWrap(true);
        auto* subtitle = new QLabel(QStringLiteral("Le logiciel de création de bande rythmo et de séparation vocale par excellence."), page);
        subtitle->setObjectName(QStringLiteral("homeSubtitle"));
        subtitle->setWordWrap(true);
        layout->addWidget(title);
        layout->addWidget(subtitle);
        layout->addSpacing(16);
        auto* section = new QLabel(QStringLiteral("ESPACES DE TRAVAIL"), page);
        section->setObjectName(QStringLiteral("homeSection"));
        layout->addWidget(section);
        auto* cards = new QGridLayout;
        cards->setHorizontalSpacing(14);
        cards->setVerticalSpacing(14);
        const auto addCard = [this, cards, page](int column, const QString& title_text,
                                                    const QString& description_text, const QString& action_text,
                                                    const QString& module, const QString& accent) {
            auto* button = new QPushButton(page);
            button->setObjectName(QStringLiteral("homeModule"));
            button->setMinimumWidth(220);
            button->setMinimumHeight(145);
            button->setStyleSheet(QStringLiteral("QPushButton#homeModule { border-top:3px solid %1; }").arg(accent));
            auto* card_layout = new QVBoxLayout(button);
            card_layout->setContentsMargins(20, 18, 20, 18);
            card_layout->setSpacing(10);
            auto* title_label = new QLabel(title_text, button);
            title_label->setStyleSheet(QStringLiteral("background:transparent;color:#F0F6FB;font-size:14pt;font-weight:700;"));
            title_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            auto* description_label = new QLabel(description_text, button);
            description_label->setWordWrap(true);
            description_label->setStyleSheet(QStringLiteral("background:transparent;color:#AFBECC;font-size:11pt;font-weight:500;"));
            description_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            auto* action_label = new QLabel(action_text, button);
            action_label->setStyleSheet(QStringLiteral("background:transparent;color:#67C9EF;font-size:10pt;font-weight:700;"));
            action_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            card_layout->addWidget(title_label);
            card_layout->addWidget(description_label, 1);
            card_layout->addWidget(action_label);
            cards->addWidget(button, 0, column);
            connect(button, &QPushButton::clicked, this, [this, module] { showProjectPage(module); });
        };
        addCard(0, QStringLiteral("BANDE RYTHMO"), QStringLiteral("Créez et synchronisez vos répliques à l’image."), QStringLiteral("Ouvrir l’espace  →"), QStringLiteral("Bande Rythmo"), QStringLiteral("#4AB7E5"));
        addCard(1, QStringLiteral("SÉPARATION DE VOIX"), QStringLiteral("Isolez les dialogues d’une scène en local."), QStringLiteral("Choisir un modèle  →"), QStringLiteral("Séparation vocale"), QStringLiteral("#71C8B8"));
        layout->addLayout(cards);
        auto* recent_heading = new QLabel(QStringLiteral("PROJETS RÉCENTS"), page);
        recent_heading->setStyleSheet(QStringLiteral("background:transparent;color:#8FA4B5;font-size:9pt;font-weight:700;letter-spacing:1px;"));
        layout->addSpacing(10);
        layout->addWidget(recent_heading);
        home_recent_panel_ = new QWidget(page);
        home_recent_panel_->setObjectName(QStringLiteral("homeRecentPanel"));
        auto* recent_row = new QHBoxLayout(home_recent_panel_);
        recent_row->setContentsMargins(0, 0, 0, 0);
        recent_row->setSpacing(12);
        layout->addWidget(home_recent_panel_);
        layout->addStretch(1);
        home_widget_ = page;
        page_stack_->addWidget(home_widget_);
    }
    page_stack_->setCurrentWidget(home_widget_);
    if (home_recent_panel_) {
        auto* row = qobject_cast<QHBoxLayout*>(home_recent_panel_->layout());
        while (row && row->count() > 0) {
            QLayoutItem* item = row->takeAt(0);
            if (QWidget* widget = item->widget()) widget->deleteLater();
            delete item;
        }
        QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
        const QStringList paths = settings.value(QStringLiteral("recentProjects")).toStringList();
        int shown = 0;
        for (const auto& path : paths) {
            const QFileInfo info(path);
            if (!info.exists() || !info.isFile()) continue;
            auto* recent = new QPushButton(QStringLiteral("%1\n%2").arg(info.fileName(), info.absolutePath()), home_recent_panel_);
            recent->setObjectName(QStringLiteral("homeRecentProject"));
            recent->setToolTip(info.absoluteFilePath());
            recent->setMinimumHeight(64);
            recent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            recent->setStyleSheet(QStringLiteral(
                "QPushButton#homeRecentProject { text-align:left; padding:10px 14px; color:#E8F0F6; "
                "background:#111A23; border:1px solid #293B4B; border-radius:10px; font-weight:600; }"
                "QPushButton#homeRecentProject:hover { background:#182633; border-color:#46B5E3; }"));
            row->addWidget(recent, 1);
            connect(recent, &QPushButton::clicked, this, [this, path = info.absoluteFilePath()] {
                openRecentProject(path);
            });
            if (++shown == 3) break;
        }
        if (shown == 0) {
            auto* empty = new QLabel(QStringLiteral("Vos projets DetX apparaîtront ici après leur première ouverture."), home_recent_panel_);
            empty->setStyleSheet(QStringLiteral("background:#111A23;color:#AEBCC9;border:1px solid #293B4B;border-radius:10px;padding:16px;"));
            row->addWidget(empty);
        }
    }
    statusBar()->hide();
}

void MainWindow::showProjectPage(const QString& module) {
    if (!confirmDiscardChanges()) return;
    statusBar()->hide();
    active_project_module_ = module;
    if (module == QStringLiteral("Séparation vocale")) {
        openVoiceSeparation();
        return;
    }
    if (!projects_widget_) {
        projects_widget_ = new QWidget(page_stack_);
        projects_widget_->setObjectName(QStringLiteral("projectsPage"));
        projects_widget_->setStyleSheet(QStringLiteral(
            "QWidget#projectsPage { background:#0B1118; }"
            "QListWidget { background:#121C26; border:1px solid #2A3A49; border-radius:14px; padding:8px; font-size:10pt; }"
            "QListWidget::item { padding:12px; border-bottom:1px solid #263544; }"
            "QListWidget::item:selected { background:#1A3C51; border-radius:8px; }"));
        auto* layout = new QVBoxLayout(projects_widget_);
        layout->setContentsMargins(64, 42, 64, 36);
        layout->setSpacing(16);
        auto* brand = new QLabel(QStringLiteral("RYTHMO  /  DUBIA PRODUCTIONS"), projects_widget_);
        brand->setStyleSheet(QStringLiteral("background:transparent;color:#69CAF0;font-size:10pt;font-weight:700;letter-spacing:1px;"));
        projects_title_ = new QLabel(projects_widget_);
        projects_title_->setStyleSheet(QStringLiteral("background:transparent;color:#F1F6FB;font-size:26pt;font-weight:700;"));
        auto* subtitle = new QLabel(QStringLiteral("Reprenez un projet récent ou démarrez une nouvelle session."), projects_widget_);
        subtitle->setStyleSheet(QStringLiteral("background:transparent;color:#AEBCC9;font-size:11pt;"));
        auto* recent_heading = new QLabel(QStringLiteral("PROJETS RÉCENTS"), projects_widget_);
        recent_heading->setStyleSheet(QStringLiteral("background:transparent;color:#69CAF0;font-size:9pt;font-weight:700;letter-spacing:1px;"));
        recent_projects_list_ = new QListWidget(projects_widget_);
        recent_projects_list_->setMinimumHeight(240);
        recent_projects_empty_ = new QLabel(QStringLiteral("Aucun projet récent pour le moment."), projects_widget_);
        recent_projects_empty_->setStyleSheet(QStringLiteral("background:transparent;color:#AEBCC9;font-size:10pt;padding:8px;"));
        connect(recent_projects_list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
            if (item) openRecentProject(item->data(Qt::UserRole).toString());
        });
        auto* actions = new QHBoxLayout;
        auto* back = new QPushButton(QStringLiteral("Retour à l’accueil"), projects_widget_);
        auto* open_recent = new QPushButton(QStringLiteral("Ouvrir le projet sélectionné"), projects_widget_);
        auto* open = new QPushButton(QStringLiteral("Ouvrir un projet…"), projects_widget_);
        project_continue_button_ = new QPushButton(projects_widget_);
        for (auto* button : {back, open_recent, open, project_continue_button_}) button->setMinimumHeight(44);
        actions->addWidget(back);
        actions->addStretch(1);
        actions->addWidget(open_recent);
        actions->addWidget(open);
        actions->addWidget(project_continue_button_);
        connect(back, &QPushButton::clicked, this, [this] { showHome(); });
        connect(open_recent, &QPushButton::clicked, this, [this] {
            const auto* item = recent_projects_list_->currentItem();
            if (item) openRecentProject(item->data(Qt::UserRole).toString());
            else openDocument();
        });
        connect(open, &QPushButton::clicked, this, [this] { openDocument(); });
        connect(project_continue_button_, &QPushButton::clicked, this, [this] {
            if (active_project_module_ == QStringLiteral("Bande Rythmo")) {
                if (!confirmDiscardChanges()) return;
                player_->stop();
                document_ = std::make_unique<DetxDocument>(DetxDocument::create());
                resetUndoHistory(false);
                document_path_.clear(); video_path_.clear(); video_start_ = {}; media_fps_ = 0.0; timecode_fps_ = 25;
                refreshTable(); refreshTimeline(); setDirty(true); showEditor(); openVideo();
            } else {
                openStudioModule(active_project_module_);
            }
        });
        layout->addWidget(brand);
        layout->addSpacing(8);
        layout->addWidget(projects_title_);
        layout->addWidget(subtitle);
        layout->addSpacing(14);
        layout->addWidget(recent_heading);
        layout->addWidget(recent_projects_list_, 1);
        layout->addWidget(recent_projects_empty_);
        layout->addLayout(actions);
        page_stack_->addWidget(projects_widget_);
    }
    projects_title_->setText(module);
    project_continue_button_->setText(module == QStringLiteral("Bande Rythmo")
        ? QStringLiteral("Créer une bande rythmo") : QStringLiteral("Ouvrir cet espace"));
    refreshRecentProjects();
    page_stack_->setCurrentWidget(projects_widget_);
    status_label_->setText(QStringLiteral("%1 · projets récents").arg(module));
}

void MainWindow::refreshRecentProjects() {
    if (!recent_projects_list_) return;
    QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    const QStringList paths = settings.value(QStringLiteral("recentProjects")).toStringList();
    QStringList existing;
    recent_projects_list_->clear();
    for (const auto& path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) continue;
        const QString absolute = info.absoluteFilePath();
        existing.push_back(absolute);
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(info.fileName(), info.absolutePath()));
        item->setData(Qt::UserRole, absolute);
        item->setToolTip(absolute);
        recent_projects_list_->addItem(item);
    }
    if (existing != paths) {
        settings.setValue(QStringLiteral("recentProjects"), existing);
        settings.sync();
    }
    recent_projects_empty_->setVisible(recent_projects_list_->count() == 0);
}

void MainWindow::rememberRecentProject(const QString& path) {
    if (path.isEmpty()) return;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    QStringList paths = settings.value(QStringLiteral("recentProjects")).toStringList();
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&absolute](const QString& item) {
        return QString::compare(QDir::cleanPath(item), QDir::cleanPath(absolute), Qt::CaseInsensitive) == 0;
    }), paths.end());
    paths.prepend(absolute);
    while (paths.size() > 12) paths.removeLast();
    settings.setValue(QStringLiteral("recentProjects"), paths);
    settings.sync();
    refreshRecentProjects();
}

void MainWindow::openRecentProject(const QString& path) {
    if (path.isEmpty() || !confirmDiscardChanges()) return;
    loadDocument(path);
}

void MainWindow::chooseTimelineFont() {
    QDialog dialog(this);
    QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    dialog.setWindowTitle(QStringLiteral("Police d’écriture de la bande rythmo"));
    dialog.setMinimumWidth(480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(QStringLiteral("Choisissez une police installée ou importez un fichier de police."), &dialog);
    auto* font_combo = new QFontComboBox(&dialog);
    font_combo->setCurrentFont(QFont(settings.value(QStringLiteral("timelineFontFamily"), QStringLiteral("Montserrat")).toString()));
    auto* preview = new QLabel(QStringLiteral("Aperçu: la voix prend sa place."), &dialog);
    preview->setMinimumHeight(64);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("background:#121C26;border:1px solid #2A3A49;border-radius:10px;color:#EEF3F8;padding:10px;"));
    auto* import = new QPushButton(QStringLiteral("Importer une police…"), &dialog);
    layout->addWidget(description);
    layout->addWidget(font_combo);
    layout->addWidget(preview);
    layout->addWidget(import, 0, Qt::AlignLeft);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Appliquer"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annuler"));
    layout->addWidget(buttons);
    QString imported_path = imported_timeline_font_path_;
    const auto update_preview = [font_combo, preview] {
        QFont font = font_combo->currentFont();
        font.setBold(true);
        font.setPointSize(17);
        preview->setFont(font);
    };
    connect(font_combo, &QFontComboBox::currentFontChanged, &dialog, [update_preview](const QFont&) {
        update_preview();
    });
    update_preview();
    connect(import, &QPushButton::clicked, &dialog, [this, &dialog, font_combo, &imported_path] {
        const QString source = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Importer un fichier de police"), {},
            QStringLiteral("Polices (*.ttf *.otf *.ttc *.otc);;Tous les fichiers (*.*)"));
        if (source.isEmpty()) return;
        const QString font_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                                 QStringLiteral("/fonts");
        if (!QDir().mkpath(font_dir)) {
            QMessageBox::warning(&dialog, QStringLiteral("Import impossible"),
                                 QStringLiteral("Impossible de créer le dossier des polices de l’application."));
            return;
        }
        const QString target = QDir(font_dir).filePath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral("-") + QFileInfo(source).fileName());
        if (!QFile::copy(source, target)) {
            QMessageBox::warning(&dialog, QStringLiteral("Import impossible"),
                                 QStringLiteral("Le fichier de police n’a pas pu être copié dans les données de l’application."));
            return;
        }
        const int font_id = QFontDatabase::addApplicationFont(target);
        const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
        if (font_id < 0 || families.isEmpty()) {
            QFile::remove(target);
            QMessageBox::warning(&dialog, QStringLiteral("Police invalide"),
                                 QStringLiteral("Ce fichier ne contient pas de police que Qt peut utiliser."));
            return;
        }
        imported_path = target;
        font_combo->setCurrentFont(QFont(families.front()));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QFont chosen = font_combo->currentFont();
    chosen.setBold(true);
    timeline_->setTextFont(chosen);
    imported_timeline_font_path_ = imported_path;
    settings.setValue(QStringLiteral("timelineFontFamily"), chosen.family());
    settings.setValue(QStringLiteral("timelineFontFile"), imported_timeline_font_path_);
    settings.sync();
}

void MainWindow::showEditor() {
    statusBar()->hide();
    if (page_stack_ && editor_widget_) page_stack_->setCurrentWidget(editor_widget_);
}

void MainWindow::openStudioModule(const QString& module) {
    if (module == QStringLiteral("Séparation vocale")) {
        openVoiceSeparation();
        return;
    }
    if (module == QStringLiteral("Bande Rythmo")) {
        if (document_) { showEditor(); return; }
        openDocument();
        if (document_) showEditor();
        else {
            const auto answer = QMessageBox::question(this, QStringLiteral("Nouvelle bande rythmo"),
                QStringLiteral("Aucun document ouvert. Créer une nouvelle bande rythmo ?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                document_ = std::make_unique<DetxDocument>(DetxDocument::create());
                resetUndoHistory(false);
                document_path_.clear(); video_path_.clear(); video_start_ = {}; media_fps_ = 0.0; timecode_fps_ = 25;
                refreshTable(); refreshTimeline(); setDirty(true); showEditor(); openVideo();
            }
        }
        return;
    }
    auto* page = new QDialog(this);
    page->setWindowTitle(module);
    page->setMinimumSize(520, 260);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(module, page);
    title->setStyleSheet(QStringLiteral("font-size:23pt;font-weight:700;color:#69CAF0;padding:12px;"));
    auto* description = new QLabel(QStringLiteral("Cet espace Rythmo n’est pas disponible."), page);
    description->setWordWrap(true);
    description->setStyleSheet(QStringLiteral("font-size:11pt;color:#AEBCC9;padding:12px;"));
    auto* close = new QPushButton(QStringLiteral("Retour à Rythmo"), page);
    layout->addWidget(title); layout->addWidget(description); layout->addStretch(1); layout->addWidget(close);
    connect(close, &QPushButton::clicked, page, &QDialog::accept);
    page->exec();
    page->deleteLater();
}

void MainWindow::openVoiceSeparation() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Séparation de voix | Rythmo"));
    dialog.setMinimumSize(760, 560);
    dialog.resize(860, 620);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#0B1118; color:#EEF3F8; } QLabel { color:#EEF3F8; }"
        "QPushButton { background:#172330; color:#EEF3F8; border:1px solid #304252; border-radius:10px; padding:9px 14px; }"
        "QPushButton:hover { background:#213243; border-color:#4AB7E5; }"
        "QComboBox, QLineEdit { background:#121C26; color:#EEF3F8; border:1px solid #304252; border-radius:8px; padding:8px; }"
        "QProgressBar { background:#121C26; border:1px solid #304252; border-radius:7px; text-align:center; color:#EEF3F8; min-height:18px; }"
        "QProgressBar::chunk { background:#43B5E4; border-radius:6px; }"));

    const QString app_data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString runtime_root = QDir(app_data_dir).filePath(QStringLiteral("cinematic_separation_runtime"));
    const QString model_dir = QDir(runtime_root).filePath(QStringLiteral("checkpoints"));
    QDir().mkpath(model_dir);
    QSettings model_settings;
    QString custom_model_id = model_settings.value(QStringLiteral("voice/customModelId")).toString();
    QString custom_model_directory = model_settings.value(QStringLiteral("voice/customModelDirectory")).toString();
    QString custom_checkpoint = model_settings.value(QStringLiteral("voice/customCheckpoint")).toString();
    QString custom_config = model_settings.value(QStringLiteral("voice/customConfig")).toString();

    struct ModelSpec { QString repo; QString checkpoint; QString index_file; qint64 minimum_bytes; int sample_rate; QString summary; QString download_url; QString sha256; QString config_url; QString config_sha256; };
    const auto model_spec = [&](const QString& id) {
        if (!custom_model_id.isEmpty() && id == custom_model_id)
            return ModelSpec{QStringLiteral("custom"), custom_checkpoint, custom_config, 1024, 44100,
                QStringLiteral("Modèle RoFormer personnel"), {}, {}, {}, {}};
        if (id == QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"))
            return ModelSpec{QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"), QStringLiteral("inst_v1e_plus.ckpt"), QStringLiteral("config_melbandroformer_inst.yaml"), 913090472, 44100,
                QStringLiteral("Profil élevé · séparation voix et musique · 44,1 kHz · environ 913 Mo."),
                QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-Inst/resolve/f86cd9e99d63eb9499b00fca424bc4ed8a8aeaba/inst_v1e_plus.ckpt"),
                QStringLiteral("6a4ddba739f0352407fb6e18b29206b82318ec427fe37fcedb0f83241e4e15fb"),
                QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-Inst/resolve/b5eb552e1fd019392ad5f912aebe2e8bdd481421/config_melbandroformer_inst.yaml"), {}};
        if (id == QStringLiteral("BS-RoFormer-Viperx-1297"))
            return ModelSpec{QStringLiteral("BS-RoFormer-Viperx-1297"), QStringLiteral("model_bs_roformer_ep_317_sdr_12.9755.ckpt"), QStringLiteral("model_bs_roformer_ep_317_sdr_12.9755.yaml"), 639000000, 44100,
                QStringLiteral("Profil intermédiaire · séparation voix et musique · 44,1 kHz · environ 639 Mo."),
                QStringLiteral("https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/model_bs_roformer_ep_317_sdr_12.9755.ckpt"),
                QStringLiteral("5b84f37e8d444c8cb30c79d77f613a41c05868ff9c9ac6c7049c00aefae115aa"),
                QStringLiteral("https://raw.githubusercontent.com/TRvlvr/application_data/main/mdx_model_data/mdx_c_configs/model_bs_roformer_ep_317_sdr_12.9755.yaml"),
                QStringLiteral("2bfdd16c656bd9519aba757cc4f8834b7ede675eb1e00ec4772d74ae1c41af7f")};
        return ModelSpec{QStringLiteral("Mel-RoFormer-Small-Unwa"), QStringLiteral("melband_roformer_small_v1.ckpt"), QStringLiteral("config_melbandroformer_small.yaml"), 200000000, 44100,
            QStringLiteral("Profil léger · conçu pour les machines modestes · 44,1 kHz · environ 203 Mo."),
            QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-small/resolve/main/melband_roformer_small_v1.ckpt"),
            QStringLiteral("9a1b9f3c32946b66781b82d5de856703d0fdf3359f047830c1373c0310dcb522"),
            QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-small/resolve/1964f60d59b58ca9c3760d757f74e7b75aff2e77/config_melbandroformer_small.yaml"), {}};
    };
    const auto model_directory_path = [&, model_dir, model_spec](const QString& id) {
        if (!custom_model_id.isEmpty() && id == custom_model_id)
            return QDir::cleanPath(QFileInfo(custom_model_directory).absoluteFilePath());
        return QDir::cleanPath(QDir(model_dir).absoluteFilePath(model_spec(id).repo));
    };
    const auto model_path = [&, model_directory_path, model_spec](const QString& id) {
        const auto spec = model_spec(id);
        return QDir(model_directory_path(id)).filePath(spec.checkpoint);
    };
    const auto model_index_path = [&, model_directory_path, model_spec](const QString& id) {
        const auto spec = model_spec(id);
        return QDir(model_directory_path(id)).filePath(spec.index_file);
    };
    const auto model_directory_exists = [model_directory_path](const QString& id) {
        const QFileInfo info(model_directory_path(id));
        return info.exists() && info.isDir();
    };
    const auto model_is_installed = [model_path, model_index_path, model_spec, &custom_model_id](const QString& id) {
        const auto spec = model_spec(id);
        const QFileInfo weight(model_path(id));
        const QFileInfo index(model_index_path(id));
        if (!weight.isFile() || weight.size() < spec.minimum_bytes || !index.isFile()) return false;
        QFile config(index.filePath());
        if (!config.open(QIODevice::ReadOnly)) return false;
        const QByteArray config_data = config.readAll();
        const QString config_text = QString::fromUtf8(config_data);
        if (!spec.config_sha256.isEmpty())
            return config_text.contains(QStringLiteral("sample_rate: 44100"))
                && QString::fromLatin1(QCryptographicHash::hash(config_data, QCryptographicHash::Sha256).toHex()) == spec.config_sha256;
        if (!custom_model_id.isEmpty() && id == custom_model_id)
            return config_text.contains(QStringLiteral("sample_rate: 44100"))
                && config_text.contains(QStringLiteral("model:"));
        if (id == QStringLiteral("Mel-RoFormer-Small-Unwa")) {
            // This checkpoint was trained with a 384-wide model and a 1x mask
            // estimator. Accepting a stale/default 4x config builds 1536-wide
            // layers and makes PyTorch reject the checkpoint tensors.
            const auto has_yaml_value = [&config_text](const QString& key, const QString& value) {
                const QString pattern = QStringLiteral("(?m)^\\s*%1\\s*:\\s*%2\\s*(?:#.*)?$")
                    .arg(QRegularExpression::escape(key), QRegularExpression::escape(value));
                return QRegularExpression(pattern).match(config_text).hasMatch();
            };
            return has_yaml_value(QStringLiteral("sample_rate"), QStringLiteral("44100"))
                && has_yaml_value(QStringLiteral("target_instrument"), QStringLiteral("vocals"))
                && has_yaml_value(QStringLiteral("dim"), QStringLiteral("384"))
                && has_yaml_value(QStringLiteral("depth"), QStringLiteral("6"))
                && has_yaml_value(QStringLiteral("num_bands"), QStringLiteral("60"))
                && has_yaml_value(QStringLiteral("mlp_expansion_factor"), QStringLiteral("1"));
        }
        if (id != QStringLiteral("BS-RoFormer-Viperx-1297"))
            return config_text.contains(QStringLiteral("sample_rate: 44100"))
                && config_text.contains(QStringLiteral("target_instrument:"));
        const auto json = QJsonDocument::fromJson(config_data);
        return json.isObject() && json.object().value(QStringLiteral("sample_rate")).toInt() == spec.sample_rate;
    };

    auto* root = new QVBoxLayout(&dialog);
    auto* stack = new QStackedWidget(&dialog);
    root->addWidget(stack, 1);

    auto* setup = new QWidget(stack);
    auto* setup_layout = new QVBoxLayout(setup);
    setup_layout->setContentsMargins(28, 24, 28, 20);
    auto* heading = new QLabel(QStringLiteral("Séparation vocale"), setup);
    heading->setStyleSheet(QStringLiteral("font-size:22pt;font-weight:700;"));
    auto* intro = new QLabel(QStringLiteral("Choisissez un niveau de séparation. Les modèles fonctionnent localement."), setup);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:#AEBCC9;font-size:11pt;"));
    auto* profile = new QComboBox(setup);
    profile->addItem(QStringLiteral("Faible"), QStringLiteral("Mel-RoFormer-Small-Unwa"));
    profile->addItem(QStringLiteral("Moyen"), QStringLiteral("BS-RoFormer-Viperx-1297"));
    profile->addItem(QStringLiteral("Élevé"), QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"));
    if (!custom_model_id.isEmpty() && QFileInfo(custom_model_directory).isDir())
        profile->addItem(QStringLiteral("Modèle personnel"), custom_model_id);
    profile->addItem(QStringLiteral("Importer un modèle personnel…"), QStringLiteral("__import__"));
    const QString initial_profile = model_settings.value(QStringLiteral("voice/selectedProfile"),
        QStringLiteral("Mel-RoFormer-Small-Unwa")).toString();
    profile->setCurrentIndex(std::max(0, profile->findData(initial_profile)));
    auto* delete_model_button = new QPushButton(QStringLiteral("Supprimer le modèle installé"), setup);
    delete_model_button->setEnabled(false);
    auto* model_info = new QLabel(setup);
    model_info->setWordWrap(true);
    model_info->setStyleSheet(QStringLiteral("color:#AEBCC9;font-size:10pt;"));
    const auto update_model_info = [model_info, model_path, model_spec, model_is_installed, model_directory_exists, profile, delete_model_button, custom_model_id] {
        const QString id = profile->currentData().toString();
        if (id == QStringLiteral("__import__")) {
            model_info->setText(QStringLiteral("Importez un checkpoint RoFormer et sa configuration YAML."));
            delete_model_button->setEnabled(false);
            return;
        }
        const QFileInfo info(model_path(id));
        if (model_is_installed(id))
            model_info->setText(QStringLiteral("Installé · %1 Mo").arg(info.size() / (1024.0 * 1024.0), 0, 'f', 0));
        else if (model_directory_exists(id))
            model_info->setText(QStringLiteral("Installation incomplète · supprimez-la, puis réessayez."));
        else
            model_info->setText(QStringLiteral("Téléchargement requis · environ %1 Mo.")
                                .arg(model_spec(id).minimum_bytes / (1024.0 * 1024.0), 0, 'f', 0));
        delete_model_button->setEnabled(model_directory_exists(id) && id != custom_model_id);
    };
    update_model_info();
    auto* progress_text = new QLabel(setup);
    auto* progress = new QProgressBar(setup);
    progress->setRange(0, 1000);
    progress->hide();
    progress_text->hide();
    auto* actions = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Retour"), setup);
    auto* continue_button = new QPushButton(setup);
    continue_button->setDefault(true);
    const auto update_continue_text = [continue_button, model_is_installed, profile] {
        const QString id = profile->currentData().toString();
        continue_button->setText(id == QStringLiteral("__import__")
            ? QStringLiteral("Importer un modèle")
            : model_is_installed(id) ? QStringLiteral("Continuer") : QStringLiteral("Télécharger le modèle"));
    };
    connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [update_model_info, update_continue_text, &model_settings, profile](int) {
            update_model_info();
            update_continue_text();
            const QString id = profile->currentData().toString();
            if (!id.isEmpty() && id != QStringLiteral("__import__"))
                model_settings.setValue(QStringLiteral("voice/selectedProfile"), id);
        });
    update_continue_text();
    actions->addWidget(back);
    actions->addStretch(1);
    actions->addWidget(continue_button);
    setup_layout->addWidget(heading);
    setup_layout->addWidget(intro);
    setup_layout->addWidget(profile);
    setup_layout->addWidget(model_info);
    setup_layout->addWidget(delete_model_button, 0, Qt::AlignLeft);
    setup_layout->addWidget(progress_text);
    setup_layout->addWidget(progress);
    setup_layout->addStretch(1);
    setup_layout->addLayout(actions);
    stack->addWidget(setup);

    auto* separation = new QWidget(stack);
    auto* separation_layout = new QVBoxLayout(separation);
    separation_layout->setContentsMargins(28, 24, 28, 20);
    auto* separation_title = new QLabel(QStringLiteral("Séparation vocale"), separation);
    separation_title->setStyleSheet(QStringLiteral("font-size:22pt;font-weight:700;"));
    auto* separation_description = new QLabel(QStringLiteral("Ajoutez des fichiers audio ou vidéo. Rythmo crée une piste voix et une piste musique."), separation);
    separation_description->setWordWrap(true);
    auto* drop_zone = new QLabel(QStringLiteral("Déposez les fichiers ici ou ajoutez-les avec le bouton ci-dessous."), separation);
    drop_zone->setObjectName(QStringLiteral("voiceDropZone"));
    drop_zone->setAlignment(Qt::AlignCenter);
    drop_zone->setAcceptDrops(true);
    drop_zone->setMinimumHeight(105);
    drop_zone->setStyleSheet(QStringLiteral("background:#121C26;border:2px dashed #3C829F;border-radius:14px;color:#E3EAF1;padding:14px;font-size:11pt;font-weight:600;"));
    drop_zone->installEventFilter(this);
    auto* sources = new QListWidget(separation);
    sources->setMinimumHeight(120);
    sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sources->setStyleSheet(QStringLiteral("QListWidget { background:#121C26;border:1px solid #2A3A49;border-radius:10px;padding:4px; } QListWidget::item { padding:7px; } QListWidget::item:selected { background:#1A3C51; }"));
    drop_zone->setProperty("sourceListWidget", QVariant::fromValue(static_cast<QObject*>(sources)));
    auto* source_row = new QHBoxLayout;
    auto* choose_source = new QPushButton(QStringLiteral("Ajouter des fichiers…"), separation);
    auto* remove_sources = new QPushButton(QStringLiteral("Retirer la sélection"), separation);
    source_row->addWidget(choose_source);
    source_row->addWidget(remove_sources);
    source_row->addStretch(1);
    auto* output_dir = new QLineEdit(separation);
    output_dir->setPlaceholderText(QStringLiteral("Dossier de destination des fichiers séparés"));
    output_dir->setText(QDir(app_data_dir).filePath(QStringLiteral("output")));
    auto* output_row = new QHBoxLayout;
    auto* choose_output = new QPushButton(QStringLiteral("Choisir le dossier…"), separation);
    output_row->addWidget(output_dir, 1);
    output_row->addWidget(choose_output);
    auto* format_row = new QHBoxLayout;
    auto* format_label = new QLabel(QStringLiteral("Format de sortie"), separation);
    auto* output_format = new QComboBox(separation);
    output_format->addItem(QStringLiteral("WAV (sans perte)"), QStringLiteral("wav"));
    output_format->addItem(QStringLiteral("FLAC (compression sans perte)"), QStringLiteral("flac"));
    output_format->addItem(QStringLiteral("MP3"), QStringLiteral("mp3"));
    output_format->addItem(QStringLiteral("OGG Vorbis"), QStringLiteral("ogg"));
    output_format->addItem(QStringLiteral("M4A / AAC"), QStringLiteral("m4a"));
    format_row->addWidget(format_label);
    format_row->addWidget(output_format);
    format_row->addStretch(1);
    auto* runtime_info = new QLabel(QStringLiteral("Le traitement reste local. Le moteur et le modèle sont téléchargés lors de leur première utilisation."), separation);
    runtime_info->setWordWrap(true);
    runtime_info->setStyleSheet(QStringLiteral("background:#121C26;border:1px solid #2A3A49;border-radius:10px;padding:12px;color:#AEBCC9;"));
    auto* work_progress = new QProgressBar(separation);
    work_progress->setRange(0, 0);
    work_progress->hide();
    auto* work_actions = new QHBoxLayout;
    auto* change_profile = new QPushButton(QStringLiteral("Profil du modèle"), separation);
    auto* separate_button = new QPushButton(QStringLiteral("Séparer"), separation);
    auto* close = new QPushButton(QStringLiteral("Revenir \u00e0 l\u2019accueil"), separation);
    work_actions->addWidget(change_profile);
    work_actions->addStretch(1);
    work_actions->addWidget(separate_button);
    work_actions->addWidget(close);
    separation_layout->addWidget(separation_title);
    separation_layout->addWidget(separation_description);
    separation_layout->addSpacing(12);
    separation_layout->addWidget(drop_zone);
    separation_layout->addWidget(sources, 1);
    separation_layout->addLayout(source_row);
    separation_layout->addWidget(new QLabel(QStringLiteral("Dossier de sortie"), separation));
    separation_layout->addLayout(output_row);
    separation_layout->addLayout(format_row);
    separation_layout->addWidget(runtime_info);
    separation_layout->addWidget(work_progress);
    separation_layout->addStretch(1);
    separation_layout->addLayout(work_actions);
    stack->addWidget(separation);

    QProcess runtime_process(&dialog);
    QProcess media_process(&dialog);
    QTemporaryDir job_temp;
    QString job_model_id;
    QString job_output_dir;
    QString job_format;
    QString job_ffmpeg;
    QStringList job_sources;
    QStringList job_inputs;
    QStringList job_wav_outputs;
    QStringList job_final_outputs;
    QJsonArray job_manifest;
    QString job_process_error;
    QString media_process_error;
    int runtime_stage = 0;
    int media_stage = 0;
    int extract_index = 0;
    int encode_index = 0;
    bool job_cancelled = false;
    bool torch_mirror_retry_attempted = false;
    bool roformer_deps_mirror_retry_attempted = false;
    bool roformer_mirror_retry_attempted = false;
    bool roformer_install_attempted = false;
    constexpr int StageDownloadPython = 1;
    constexpr int StageExtractPython = 2;
    constexpr int StageDownloadPip = 3;
    constexpr int StageInstallPip = 4;
    constexpr int StageInstallTorch = 5;
    constexpr int StageInstallRoformerDeps = 12;
    constexpr int StageCheckRuntime = 7;
    constexpr int StageCheckPip = 8;
    constexpr int StageRunWorker = 9;
    constexpr int StageInstallRoformer = 13;
    constexpr int StageCheckRoformer = 14;
    constexpr int StageVerifyPythonArch = 16;
    constexpr int MediaExtract = 1;
    constexpr int MediaEncode = 2;
    bool arm64_runtime = false;
#if defined(Q_OS_WIN) && (defined(_M_ARM64) || defined(__aarch64__))
    arm64_runtime = true;
#elif defined(Q_OS_MACOS)
    // A universal app can be launched through Rosetta. Detect the host chip so
    // its inference environment is always built with an Apple Silicon Python.
    QProcess arm_probe;
    arm_probe.start(QStringLiteral("/usr/sbin/sysctl"), {QStringLiteral("-n"), QStringLiteral("hw.optional.arm64")});
    if (arm_probe.waitForFinished(1000) && arm_probe.exitCode() == 0)
        arm64_runtime = arm_probe.readAllStandardOutput().trimmed() == QByteArrayLiteral("1");
#elif defined(Q_OS_LINUX)
    const QString host_arch = QSysInfo::currentCpuArchitecture().toLower();
    arm64_runtime = host_arch == QStringLiteral("arm64") || host_arch == QStringLiteral("aarch64");
#endif
    const QString python_dir = QDir(runtime_root).filePath(arm64_runtime
        ? QStringLiteral("python-arm64") : QStringLiteral("python-x64"));
#ifdef Q_OS_WIN
    const QString python_exe = QDir(python_dir).filePath(QStringLiteral("python.exe"));
    const QString python_archive = QDir(runtime_root).filePath(arm64_runtime
        ? QStringLiteral("python-3.12.10-embed-arm64.zip") : QStringLiteral("python-3.12.10-embed-amd64.zip"));
#else
    const QString python_install_dir = QDir(python_dir).filePath(QStringLiteral("python"));
    const QString python_exe = QDir(python_install_dir).filePath(QStringLiteral("bin/python3"));
    const QString python_archive = QDir(runtime_root).filePath(arm64_runtime
        ? QStringLiteral("python-runtime-arm64.tar.gz") : QStringLiteral("python-runtime-x64.tar.gz"));
#endif
#ifdef Q_OS_WIN
    const QString python_runtime_bin = python_dir;
#else
    const QString python_runtime_bin = QDir(python_install_dir).filePath(QStringLiteral("bin"));
#endif
    const QString python_arch_check = arm64_runtime
        ? QStringLiteral("import platform,sys; m=platform.machine().lower(); sys.exit(0 if m in ('arm64','aarch64') else 86)")
        : QStringLiteral("import platform,sys; m=platform.machine().lower(); sys.exit(0 if m in ('amd64','x86_64') else 86)");
    const QString get_pip_script = QDir(runtime_root).filePath(QStringLiteral("get-pip.py"));
    const QString worker_script = QDir(runtime_root).filePath(QStringLiteral("cinematic_separation_worker.py"));
    const QString tar_path = QStandardPaths::findExecutable(nativeToolName(QStringLiteral("tar")));
    std::function<void()> start_next_extract;
    std::function<void()> start_next_encode;
    std::function<void()> start_worker;
    std::function<void(QString)> fail_job;
    const auto set_job_status = [runtime_info, work_progress, separate_button](const QString& status) {
        runtime_info->setText(status);
        work_progress->setRange(0, 0);
        work_progress->show();
        separate_button->setEnabled(false);
        separate_button->setText(QStringLiteral("Traitement en cours…"));
    };
    const auto finish_job_ui = [work_progress, separate_button] {
        work_progress->hide();
        separate_button->setEnabled(true);
        separate_button->setText(QStringLiteral("Séparer Voix + M&E"));
    };
    fail_job = [&dialog, runtime_info, finish_job_ui, &job_cancelled](QString error) {
        finish_job_ui();
        runtime_info->setText(QStringLiteral("Le traitement a échoué : %1").arg(error));
        if (!job_cancelled && dialog.isVisible())
            QMessageBox::warning(&dialog, QStringLiteral("Séparation impossible"), error);
    };
    const auto start_runtime_process = [&runtime_process, runtime_info, work_progress, separate_button, runtime_root, python_dir, python_runtime_bin, &job_ffmpeg,
                                        &runtime_stage, &job_process_error](int stage, const QString& program, const QStringList& arguments, const QString& status) {
        runtime_stage = stage;
        job_process_error.clear();
        runtime_info->setText(status);
        work_progress->setRange(0, 0);
        work_progress->show();
        separate_button->setEnabled(false);
        separate_button->setText(QStringLiteral("Préparation…"));
        QDir().mkpath(runtime_root);
        QDir().mkpath(QDir(runtime_root).filePath(QStringLiteral("tmp")));
        QDir().mkpath(QDir(runtime_root).filePath(QStringLiteral("pip-cache")));
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const QString temp_path = QDir(runtime_root).filePath(QStringLiteral("tmp"));
        environment.insert(QStringLiteral("TEMP"), temp_path);
        environment.insert(QStringLiteral("TMP"), temp_path);
        environment.insert(QStringLiteral("TMPDIR"), temp_path);
        environment.insert(QStringLiteral("PIP_CACHE_DIR"), QDir(runtime_root).filePath(QStringLiteral("pip-cache")));
        environment.insert(QStringLiteral("HF_HOME"), QDir(runtime_root).filePath(QStringLiteral("huggingface-cache")));
        environment.insert(QStringLiteral("HF_HUB_DISABLE_XET"), QStringLiteral("1"));
        environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
        environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        const QString old_path = environment.value(QStringLiteral("PATH"));
        const QString ffmpeg_path = QFileInfo(job_ffmpeg).absoluteFilePath();
        const QString ffmpeg_dir = QFileInfo(ffmpeg_path).absolutePath();
        const auto path_separator = QDir::listSeparator();
        environment.insert(QStringLiteral("RYTHMO_FFMPEG"), ffmpeg_path);
        environment.insert(QStringLiteral("PATH"), ffmpeg_dir + path_separator + python_runtime_bin
                           + (old_path.isEmpty() ? QString() : path_separator + old_path));
        runtime_process.setProcessEnvironment(environment);
        runtime_process.setWorkingDirectory(runtime_root);
        runtime_process.setProgram(program);
        runtime_process.setArguments(arguments);
        runtime_process.start();
    };
    const auto prepare_python_path = [python_dir, &fail_job]() {
#ifdef Q_OS_WIN
        const QString pth_path = QDir(python_dir).filePath(QStringLiteral("python312._pth"));
        QFile pth(pth_path);
        if (!pth.open(QIODevice::ReadOnly | QIODevice::Text)) {
            fail_job(QStringLiteral("Le fichier de configuration du runtime Python est absent ou illisible."));
            return false;
        }
        QString content = QString::fromUtf8(pth.readAll());
        pth.close();
        if (!content.contains(QStringLiteral("Lib\\site-packages")))
            content.replace(QStringLiteral("python312.zip"), QStringLiteral("python312.zip\nLib\\site-packages"));
        content.replace(QStringLiteral("#import site"), QStringLiteral("import site"));
        if (!content.contains(QStringLiteral("import site"))) content += QStringLiteral("\nimport site\n");
        if (!pth.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            fail_job(QStringLiteral("La configuration du runtime Python ne peut pas être écrite."));
            return false;
        }
        pth.write(content.toUtf8());
        pth.close();
        QDir().mkpath(QDir(python_dir).filePath(QStringLiteral("Lib/site-packages")));
        return true;
#else
        Q_UNUSED(python_dir);
        Q_UNUSED(fail_job);
        return true; // The bundled standalone runtime contains pip; no system venv is needed.
#endif
    };
    const auto start_torch_install = [&](bool use_mirror = false) {
        bool nvidia_cuda_available = false;
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        QProcess gpu_probe;
        gpu_probe.start(QStringLiteral("nvidia-smi"),
                        {QStringLiteral("--query-gpu=name"), QStringLiteral("--format=csv,noheader")});
        nvidia_cuda_available = gpu_probe.waitForFinished(1200) && gpu_probe.exitCode() == 0;
#endif
        if (arm64_runtime) {
#ifdef Q_OS_MACOS
            const QString torch_index = use_mirror
                ? QStringLiteral("https://mirrors.cloud.tencent.com/pypi/simple")
                : QStringLiteral("https://pypi.org/simple");
            start_runtime_process(StageInstallTorch, python_exe,
                {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
                 QStringLiteral("--no-warn-script-location"), QStringLiteral("--only-binary=:all:"), QStringLiteral("--retries"), QStringLiteral("2"),
                 QStringLiteral("--index-url"), torch_index,
                 QStringLiteral("torch>=2.13,<3")},
                use_mirror
                    ? QStringLiteral("Nouvel essai de PyTorch Apple Silicon via l’index officiel…")
                    : QStringLiteral("Installation de PyTorch natif Apple Silicon…"));
#elif defined(Q_OS_LINUX)
            Q_UNUSED(use_mirror);
            if (nvidia_cuda_available) {
                // Current ARM64 Linux PyTorch releases publish CUDA-enabled wheels
                // on PyPI; do not force the CPU-only index on a detected NVIDIA host.
                start_runtime_process(StageInstallTorch, python_exe,
                    {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
                     QStringLiteral("--no-warn-script-location"), QStringLiteral("torch"),
                     QStringLiteral("torchaudio"), QStringLiteral("torchvision")},
                    QStringLiteral("Installation de PyTorch ARM64 avec accélération NVIDIA native…"));
            } else {
                start_runtime_process(StageInstallTorch, python_exe,
                    {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
                     QStringLiteral("--no-warn-script-location"), QStringLiteral("torch"),
                     QStringLiteral("torchaudio"), QStringLiteral("torchvision"),
                     QStringLiteral("--index-url"), QStringLiteral("https://download.pytorch.org/whl/cpu")},
                    QStringLiteral("Installation des roues PyTorch CPU ARM64 natives…"));
            }
#else
            Q_UNUSED(use_mirror);
            // The Windows ARM64 CPU index currently publishes the torch wheel;
            // auxiliary torchvision/torchaudio wheels are not required by our
            // worker and are not consistently available for this target.
            start_runtime_process(StageInstallTorch, python_exe,
                {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
                 QStringLiteral("--no-warn-script-location"), QStringLiteral("torch"),
                 QStringLiteral("--index-url"), QStringLiteral("https://download.pytorch.org/whl/cpu")},
                QStringLiteral("Installation des roues PyTorch ARM64 natives…"));
#endif
            return;
        }
#ifdef Q_OS_MACOS
        Q_UNUSED(use_mirror);
        start_runtime_process(StageInstallTorch, python_exe,
            {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
             QStringLiteral("--no-warn-script-location"), QStringLiteral("torch")},
            QStringLiteral("Installation de PyTorch pour macOS (CPU ou Metal selon le matériel)…"));
#else
        Q_UNUSED(use_mirror);
        const QString suffix = nvidia_cuda_available ? QStringLiteral("+cu118") : QStringLiteral("+cpu");
        const QString torch_index = nvidia_cuda_available ? QStringLiteral("https://download.pytorch.org/whl/cu118") : QStringLiteral("https://download.pytorch.org/whl/cpu");
        start_runtime_process(StageInstallTorch, python_exe,
            {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
             QStringLiteral("--no-warn-script-location"), QStringLiteral("torch==2.4.1") + suffix,
             QStringLiteral("torchaudio==2.4.1") + suffix, QStringLiteral("torchvision==0.19.1") + suffix,
             QStringLiteral("--index-url"), torch_index},
            nvidia_cuda_available ? QStringLiteral("Installation de PyTorch avec accélération NVIDIA (plusieurs Go)…")
                         : QStringLiteral("Installation de PyTorch CPU…"));
#endif
    };
    const auto start_roformer_deps_install = [&](bool use_mirror = false) {
        QStringList arguments{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
            QStringLiteral("--disable-pip-version-check"), QStringLiteral("--no-warn-script-location"),
            QStringLiteral("einops>=0.7"), QStringLiteral("huggingface_hub>=0.23,<1.0"),
            QStringLiteral("safetensors>=0.4"), QStringLiteral("rotary-embedding-torch>=0.6"), QStringLiteral("soundfile>=0.12")};
#ifdef Q_OS_MACOS
        if (arm64_runtime) {
            // audio-separator declares diffq for its Demucs backend, but this
            // profile uses MDXC/BS-RoFormer. Install its actual runtime closure
            // explicitly and keep the Mac install wheel-only (no local compiler).
            arguments << QStringLiteral("beartype>=0.18.5,<0.19") << QStringLiteral("julius>=0.2")
                      << QStringLiteral("librosa>=0.10") << QStringLiteral("soxr==1.1.0") << QStringLiteral("decorator>=5.1,<6") << QStringLiteral("joblib>=1.2,<2") << QStringLiteral("cloudpickle>=3.0,<4") << QStringLiteral("msgpack>=1.0.7,<2") << QStringLiteral("ml_collections") << QStringLiteral("numpy>=2")
                      << QStringLiteral("onnx-weekly") << QStringLiteral("onnx2torch-py313>=1.6")
                      << (arm64_runtime ? QStringLiteral("onnxruntime") : QStringLiteral("onnxruntime==1.20.1"))
                      << QStringLiteral("packaging") << QStringLiteral("pydub>=0.25")
                      << QStringLiteral("pyyaml") << QStringLiteral("requests>=2") << QStringLiteral("resampy>=0.4")
                      << QStringLiteral("numba==0.68.0") << QStringLiteral("llvmlite==0.50.0")
                      << QStringLiteral("rotary-embedding-torch>=0.6.1,<0.7.0") << QStringLiteral("samplerate==0.2.2")
                      << QStringLiteral("scipy>=1.13,<2.0.0") << QStringLiteral("six>=1.16")
                      << QStringLiteral("tqdm") << QStringLiteral("audioread==3.0.1") << QStringLiteral("coloredlogs==15.0.1");
            arguments << QStringLiteral("--only-binary=:all:");
        }
#endif
        QString dependency_index;
#ifdef Q_OS_MACOS
        if (arm64_runtime)
            dependency_index = use_mirror
                ? QStringLiteral("https://mirrors.cloud.tencent.com/pypi/simple")
                : QStringLiteral("https://pypi.org/simple");
        else
#endif
        if (use_mirror)
            dependency_index = QStringLiteral("https://mirrors.cloud.tencent.com/pypi/simple");
        if (!dependency_index.isEmpty())
            arguments << QStringLiteral("--index-url") << dependency_index;
        start_runtime_process(StageInstallRoformerDeps, python_exe, arguments,
            use_mirror
                ? QStringLiteral("Nouvel essai des dépendances via l’index officiel…")
                : QStringLiteral("Installation des dépendances audio du moteur…"));
    };
    const auto start_roformer_install = [&](bool use_mirror = false) {
        QStringList arguments{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
            QStringLiteral("--disable-pip-version-check"), QStringLiteral("--no-warn-script-location")};
#if defined(Q_OS_MACOS)
        if (arm64_runtime)
            arguments << QStringLiteral("--only-binary=:all:") << QStringLiteral("--no-deps");
#endif
        QString package_index;
#if defined(Q_OS_MACOS)
        if (arm64_runtime)
            package_index = use_mirror
                ? QStringLiteral("https://mirrors.cloud.tencent.com/pypi/simple")
                : QStringLiteral("https://pypi.org/simple");
        else
#endif
        if (use_mirror)
            package_index = QStringLiteral("https://mirrors.cloud.tencent.com/pypi/simple");
        if (!package_index.isEmpty())
            arguments << QStringLiteral("--index-url") << package_index;
#if defined(Q_OS_MACOS)
        if (arm64_runtime) {
            // Repair the explicit RoFormer runtime set. `diffq` belongs to the unused
            // Demucs backend and has no compatible wheel for this Python 3.12 runtime.
            // Its absence does not affect the MDXC/BS-RoFormer inference path.
            arguments << QStringLiteral("beartype>=0.18.5,<0.19") << QStringLiteral("einops>=0.7")
                      << QStringLiteral("huggingface_hub>=0.23,<1.0") << QStringLiteral("julius>=0.2")
                      << QStringLiteral("librosa>=0.10") << QStringLiteral("soxr==1.1.0") << QStringLiteral("decorator>=5.1,<6") << QStringLiteral("joblib>=1.2,<2") << QStringLiteral("cloudpickle>=3.0,<4") << QStringLiteral("msgpack>=1.0.7,<2") << QStringLiteral("lazy_loader>=0.1")
                      << QStringLiteral("ml_collections") << QStringLiteral("numpy>=2")
                      << QStringLiteral("onnx-weekly") << QStringLiteral("onnx2torch-py313>=1.6")
                      << QStringLiteral("onnxruntime") << QStringLiteral("packaging")
                      << QStringLiteral("pydub>=0.25") << QStringLiteral("pyyaml")
                      << QStringLiteral("requests>=2") << QStringLiteral("resampy>=0.4")
                      << QStringLiteral("numba==0.68.0") << QStringLiteral("llvmlite==0.50.0")
                      << QStringLiteral("rotary-embedding-torch>=0.6.1,<0.7.0")
                      << QStringLiteral("samplerate==0.2.2") << QStringLiteral("scipy>=1.13,<2.0.0")
                      << QStringLiteral("six>=1.16") << QStringLiteral("tqdm")
                      << QStringLiteral("audioread==3.0.1") << QStringLiteral("coloredlogs==15.0.1")
                      << QStringLiteral("soundfile>=0.12") << QStringLiteral("audio-separator==0.47.0");
        } else
#endif
        {
            arguments << QStringLiteral("audio-separator==0.47.0")
                      << (arm64_runtime ? QStringLiteral("onnxruntime") : QStringLiteral("onnxruntime==1.20.1"))
                      << QStringLiteral("audioread==3.0.1") << QStringLiteral("coloredlogs==15.0.1");
        }
        start_runtime_process(StageInstallRoformer, python_exe, arguments,
            use_mirror
                ? QStringLiteral("Nouvel essai du moteur local RoFormer via l’index officiel…")
                : QStringLiteral("Installation du moteur local RoFormer…"));
    };

    QProcess downloader(&dialog);
    bool downloading = false;
    QString active_model_file;
    QString active_model_index_file;
    QString active_model_id;
    qint64 active_model_minimum_bytes = 0;
    QString partial_model_file;
    QString partial_model_index_file;
    QString active_download_output;
    const QString curl_path = QStandardPaths::findExecutable(nativeToolName(QStringLiteral("curl")));
    const auto sha256_file = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QString();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) hash.addData(file.read(4 * 1024 * 1024));
        return QString::fromLatin1(hash.result().toHex());
    };
    connect(&dialog, &QDialog::finished, &downloader, [&] {
        if (downloader.state() != QProcess::NotRunning) {
            downloader.terminate();
            if (!downloader.waitForFinished(1500)) downloader.kill();
        }
    });
    connect(back, &QPushButton::clicked, &dialog, [&] {
        if (downloader.state() != QProcess::NotRunning) downloader.kill();
        else dialog.reject();
    });
    connect(&downloader, &QProcess::readyReadStandardError, &dialog, [&downloader, &active_download_output, progress_text] {
        const QString output = QString::fromLocal8Bit(downloader.readAllStandardError()).trimmed();
        if (!output.isEmpty()) {
            active_download_output = output;
            progress_text->setText(QStringLiteral("Téléchargement sécurisé · %1").arg(output.right(100)));
        }
    });
    connect(&downloader, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog, [&, progress, progress_text](int exit_code, QProcess::ExitStatus exit_status) {
        const auto spec = model_spec(active_model_id);
        downloading = false;
        profile->setEnabled(true);
        continue_button->setEnabled(true);
        back->setText(QStringLiteral("Retour"));
        progress->hide();
        const qint64 received_bytes = QFileInfo(partial_model_file).size();
        QFile marker_file(partial_model_index_file);
        const QByteArray marker_data = marker_file.open(QIODevice::ReadOnly) ? marker_file.readAll() : QByteArray();
        const QString marker_text = QString::fromUtf8(marker_data);
        const bool marker_ok = spec.config_url.isEmpty()
            ? QJsonDocument::fromJson(marker_data).object().value(QStringLiteral("sample_rate")).toInt() == spec.sample_rate
            : marker_text.contains(QStringLiteral("sample_rate: 44100"))
                && (spec.config_sha256.isEmpty()
                    ? marker_text.contains(QStringLiteral("target_instrument:"))
                    : QString::fromLatin1(QCryptographicHash::hash(marker_data, QCryptographicHash::Sha256).toHex()) == spec.config_sha256);
        marker_file.close();
        const bool digest_ok = spec.sha256.isEmpty() || sha256_file(partial_model_file) == spec.sha256;
        if (exit_status != QProcess::NormalExit || exit_code != 0 || received_bytes < active_model_minimum_bytes || !marker_ok || !digest_ok) {
            QString error_text = active_download_output;
            const QString remaining_output = QString::fromLocal8Bit(downloader.readAllStandardError()).trimmed();
            if (!remaining_output.isEmpty()) error_text = remaining_output;
            QFile::remove(partial_model_file);
            QFile::remove(partial_model_index_file);
            progress_text->setText(QStringLiteral("Téléchargement incomplet."));
            progress_text->show();
            QMessageBox::warning(&dialog, QStringLiteral("Modèle indisponible"),
                QStringLiteral("%1 n’a pas été téléchargé complètement (%2). Vérifiez la connexion Internet et réessayez.").arg(spec.repo, error_text.isEmpty() ? QStringLiteral("connexion interrompue ou fichier incomplet") : error_text));
            update_model_info();
            update_continue_text();
            return;
        }
        QFile::remove(active_model_file);
        if (!QFile::rename(partial_model_file, active_model_file)) {
            QFile::remove(partial_model_file);
            QMessageBox::warning(&dialog, QStringLiteral("Installation impossible"), QStringLiteral("Le modèle reçu ne peut pas être installé dans les données de l’application."));
            return;
        }
        QFile::remove(active_model_index_file);
        if (!QFile::rename(partial_model_index_file, active_model_index_file)) {
            QFile::remove(active_model_file);
            QFile::remove(partial_model_index_file);
            QMessageBox::warning(&dialog, QStringLiteral("Installation impossible"), QStringLiteral("L’index du modèle ne peut pas être installé."));
            return;
        }
        update_model_info();
        update_continue_text();
        stack->setCurrentWidget(separation);
    });
    connect(delete_model_button, &QPushButton::clicked, &dialog, [&] {
        if (downloading) return;
        const QString id = profile->currentData().toString();
        const auto spec = model_spec(id);
        const QString target_path = model_directory_path(id);
        const QFileInfo target_info(target_path);
        if (!target_info.exists() || !target_info.isDir()) {
            update_model_info();
            return;
        }
        const QString checkpoints_path = QDir::cleanPath(QFileInfo(model_dir).canonicalFilePath());
        const QString canonical_target = QDir::cleanPath(target_info.canonicalFilePath());
        const QString prefix = checkpoints_path + QDir::separator();
        const bool target_is_child = !checkpoints_path.isEmpty() && canonical_target.startsWith(prefix, Qt::CaseInsensitive);
        if (!target_is_child) {
            QMessageBox::warning(&dialog, QStringLiteral("Suppression impossible"),
                QStringLiteral("Le dossier du modèle ne se trouve pas dans le répertoire de modèles attendu."));
            return;
        }
        const auto answer = QMessageBox::question(&dialog, QStringLiteral("Supprimer le modèle"),
            QStringLiteral("Supprimer les fichiers installés du modèle sélectionné ?\n\nLe modèle pourra être téléchargé à nouveau. Le moteur local restera en place."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        QDir model_directory(canonical_target);
        if (!model_directory.removeRecursively()) {
            QMessageBox::warning(&dialog, QStringLiteral("Suppression incomplète"),
                QStringLiteral("Certains fichiers du modèle n’ont pas pu être supprimés. Fermez toute application qui les utilise, puis réessayez."));
        }
        update_model_info();
        update_continue_text();
    });
    const auto import_custom_model = [&] {
        QMessageBox::information(&dialog, QStringLiteral("Fonction avancée"),
            QStringLiteral("L’import de modèle s’adresse aux utilisateurs avancés. Seuls les checkpoints RoFormer accompagnés de leur configuration YAML sont pris en charge. Rythmo ne peut pas valider la provenance ni la sécurité d’un modèle tiers."));
        const QString checkpoint = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Choisir le checkpoint RoFormer"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            QStringLiteral("Checkpoints RoFormer (*.ckpt)"));
        if (checkpoint.isEmpty()) return;
        const QString config = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Choisir la configuration YAML"),
            QFileInfo(checkpoint).absolutePath(), QStringLiteral("Configuration YAML (*.yaml *.yml)"));
        if (config.isEmpty()) return;
        const QFileInfo checkpoint_info(checkpoint), config_info(config);
        QFile config_file(config);
        if (checkpoint_info.size() < 1024 || checkpoint_info.size() > (qint64(12) << 30)
            || checkpoint_info.suffix().compare(QStringLiteral("ckpt"), Qt::CaseInsensitive) != 0
            || config_info.size() < 16 || config_info.size() > 1024 * 1024
            || !config_file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(&dialog, QStringLiteral("Modèle invalide"),
                QStringLiteral("Le checkpoint ou la configuration est vide, trop volumineux ou dans un format non pris en charge."));
            return;
        }
        const QByteArray yaml = config_file.read(1024 * 1024 + 1);
        const QString yaml_text = QString::fromUtf8(yaml);
        if (yaml.size() > 1024 * 1024 || !yaml_text.contains(QStringLiteral("model:"))
            || !yaml_text.contains(QStringLiteral("sample_rate: 44100"))) {
            QMessageBox::warning(&dialog, QStringLiteral("Configuration invalide"),
                QStringLiteral("La configuration doit décrire un modèle RoFormer à 44,1 kHz."));
            return;
        }
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString target_dir = QDir(model_dir).filePath(QStringLiteral("custom/") + token);
        if (!QDir().mkpath(target_dir)
            || !QFile::copy(checkpoint, QDir(target_dir).filePath(checkpoint_info.fileName()))
            || !QFile::copy(config, QDir(target_dir).filePath(config_info.fileName()))) {
            QDir(target_dir).removeRecursively();
            QMessageBox::warning(&dialog, QStringLiteral("Import impossible"),
                QStringLiteral("Rythmo n’a pas pu copier les deux fichiers dans ses données locales."));
            return;
        }
        custom_model_id = QStringLiteral("custom:") + token;
        custom_model_directory = target_dir;
        custom_checkpoint = checkpoint_info.fileName();
        custom_config = config_info.fileName();
        model_settings.setValue(QStringLiteral("voice/customModelId"), custom_model_id);
        model_settings.setValue(QStringLiteral("voice/customModelDirectory"), custom_model_directory);
        model_settings.setValue(QStringLiteral("voice/customCheckpoint"), custom_checkpoint);
        model_settings.setValue(QStringLiteral("voice/customConfig"), custom_config);
        model_settings.setValue(QStringLiteral("voice/selectedProfile"), custom_model_id);
        int custom_index = profile->findData(custom_model_id);
        if (custom_index < 0) {
            custom_index = profile->count() - 1;
            profile->insertItem(custom_index, QStringLiteral("Modèle personnel"), custom_model_id);
        }
        profile->setCurrentIndex(custom_index);
        update_model_info();
        update_continue_text();
    };
    connect(profile, qOverload<int>(&QComboBox::activated), &dialog, [&](int) {
        if (profile->currentData().toString() != QStringLiteral("__import__")) return;
        const int previous_index = std::max(0, profile->findData(
            model_settings.value(QStringLiteral("voice/selectedProfile"), QStringLiteral("Mel-RoFormer-Small-Unwa")).toString()));
        import_custom_model();
        if (profile->currentData().toString() == QStringLiteral("__import__"))
            profile->setCurrentIndex(previous_index);
    });
    connect(continue_button, &QPushButton::clicked, &dialog, [&] {
        if (profile->currentData().toString() == QStringLiteral("__import__")) {
            import_custom_model();
            return;
        }

        const QString model_id = profile->currentData().toString();
        const auto spec = model_spec(model_id);
        active_model_id = model_id;
        active_model_file = model_path(model_id);
        active_model_index_file = model_index_path(model_id);
        active_model_minimum_bytes = spec.minimum_bytes;
        if (model_is_installed(model_id)) {
            stack->setCurrentWidget(separation);
            return;
        }
        if (curl_path.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Téléchargeur indisponible"),
                QStringLiteral("Le client curl de Windows est introuvable. Vérifiez la connexion Internet et la présence de curl.exe (fourni avec Windows 10/11), puis réessayez."));
            return;
        }
        if (downloading) return;
        const QString target_dir = QFileInfo(active_model_file).absolutePath();
        if (!QDir().mkpath(target_dir)) {
            QMessageBox::warning(&dialog, QStringLiteral("Téléchargement impossible"), QStringLiteral("Le dossier de modèles ne peut pas être créé."));
            return;
        }
        partial_model_file = active_model_file + QStringLiteral(".download");
        partial_model_index_file = active_model_index_file + QStringLiteral(".download");
        QFile::remove(partial_model_file);
        QFile::remove(partial_model_index_file);
        QFile probe(partial_model_file);
        if (!probe.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(&dialog, QStringLiteral("Téléchargement impossible"), QStringLiteral("Le modèle ne peut pas être écrit sur le disque."));
            return;
        }
        probe.close();
        downloading = true;
        active_download_output.clear();
        profile->setEnabled(false);
        delete_model_button->setEnabled(false);
        continue_button->setEnabled(false);
        back->setText(QStringLiteral("Annuler le téléchargement"));
        progress->show();
        progress_text->show();
        progress->setRange(0, 0);
        progress_text->setText(QStringLiteral("Connexion HTTPS sécurisée…"));
        downloader.setProgram(curl_path);
        QStringList download_args{QStringLiteral("--location"), QStringLiteral("--proto"), QStringLiteral("=https"),
            QStringLiteral("--proto-redir"), QStringLiteral("=https"), QStringLiteral("--fail"), QStringLiteral("--retry"), QStringLiteral("5"),
            QStringLiteral("--retry-delay"), QStringLiteral("2"), QStringLiteral("--connect-timeout"), QStringLiteral("30"),
            QStringLiteral("--continue-at"), QStringLiteral("-"),
            QStringLiteral("--output"), partial_model_file};
        if (!spec.config_url.isEmpty()) {
            download_args << spec.download_url << QStringLiteral("--output") << partial_model_index_file << spec.config_url;
        } else if (spec.download_url.isEmpty()) {
            const QUrl model_url(QStringLiteral("https://huggingface.co/%1/resolve/main/%2").arg(spec.repo, spec.checkpoint));
            const QUrl marker_url(QStringLiteral("https://huggingface.co/%1/resolve/main/%2").arg(spec.repo, spec.index_file));
            download_args << model_url.toString() << QStringLiteral("--output") << partial_model_index_file << marker_url.toString();
        } else {
            QFile marker(partial_model_index_file);
            if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                downloading = false;
                profile->setEnabled(true);
                continue_button->setEnabled(true);
                fail_job(QStringLiteral("L’index local du modèle ne peut pas être écrit."));
                return;
            }
            marker.write(QJsonDocument(QJsonObject{{QStringLiteral("sample_rate"), spec.sample_rate}}).toJson(QJsonDocument::Compact));
            marker.close();
            download_args << spec.download_url;
        }
        downloader.setArguments(download_args);
        downloader.start();
    });
    const auto add_sources = [sources](const QStringList& paths) {
        for (const auto& path : paths) {
            const QFileInfo info(path);
            if (!info.exists() || !info.isFile()) continue;
            bool duplicate = false;
            for (int i = 0; i < sources->count(); ++i)
                if (QDir::cleanPath(sources->item(i)->data(Qt::UserRole).toString()).compare(QDir::cleanPath(info.absoluteFilePath()), Qt::CaseInsensitive) == 0) duplicate = true;
            if (duplicate) continue;
            auto* item = new QListWidgetItem(info.fileName(), sources);
            item->setToolTip(info.absoluteFilePath());
            item->setData(Qt::UserRole, info.absoluteFilePath());
        }
    };
    connect(choose_source, &QPushButton::clicked, &dialog, [&dialog, add_sources] {
        const QStringList paths = QFileDialog::getOpenFileNames(&dialog, QStringLiteral("Choisir les scènes"), {},
            QStringLiteral("Vidéo et audio (*.mp4 *.mkv *.mov *.avi *.webm *.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus *.wma *.aiff);;Tous les fichiers (*.*)"));
        add_sources(paths);
    });
    connect(remove_sources, &QPushButton::clicked, &dialog, [sources] {
        const auto selected = sources->selectedItems();
        for (auto* item : selected) delete sources->takeItem(sources->row(item));
    });
    connect(choose_output, &QPushButton::clicked, &dialog, [&dialog, output_dir] {
        const QString path = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choisir le dossier de sortie"), output_dir->text());
        if (!path.isEmpty()) output_dir->setText(path);
    });
    connect(change_profile, &QPushButton::clicked, &dialog, [stack] { stack->setCurrentIndex(0); });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    start_next_encode = [&, fail_job, finish_job_ui] {
        if (encode_index >= job_wav_outputs.size()) {
            if (job_final_outputs.isEmpty()) {
                fail_job(QStringLiteral("Aucun fichier audio de sortie n’a été créé."));
                return;
            }
            finish_job_ui();
            runtime_info->setStyleSheet(QStringLiteral("background:#172B36;border:1px solid #397D99;border-radius:10px;padding:12px;color:#D9F3FC;"));
            runtime_info->setText(QStringLiteral("Séparation terminée : %1 fichier(s) dans %2").arg(job_final_outputs.size()).arg(job_output_dir));
            QDesktopServices::openUrl(QUrl::fromLocalFile(job_output_dir));
            return;
        }
        const QString source_wav = job_wav_outputs.at(encode_index);
        const QString final_path = job_final_outputs.at(encode_index);
        QFile::remove(final_path);
        if (job_format == QStringLiteral("wav")) {
            if (!QFile::copy(source_wav, final_path) || QFileInfo(final_path).size() < 1024) {
                fail_job(QStringLiteral("Le résultat WAV %1 ne peut pas être enregistré.").arg(QFileInfo(final_path).fileName()));
                return;
            }
            ++encode_index;
            start_next_encode();
            return;
        }
        QStringList arguments{QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                              QStringLiteral("-i"), source_wav};
        if (job_format == QStringLiteral("flac")) arguments << QStringLiteral("-c:a") << QStringLiteral("flac");
        else if (job_format == QStringLiteral("mp3")) arguments << QStringLiteral("-c:a") << QStringLiteral("libmp3lame") << QStringLiteral("-q:a") << QStringLiteral("2");
        else if (job_format == QStringLiteral("ogg")) arguments << QStringLiteral("-c:a") << QStringLiteral("libvorbis") << QStringLiteral("-q:a") << QStringLiteral("5");
        else arguments << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a") << QStringLiteral("192k");
        arguments << final_path;
        media_stage = MediaEncode;
        media_process_error.clear();
        set_job_status(QStringLiteral("Conversion %1/%2 · %3").arg(encode_index + 1).arg(job_wav_outputs.size()).arg(QFileInfo(final_path).fileName()));
        media_process.setProgram(job_ffmpeg);
        media_process.setArguments(arguments);
        media_process.start();
    };

    start_worker = [&, start_runtime_process, fail_job] {
        const QString manifest_path = QDir(job_temp.path()).filePath(QStringLiteral("manifest.json"));
        QFile manifest(manifest_path);
        if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail_job(QStringLiteral("Le manifeste des fichiers à traiter ne peut pas être écrit."));
            return;
        }
        manifest.write(QJsonDocument(job_manifest).toJson(QJsonDocument::Compact));
        manifest.close();
        QFile worker_resource(QStringLiteral(":/runtime/cinematic_separation_worker.py"));
        if (!worker_resource.open(QIODevice::ReadOnly)) {
            fail_job(QStringLiteral("Le script de séparation cinématique n’est pas présent dans cette version de Rythmo."));
            return;
        }
        QFile worker_target(worker_script);
        if (!worker_target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail_job(QStringLiteral("Le script de séparation ne peut pas être installé dans les données de l’application."));
            return;
        }
        worker_target.write(worker_resource.readAll());
        worker_target.close();
        const QString ffmpeg_bootstrap = QStringLiteral(
            "import os,runpy,sys; p=sys.argv.pop(1); f=os.environ.get('RYTHMO_FFMPEG',''); "
            "os.environ['PATH']=(os.path.dirname(f)+os.pathsep if f else '')+os.environ.get('PATH',''); "
            "sys.argv=[p]+sys.argv[1:]; runpy.run_path(p,run_name='__main__')");
        start_runtime_process(StageRunWorker, python_exe,
            {QStringLiteral("-c"), ffmpeg_bootstrap, worker_script, QStringLiteral("--model"), job_model_id,
             QStringLiteral("--manifest"), manifest_path, QStringLiteral("--model-dir"),
             model_directory_path(job_model_id)},
            QStringLiteral("Le moteur cinématique sépare Voix + M&E sur %1 fichier(s)…").arg(job_sources.size()));
    };

    start_next_extract = [&, start_worker, fail_job] {
        if (extract_index >= job_sources.size()) {
            start_worker();
            return;
        }
        const QString source = job_sources.at(extract_index);
        const QString input_wav = QDir(job_temp.path()).filePath(QStringLiteral("input_%1.wav").arg(extract_index + 1, 3, 10, QLatin1Char('0')));
        const QString base_name = QFileInfo(source).completeBaseName();
        const int sample_rate = model_spec(job_model_id).sample_rate;
        const QString voice_wav = QDir(job_temp.path()).filePath(QStringLiteral("voice_%1.wav").arg(extract_index + 1, 3, 10, QLatin1Char('0')));
        const QString me_wav = QDir(job_temp.path()).filePath(QStringLiteral("me_%1.wav").arg(extract_index + 1, 3, 10, QLatin1Char('0')));
        const QString numbered_base = QStringLiteral("%1_%2").arg(extract_index + 1, 2, 10, QLatin1Char('0')).arg(base_name);
        const QString voice_final = QDir(job_output_dir).filePath(QStringLiteral("%1_Voix.%2").arg(numbered_base, job_format));
        const QString me_final = QDir(job_output_dir).filePath(QStringLiteral("%1_M-E.%2").arg(numbered_base, job_format));
        QDir().mkpath(QFileInfo(input_wav).absolutePath());
        job_inputs.push_back(input_wav);
        job_wav_outputs.push_back(voice_wav);
        job_wav_outputs.push_back(me_wav);
        job_final_outputs.push_back(voice_final);
        job_final_outputs.push_back(me_final);
        QJsonObject manifest_item;
        manifest_item.insert(QStringLiteral("input"), input_wav);
        manifest_item.insert(QStringLiteral("voice_output"), voice_wav);
        manifest_item.insert(QStringLiteral("me_output"), me_wav);
        job_manifest.append(manifest_item);
        media_stage = MediaExtract;
        media_process_error.clear();
        set_job_status(QStringLiteral("Décodage audio %1/%2 · %3").arg(extract_index + 1).arg(job_sources.size()).arg(QFileInfo(source).fileName()));
        media_process.setProgram(job_ffmpeg);
        media_process.setArguments({QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), source, QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"),
            QStringLiteral("-ar"), QString::number(sample_rate), QStringLiteral("-c:a"), QStringLiteral("pcm_f32le"), input_wav});
        media_process.start();
    };

    connect(&runtime_process, &QProcess::readyReadStandardOutput, &dialog, [&runtime_process, runtime_info] {
        const QString output = QString::fromUtf8(runtime_process.readAllStandardOutput()).trimmed();
        if (!output.isEmpty()) runtime_info->setText(output.right(320));
    });
    connect(&runtime_process, &QProcess::readyReadStandardError, &dialog, [&runtime_process, &job_process_error, runtime_info] {
        const QString output = QString::fromUtf8(runtime_process.readAllStandardError()).trimmed();
        if (!output.isEmpty()) {
            // Keep earlier pip warnings too: its final "No matching distribution"
            // often hides the DNS failure that prevented the index from loading.
            job_process_error += output + QLatin1Char('\n');
            constexpr qsizetype max_diagnostic_chars = 16000;
            if (job_process_error.size() > max_diagnostic_chars)
                job_process_error.remove(0, job_process_error.size() - max_diagnostic_chars);
            runtime_info->setText(output.right(320));
        }
    });
    connect(&media_process, &QProcess::readyReadStandardError, &dialog, [&media_process, &media_process_error] {
        const QString output = QString::fromUtf8(media_process.readAllStandardError()).trimmed();
        if (!output.isEmpty()) media_process_error = output.right(2500);
    });
    connect(&media_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
        [&, fail_job](int exit_code, QProcess::ExitStatus exit_status) {
            if (job_cancelled) return;
            if (exit_status != QProcess::NormalExit || exit_code != 0) {
                fail_job(media_process_error.isEmpty() ? QStringLiteral("FFmpeg n’a pas pu lire ou convertir l’un des fichiers audio/vidéo.") : media_process_error);
                return;
            }
            if (media_stage == MediaExtract) {
                if (extract_index >= job_inputs.size() || QFileInfo(job_inputs.at(extract_index)).size() < 1024) {
                    fail_job(QStringLiteral("Aucune piste audio exploitable n’a été trouvée dans %1.").arg(QFileInfo(job_sources.value(extract_index)).fileName()));
                    return;
                }
                ++extract_index;
                start_next_extract();
            } else if (media_stage == MediaEncode) {
                if (encode_index >= job_final_outputs.size() || QFileInfo(job_final_outputs.value(encode_index)).size() < 1024) {
                    fail_job(QStringLiteral("Le format %1 n’a pas pu être créé. Vérifiez que le FFmpeg installé contient l’encodeur correspondant.").arg(job_format.toUpper()));
                    return;
                }
                ++encode_index;
                start_next_encode();
            }
        });
    connect(&runtime_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
        [&, start_runtime_process, prepare_python_path, start_next_extract, start_torch_install,
            start_roformer_deps_install, start_roformer_install, fail_job](int exit_code, QProcess::ExitStatus exit_status) {
            if (job_cancelled) return;
            const int completed_stage = runtime_stage;
            const bool success = exit_status == QProcess::NormalExit && exit_code == 0;
            if (!success && completed_stage == StageCheckRuntime) {
                start_runtime_process(StageCheckPip, python_exe, {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("--version")},
                                      QStringLiteral("Vérification du gestionnaire de dépendances…"));
                return;
            }
            if (!success && completed_stage == StageCheckRoformer) {
                if (roformer_install_attempted) {
                    const QString detail = job_process_error.trimmed();
                    fail_job(detail.isEmpty()
                        ? QStringLiteral("Le moteur local RoFormer ne peut pas être chargé. Vérifiez le runtime et réessayez.")
                        : QStringLiteral("Le moteur local RoFormer ne peut pas être chargé : %1").arg(detail.right(600)));
                    return;
                }
                roformer_install_attempted = true;
                start_roformer_deps_install();
                return;
            }
            if (!success && completed_stage == StageVerifyPythonArch) {
                fail_job(arm64_runtime
                    ? QStringLiteral("Le runtime Python sélectionné n’est pas ARM64 natif. Aucun moteur x64 traduit ne sera utilisé.")
                    : QStringLiteral("Le runtime Python sélectionné ne correspond pas à l’architecture native de Rythmo."));
                return;
            }
            if (!success && completed_stage == StageCheckPip) {
                start_runtime_process(StageDownloadPip, curl_path,
                    {QStringLiteral("--location"), QStringLiteral("--proto"), QStringLiteral("=https"), QStringLiteral("--proto-redir"), QStringLiteral("=https"),
                     QStringLiteral("--fail"), QStringLiteral("--retry"), QStringLiteral("3"),
                     QStringLiteral("--output"), get_pip_script, QStringLiteral("https://bootstrap.pypa.io/get-pip.py")},
                    QStringLiteral("Préparation de pip dans le runtime isolé…"));
                return;
            }
            if (!success) {
                const QString detail = job_process_error.trimmed();
                const bool package_index_dns_failure = detail.contains(QStringLiteral("NameResolutionError"), Qt::CaseInsensitive)
                    || detail.contains(QStringLiteral("Failed to resolve"), Qt::CaseInsensitive)
                    || detail.contains(QStringLiteral("Temporary failure in name resolution"), Qt::CaseInsensitive)
                    || detail.contains(QStringLiteral("nodename nor servname provided"), Qt::CaseInsensitive);
                if (package_index_dns_failure && completed_stage == StageInstallTorch && arm64_runtime
                    && !torch_mirror_retry_attempted) {
                    torch_mirror_retry_attempted = true;
                    start_torch_install(true);
                    return;
                }
                if (package_index_dns_failure && completed_stage == StageInstallRoformerDeps
                    && !roformer_deps_mirror_retry_attempted) {
                    roformer_deps_mirror_retry_attempted = true;
                    start_roformer_deps_install(true);
                    return;
                }
                if (package_index_dns_failure && completed_stage == StageInstallRoformer
                    && !roformer_mirror_retry_attempted) {
                    roformer_mirror_retry_attempted = true;
                    start_roformer_install(true);
                    return;
                }
                if (package_index_dns_failure) {
                    fail_job(QStringLiteral("Le runtime ne peut pas joindre l’index de paquets : le nom du serveur ne se résout pas par DNS. Vérifiez la connexion Internet ou le DNS du Mac, puis réessayez. Aucun modèle n’a été installé.\n\nDétail : %1").arg(detail.right(500)));
                } else {
                    fail_job(detail.isEmpty()
                        ? QStringLiteral("L’installation ou le lancement du moteur de séparation a échoué (étape %1). Vérifiez la connexion Internet et l’espace disque.").arg(completed_stage)
                        : detail);
                }
                return;
            }
            if (completed_stage == StageDownloadPython) {
                if (QFileInfo(python_archive).size() < 8LL * 1024 * 1024 || tar_path.isEmpty()) {
                    fail_job(QStringLiteral("L’archive Python est incomplète ou l’outil d’extraction Windows (tar.exe) est introuvable."));
                    return;
                }
                QDir().mkpath(python_dir);
                start_runtime_process(StageExtractPython, tar_path,
                    {QStringLiteral("-xf"), python_archive, QStringLiteral("-C"), python_dir},
                    QStringLiteral("Extraction du runtime Python isolé…"));
            } else if (completed_stage == StageExtractPython) {
                QFile::remove(python_archive);
                start_runtime_process(StageVerifyPythonArch, python_exe,
                    {QStringLiteral("-c"), python_arch_check},
                    QStringLiteral("Vérification de l’architecture ARM64 native…"));
            } else if (completed_stage == StageVerifyPythonArch) {
                if (!prepare_python_path()) return;
                if (QFileInfo::exists(get_pip_script)) {
                    start_runtime_process(StageInstallPip, python_exe,
                        {get_pip_script, QStringLiteral("--disable-pip-version-check")},
                        QStringLiteral("Initialisation du runtime Python natif…"));
                } else {
                    start_runtime_process(StageCheckPip, python_exe,
                        {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("--version")},
                        QStringLiteral("Vérification des dépendances du runtime natif…"));
                }
            } else if (completed_stage == StageDownloadPip) {
                if (QFileInfo(get_pip_script).size() < 100000) {
                    fail_job(QStringLiteral("Le programme d’installation de pip a été téléchargé de manière incomplète."));
                    return;
                }
                start_runtime_process(StageInstallPip, python_exe,
                    {get_pip_script, QStringLiteral("--disable-pip-version-check")},
                    QStringLiteral("Initialisation du runtime Python…"));
            } else if (completed_stage == StageInstallPip || completed_stage == StageCheckPip) {
                start_torch_install();
            } else if (completed_stage == StageInstallTorch) {
                start_roformer_deps_install();
            } else if (completed_stage == StageInstallRoformerDeps) {
                start_roformer_install();
            } else if (completed_stage == StageInstallRoformer) {
                start_runtime_process(StageCheckRoformer, python_exe,
                    {QStringLiteral("-c"), QStringLiteral("import importlib.metadata as md, inspect, platform, sys; from packaging.version import Version; import torch; v=Version(md.version('audio-separator')); t=Version(torch.__version__.split('+')[0]); assert v >= Version('0.47.0'); assert platform.system() != 'Darwin' or platform.machine().lower() not in ('arm64','aarch64') or t >= Version('2.13'); assert Version(md.version('samplerate')) >= Version('0.2.2'); import audioread, librosa, numba, llvmlite, onnxruntime, resampy, samplerate, soundfile, soxr, decorator, joblib, cloudpickle, msgpack, einops, huggingface_hub, safetensors, beartype, julius, ml_collections, onnx, onnx2torch, pydub, requests, scipy, six, tqdm, yaml, rotary_embedding_torch; inspect.signature(librosa.get_duration); from audio_separator.separator import Separator; print('Moteur RoFormer prêt')")},
                    QStringLiteral("Vérification du moteur RoFormer local…"));
            } else if (completed_stage == StageCheckRoformer) {
                QDir(QDir(runtime_root).filePath(QStringLiteral("pip-cache"))).removeRecursively();
                start_next_extract();
            } else if (completed_stage == StageCheckRuntime) {
                start_runtime_process(StageCheckRoformer, python_exe,
                    {QStringLiteral("-c"), QStringLiteral("import importlib.metadata as md, inspect, platform, sys; from packaging.version import Version; import torch; v=Version(md.version('audio-separator')); t=Version(torch.__version__.split('+')[0]); assert v >= Version('0.47.0'); assert platform.system() != 'Darwin' or platform.machine().lower() not in ('arm64','aarch64') or t >= Version('2.13'); assert Version(md.version('samplerate')) >= Version('0.2.2'); import audioread, librosa, numba, llvmlite, onnxruntime, resampy, samplerate, soundfile, soxr, decorator, joblib, cloudpickle, msgpack, einops, huggingface_hub, safetensors, beartype, julius, ml_collections, onnx, onnx2torch, pydub, requests, scipy, six, tqdm, yaml, rotary_embedding_torch; inspect.signature(librosa.get_duration); from audio_separator.separator import Separator; print('Moteur RoFormer prêt')")},
                    QStringLiteral("Vérification du moteur RoFormer local…"));
            } else if (completed_stage == StageRunWorker) {
                QFile manifest_file(QDir(job_temp.path()).filePath(QStringLiteral("manifest.json")));
                if (!manifest_file.open(QIODevice::ReadOnly)) {
                    fail_job(QStringLiteral("Le manifeste de séparation n’a pas pu être relu après le traitement."));
                    return;
                }
                const QJsonDocument manifest_doc = QJsonDocument::fromJson(manifest_file.readAll());
                manifest_file.close();
                if (!manifest_doc.isArray() || manifest_doc.array().size() != job_sources.size()) {
                    fail_job(QStringLiteral("Le moteur n’a pas confirmé le nombre de canaux des pistes traitées."));
                    return;
                }
                if (job_format == QStringLiteral("mp3")) {
                    for (const auto& item : manifest_doc.array()) {
                        if (item.toObject().value(QStringLiteral("channels")).toInt() > 2) {
                            fail_job(QStringLiteral("MP3 ne peut pas préserver plus de deux canaux. Choisissez WAV, FLAC ou OGG pour garder le multicanal d’origine."));
                            return;
                        }
                    }
                }
                start_next_encode();
            }
        });

    connect(&dialog, &QDialog::finished, &dialog, [&] {
        job_cancelled = true;
        for (QProcess* process : {&downloader, &runtime_process, &media_process})
            if (process->state() != QProcess::NotRunning) process->kill();
    });

    connect(separate_button, &QPushButton::clicked, &dialog, [&] {
        if (sources->count() == 0) {
            QMessageBox::warning(&dialog, QStringLiteral("Fichiers manquants"), QStringLiteral("Déposez ou ajoutez au moins un fichier vidéo ou audio."));
            return;
        }
        if (!model_is_installed(profile->currentData().toString())) {
            QMessageBox::warning(&dialog, QStringLiteral("Modèle manquant"), QStringLiteral("Retournez au choix du profil et installez d’abord les poids du modèle sélectionné."));
            return;
        }
        const QString selected_output_dir = QDir::cleanPath(output_dir->text().trimmed());
        if (selected_output_dir.isEmpty() || !QDir().mkpath(selected_output_dir)) {
            QMessageBox::warning(&dialog, QStringLiteral("Dossier de sortie manquant"), QStringLiteral("Choisissez un dossier de sortie valide."));
            return;
        }
        // Resolve the bundled app binary first, even when separation starts
        // directly from Home. Finder does not inherit an interactive shell PATH.
        job_ffmpeg = findBundledOrSystemTool(QStringLiteral("ffmpeg"));
        if (!job_ffmpeg.isEmpty()) ffmpeg_path_ = job_ffmpeg;
        if (job_ffmpeg.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("FFmpeg requis"), QStringLiteral("FFmpeg est nécessaire pour décoder les vidéos, préparer les pistes et encoder les sorties. Placez ffmpeg.exe avec Rythmo ou ajoutez-le au PATH."));
            return;
        }
        if (!job_temp.isValid()) {
            QMessageBox::warning(&dialog, QStringLiteral("Espace temporaire indisponible"), QStringLiteral("Rythmo ne peut pas créer son dossier de travail temporaire."));
            return;
        }
        job_model_id = profile->currentData().toString();
        job_output_dir = selected_output_dir;
        job_format = output_format->currentData().toString();
        job_sources.clear();
        job_inputs.clear();
        job_wav_outputs.clear();
        job_final_outputs.clear();
        job_manifest = QJsonArray();
        extract_index = 0;
        encode_index = 0;
        job_cancelled = false;
        for (int i = 0; i < sources->count(); ++i)
            job_sources.push_back(sources->item(i)->data(Qt::UserRole).toString());
        runtime_info->setStyleSheet(QStringLiteral("background:#121C26;border:1px solid #2A3A49;border-radius:10px;padding:12px;color:#AEBCC9;"));
        set_job_status(QStringLiteral("Préparation du moteur RoFormer… Le runtime et ses dépendances sont installés localement."));
        if (!QFileInfo::exists(python_exe)) {
#ifdef Q_OS_WIN
            if (curl_path.isEmpty()) {
                fail_job(QStringLiteral("curl est requis pour préparer le runtime Python local."));
                return;
            }
            start_runtime_process(StageDownloadPython, curl_path,
                {QStringLiteral("--location"), QStringLiteral("--proto"), QStringLiteral("=https"), QStringLiteral("--proto-redir"), QStringLiteral("=https"),
                 QStringLiteral("--fail"), QStringLiteral("--retry"), QStringLiteral("3"),
                 QStringLiteral("--retry-delay"), QStringLiteral("2"), QStringLiteral("--connect-timeout"), QStringLiteral("30"),
                 QStringLiteral("--output"), python_archive,
                 QStringLiteral("https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-")
                     + (arm64_runtime ? QStringLiteral("arm64") : QStringLiteral("amd64")) + QStringLiteral(".zip")},
                QStringLiteral("Téléchargement sécurisé du runtime Python 3.12…"));
            return;
#else
            QFile bundled_runtime(QStringLiteral(":/runtime/python-runtime.tar.gz"));
            if (!bundled_runtime.open(QIODevice::ReadOnly)) {
                fail_job(QStringLiteral("Le runtime Python natif n’est pas inclus dans cette version de Rythmo. Réinstallez l’application depuis un build complet."));
                return;
            }
            QDir().mkpath(python_dir);
            const QByteArray archive_contents = bundled_runtime.readAll();
            QSaveFile archive_file(python_archive);
            if (!archive_file.open(QIODevice::WriteOnly)
                || archive_file.write(archive_contents) != archive_contents.size() || !archive_file.commit()) {
                fail_job(QStringLiteral("Le runtime Python embarqué n’a pas pu être préparé dans le dossier de l’application."));
                return;
            }
            if (tar_path.isEmpty()) {
                fail_job(QStringLiteral("L’outil système tar est requis pour extraire le runtime embarqué."));
                return;
            }
            start_runtime_process(StageExtractPython, tar_path,
                {QStringLiteral("-xzf"), python_archive, QStringLiteral("-C"), python_dir},
                QStringLiteral("Préparation du moteur Python natif embarqué…"));
            return;
#endif
        }
        if (!prepare_python_path()) return;
        start_runtime_process(StageCheckRuntime, python_exe,
            {QStringLiteral("-c"), QStringLiteral("import torch, soundfile, audio_separator; print('Runtime RoFormer prêt · PyTorch', torch.__version__, '· CUDA', torch.cuda.is_available())")},
            QStringLiteral("Vérification du moteur RoFormer installé…"));
    });

    if (model_is_installed(profile->currentData().toString()))
        stack->setCurrentWidget(separation);
    hide();
    dialog.exec();
    showHome();
    show();
    raise();
    activateWindow();
}

void MainWindow::createMenus() {
    auto* file_menu = menuBar()->addMenu("Fichier");
    auto* home_action = file_menu->addAction(QStringLiteral("Accueil Rythmo"));
    connect(home_action, &QAction::triggered, this, [this] { showHome(); });
    file_menu->addSeparator();
    auto* new_action = file_menu->addAction("Nouveau document");
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, [this] {
        if (!confirmDiscardChanges()) return;
        player_->stop();
        document_ = std::make_unique<DetxDocument>(DetxDocument::create());
        resetUndoHistory(false);
        document_path_.clear(); video_path_.clear(); video_start_ = {}; media_fps_ = 0.0; timecode_fps_ = 25;
        refreshTable(); refreshTimeline(); setDirty(true);
        status_label_->setText("Nouveau document DetX (25 i/s).");
        openVideo();
    });
    auto* open_action = file_menu->addAction("Ouvrir…");
    open_action->setShortcut(QKeySequence::Open);
    connect(open_action, &QAction::triggered, this, [this] { openDocument(); });
    auto* save_action = file_menu->addAction("Enregistrer");
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, [this] { saveDocument(); });
    auto* save_as_action = file_menu->addAction("Enregistrer sous…");
    save_as_action->setShortcut(QKeySequence::SaveAs);
    connect(save_as_action, &QAction::triggered, this, [this] { saveDocumentAs(); });
    file_menu->addSeparator();
    auto* open_video_action = file_menu->addAction("Ouvrir la vidéo source…");
    connect(open_video_action, &QAction::triggered, this, [this] { openVideo(); });
    auto* export_action = file_menu->addAction("Exporter");
    connect(export_action, &QAction::triggered, this, [this] { exportVideoWithRhythm(); });
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction("Quitter");
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    auto* edit_menu = menuBar()->addMenu("Édition");
    auto* undo_action = edit_menu->addAction(QStringLiteral("Annuler (%1)")
        .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)));
    connect(undo_action, &QAction::triggered, this, [this] { undoLastChange(); });
    auto* redo_action = edit_menu->addAction(QStringLiteral("Rétablir (%1)")
        .arg(QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText)));
    connect(redo_action, &QAction::triggered, this, [this] { redoLastChange(); });
    edit_menu->addSeparator();
    auto* font_action = edit_menu->addAction(QStringLiteral("Police d’écriture…"));
    connect(font_action, &QAction::triggered, this, [this] { chooseTimelineFont(); });
    edit_menu->addSeparator();
    auto* add_line_action = edit_menu->addAction("Ajouter une réplique");
    add_line_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(add_line_action, &QAction::triggered, this, [this] { addLine(); });
    auto* remove_line_action = edit_menu->addAction("Supprimer la réplique sélectionnée");
    remove_line_action->setShortcut(QKeySequence::Delete);
    connect(remove_line_action, &QAction::triggered, this, [this] { removeSelectedLine(); });
    auto* split_action = edit_menu->addAction("Scinder la réplique…");
    split_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(split_action, &QAction::triggered, this, [this] { splitSelectedLine(); });

    auto* view_menu = menuBar()->addMenu("Affichage");
    auto* fullscreen_action = view_menu->addAction("Vidéo + bande rythmo en plein écran");
    fullscreen_action->setShortcut(QKeySequence(Qt::Key_F11));
    connect(fullscreen_action, &QAction::triggered, this, [this] { toggleRhythmFullscreen(); });
}

void MainWindow::openDocument() {
    if (!confirmDiscardChanges()) return;
    QFileDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Ouvrir un document DetX"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters({QStringLiteral("Documents DetX (*.detx *.DetX)"),
                           QStringLiteral("XML (*.xml *.XML)"),
                           QStringLiteral("Tous les fichiers (*)")});
    // Keep the operating-system picker enabled. On macOS Qt then presents the
    // native NSOpenPanel, while the explicit dialog object also gives it a
    // stable parent and a useful starting folder.
    dialog.setOption(QFileDialog::DontUseNativeDialog, false);
    QString start_directory = document_path_.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        : QFileInfo(document_path_).absolutePath();
    if (start_directory.isEmpty() || !QDir(start_directory).exists())
        start_directory = QDir::homePath();
    dialog.setDirectory(start_directory);
    dialog.setWindowModality(Qt::WindowModal);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    loadDocument(dialog.selectedFiles().constFirst());
}

void MainWindow::loadDocument(const QString& path) {
    try {
        auto loaded = DetxDocument::load(fs_path(path));
        player_->stop();
        document_ = std::make_unique<DetxDocument>(std::move(loaded));
        resetUndoHistory(true);
        document_path_ = QFileInfo(path).absoluteFilePath();
        rememberRecentProject(document_path_);
        timecode_fps_ = document_->fps();
        video_start_ = video_start_timecode(*document_).value_or(Timecode{});
        video_path_.clear(); media_fps_ = 0.0;
        refreshTable(); refreshTimeline(); setDirty(false); showEditor();
        status_label_->setText(QString("Document chargé : %1 rôles, %2 répliques, %3 i/s%4")
                               .arg(static_cast<qulonglong>(document_->roles().size()))
                               .arg(static_cast<qulonglong>(document_->lines().size())).arg(timecode_fps_)
                               .arg(document_->fps_is_assumed() ? " (FPS à confirmer)" : ""));
        auto stored_video = video_path_from_document(*document_);
        if (!stored_video.isEmpty()) {
            QFileInfo stored_info(stored_video);
            QString candidate = stored_info.isAbsolute() ? stored_info.absoluteFilePath()
                : QFileInfo(QFileInfo(document_path_).absoluteDir(), stored_video).absoluteFilePath();
            if (QFileInfo::exists(candidate)) {
                // Let Qt paint and expose the project first. Media source opening is
                // asynchronous; unsupported codecs are handed to FFmpeg on error.
                status_label_->setText(QStringLiteral("Projet prêt · Préchargement vidéo : %1")
                    .arg(stored_info.fileName()));
                QTimer::singleShot(0, this, [this, candidate] {
                    if (document_ && QFileInfo::exists(candidate)) loadVideo(candidate, false);
                });
            }
            else status_label_->setText(status_label_->text() + ". Vidéo référencée introuvable, sélectionnez-la manuellement.");
        }
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Ouverture impossible", QString::fromUtf8(error.what()));
    }
}

void MainWindow::saveDocument() {
    if (!document_) return;
    if (document_path_.isEmpty()) { saveDocumentAs(); return; }
    try {
        document_->save(fs_path(document_path_));
        setDirty(false);
        rememberRecentProject(document_path_);
        status_label_->setText("Document enregistré.");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Enregistrement impossible", QString::fromUtf8(error.what()));
    }
}

bool MainWindow::saveDocumentAs() {
    if (!document_) return false;
    const auto path = QFileDialog::getSaveFileName(this, "Enregistrer le document DetX", document_path_, "Documents DetX (*.detx)");
    if (path.isEmpty()) return false;
    document_path_ = path.endsWith(".detx", Qt::CaseInsensitive) ? path : path + ".detx";
    saveDocument();
    return !dirty_;
}

bool MainWindow::confirmDiscardChanges() {
    if (!dirty_) return true;
    const auto result = QMessageBox::warning(this, "Modifications non enregistrées",
        "Le document a été modifié. Enregistrer avant de continuer ?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (result == QMessageBox::Cancel) return false;
    if (result == QMessageBox::Save) { saveDocument(); return !dirty_; }
    return true;
}

void MainWindow::openVideo() {
    const auto path = QFileDialog::getOpenFileName(this, "Ouvrir la vidéo source", {},
        "Vidéos courantes (*.mp4 *.m4v *.mov *.mkv *.avi *.webm *.wmv *.mpg *.mpeg *.m2v *.ts *.mts *.m2ts *.mxf *.flv *.3gp *.ogv *.vob *.ivf *.av1);;Tous les fichiers (*.*)");
    if (!path.isEmpty()) loadVideo(path);
}

void MainWindow::loadVideo(const QString& path, bool persist_reference) {
    if (decode_encoder_probe_process_ && decode_encoder_probe_process_->state() != QProcess::NotRunning) {
        disconnect(decode_encoder_probe_process_, nullptr, this, nullptr);
        decode_encoder_probe_process_->kill();
        decode_encoder_probe_process_->deleteLater();
        decode_encoder_probe_process_ = nullptr;
    }
    if (decode_fallback_process_ && decode_fallback_process_->state() != QProcess::NotRunning) {
        decode_fallback_running_ = false;
        decode_fallback_process_->kill();
    }
    player_->stop();
    player_->setSource(QUrl{});
    if (!decode_proxy_path_.isEmpty()) QFile::remove(decode_proxy_path_);
    decode_proxy_path_.clear();
    decode_fallback_attempted_ = false;
    decode_fallback_running_ = false;
    decode_proxy_active_ = false;
    decode_log_tail_.clear();
    video_path_ = QFileInfo(path).absoluteFilePath();
    if (persist_reference && document_) {
        document_->set_video_reference(utf8(video_path_));
        setDirty(true);
    }
    media_fps_ = 0.0;
    ffmpeg_path_ = findBundledOrSystemTool(QStringLiteral("ffmpeg"));
    refreshTimeline();
    if (ffmpeg_path_.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg introuvable : placez FFmpeg à côté de l’application ou ajoutez-le au PATH."));
        return;
    }
    media_fps_ = 0.0;
    status_label_->setText(QStringLiteral("Préchargement vidéo : %1").arg(QFileInfo(video_path_).fileName()));
    // Native playback starts immediately and lets the backend prepare frames in
    // parallel with editing. FFmpeg is started only if this system cannot decode
    // the source codec/container, avoiding a full transcode for common formats.
    player_->setSource(QUrl::fromLocalFile(video_path_));
}

void MainWindow::startFfmpegDecodeFallback(const QString& qt_error) {
    if (decode_fallback_attempted_) return;
    decode_fallback_attempted_ = true;
    const QString ffmpeg = ffmpeg_path_.isEmpty() ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty() || !decode_temp_dir_.isValid()) {
        status_label_->setText(QStringLiteral("FFmpeg est requis pour le décodage vidéo. Placez FFmpeg à côté de l’application ou ajoutez-le au PATH."));
        return;
    }
    if (decode_encoder_probe_process_ && decode_encoder_probe_process_->state() != QProcess::NotRunning) {
        decode_encoder_probe_process_->kill();
        decode_encoder_probe_process_->waitForFinished(1000);
    }
    auto* probe = new QProcess(this);
    decode_encoder_probe_process_ = probe;
    probe->setProcessChannelMode(QProcess::MergedChannels);
    connect(probe, &QProcess::readyRead, this, [probe] { probe->setProperty("encoderOutput",
        probe->property("encoderOutput").toString() + QString::fromLocal8Bit(probe->readAll())); });
    connect(probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, probe, qt_error](int code, QProcess::ExitStatus state) {
            const QString encoders = probe->property("encoderOutput").toString()
                + QString::fromLocal8Bit(probe->readAll());
            probe->deleteLater();
            if (decode_encoder_probe_process_ == probe) decode_encoder_probe_process_ = nullptr;
            if (code != 0 || state != QProcess::NormalExit) {
                decode_fallback_attempted_ = false;
                status_label_->setText(QStringLiteral("FFmpeg ne répond pas : %1").arg(qt_error));
                return;
            }
            startFfmpegDecodeProxy(encoders);
        });
    connect(probe, &QProcess::errorOccurred, this, [this, probe, qt_error](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        probe->deleteLater();
        if (decode_encoder_probe_process_ == probe) decode_encoder_probe_process_ = nullptr;
        decode_fallback_attempted_ = false;
        status_label_->setText(QStringLiteral("Qt n’a pas pu lire cette vidéo et FFmpeg ne répond pas : %1").arg(qt_error));
    });
    status_label_->setText(QStringLiteral("Préparation du décodage vidéo en arrière-plan : %1")
        .arg(QFileInfo(video_path_).fileName()));
    probe->start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
}

void MainWindow::startFfmpegDecodeProxy(const QString& encoders) {
    if (video_path_.isEmpty() || !decode_temp_dir_.isValid()) return;
    const QString ffmpeg = ffmpeg_path_.isEmpty()
        ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg introuvable pour décoder cette vidéo."));
        return;
    }
    QString video_encoder;
    QString audio_encoder;
#ifdef Q_OS_MACOS
    // Prefer Apple's hardware path for the playback proxy. Software remains a
    // fallback for FFmpeg builds without VideoToolbox support.
    if (encoders.contains(QRegularExpression(QStringLiteral("\\bh264_videotoolbox\\b"))))
        video_encoder = QStringLiteral("h264_videotoolbox");
    else if (encoders.contains(QRegularExpression(QStringLiteral("\\blibx264\\b"))))
        video_encoder = QStringLiteral("libx264");
    if (!video_encoder.isEmpty() && encoders.contains(QRegularExpression(QStringLiteral("\\baac\\b"))))
        audio_encoder = QStringLiteral("aac");
    if (video_encoder.isEmpty() || audio_encoder.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg doit fournir un encodeur H.264 et AAC pour créer l’aperçu vidéo macOS."));
        return;
    }
#else
    video_encoder = encoders.contains(QRegularExpression(QStringLiteral("\\blibx264\\b")))
        ? QStringLiteral("libx264") : QStringLiteral("mpeg4");
    audio_encoder = encoders.contains(QRegularExpression(QStringLiteral("\\baac\\b")))
        ? QStringLiteral("aac") : (encoders.contains(QStringLiteral("libmp3lame"))
            ? QStringLiteral("libmp3lame") : QStringLiteral("pcm_s16le"));
#endif

#ifdef Q_OS_MACOS
    const QString preview_extension = QStringLiteral("mp4");
#else
    const QString preview_extension = QStringLiteral("mkv");
#endif
    decode_proxy_path_ = QDir(decode_temp_dir_.path()).filePath(
        QStringLiteral("preview-%1.%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), preview_extension));
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-hide_banner")};
#ifdef Q_OS_MACOS
    // Ask FFmpeg to use VideoToolbox for supported AV1/H.264/HEVC streams,
    // falling back to software decoding when the codec or OS cannot accelerate it.
    args << QStringLiteral("-hwaccel") << QStringLiteral("auto");
#endif
    args << QStringLiteral("-i") << video_path_ << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-map") << QStringLiteral("0:a:0?") << QStringLiteral("-sn")
         << QStringLiteral("-dn") << QStringLiteral("-c:v") << video_encoder;
    if (video_encoder == "libx264") args << "-preset" << "ultrafast" << "-crf" << "20" << "-pix_fmt" << "yuv420p";
    else if (video_encoder == "h264_videotoolbox")
        args << "-allow_sw" << "0" << "-realtime" << "1" << "-b:v" << "16M" << "-pix_fmt" << "yuv420p";
    else args << "-q:v" << "3";
    args << "-c:a" << audio_encoder;
    if (audio_encoder == "aac" || audio_encoder == "libmp3lame") args << "-b:a" << "192k";
#ifdef Q_OS_MACOS
    args << "-movflags" << "+faststart";
#endif
    args << "-avoid_negative_ts" << "make_zero" << decode_proxy_path_;

    if (!decode_fallback_process_) {
        decode_fallback_process_ = new QProcess(this);
        decode_fallback_process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(decode_fallback_process_, &QProcess::readyRead, this, [this] {
            decode_log_tail_ += QString::fromLocal8Bit(decode_fallback_process_->readAll());
            if (decode_log_tail_.size() > 5000) decode_log_tail_.remove(0, decode_log_tail_.size() - 5000);
        });
        connect(decode_fallback_process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus state) { finishFfmpegDecodeFallback(code, state); });
        connect(decode_fallback_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finishFfmpegDecodeFallback(-1, QProcess::CrashExit);
        });
    }
    decode_log_tail_.clear();
    decode_fallback_running_ = true;
    status_label_->setText(QStringLiteral("Décodage de la vidéo avec FFmpeg (%1)…").arg(QFileInfo(video_path_).fileName()));
    decode_fallback_process_->start(ffmpeg, args);
}

void MainWindow::finishFfmpegDecodeFallback(int exit_code, QProcess::ExitStatus status) {
    if (!decode_fallback_running_) return;
    decode_fallback_running_ = false;
    if (decode_fallback_process_)
        decode_log_tail_ += QString::fromLocal8Bit(decode_fallback_process_->readAll());
    if (exit_code == 0 && status == QProcess::NormalExit && QFileInfo(decode_proxy_path_).size() > 0) {
        decode_proxy_active_ = true;
        player_->setSource(QUrl::fromLocalFile(decode_proxy_path_));
        status_label_->setText(QStringLiteral("Aperçu FFmpeg prêt | source : %1").arg(QFileInfo(video_path_).fileName()));
        return;
    }
    QFile::remove(decode_proxy_path_);
    decode_proxy_path_.clear();
    QMessageBox::warning(this, "Décodage vidéo impossible",
        QStringLiteral("Qt Multimedia n’a pas pu lire cette vidéo et FFmpeg n’a pas réussi à créer l’aperçu. Vérifiez que FFmpeg inclut le décodeur du codec source et qu’il reste assez d’espace temporaire.\n\n%1")
            .arg(decode_log_tail_.right(2500)));
}

void MainWindow::refreshTable() {
    populating_table_ = true;
    const QSignalBlocker blocker(lines_table_);
    lines_table_->setRowCount(document_ ? static_cast<int>(document_->lines().size()) : 0);
    if (document_) {
        const auto& lines = document_->lines();
        for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
            const auto& line = lines[static_cast<std::size_t>(row)];
            auto* in = new QTableWidgetItem(first_timecode(line, true));
            auto* out = new QTableWidgetItem(first_timecode(line, false));
            if (line.lipsyncs.empty()) {
                in->setFlags(in->flags() & ~Qt::ItemIsEditable);
                out->setFlags(out->flags() & ~Qt::ItemIsEditable);
            }
            auto* track = new QTableWidgetItem(QString::number(line.track));
            auto* text = new QTableWidgetItem(qstr(line.text));
            text->setFlags(text->flags() & ~Qt::ItemIsEditable);
            lines_table_->setItem(row, 0, in); lines_table_->setItem(row, 1, out);
            auto* role = new QComboBox(lines_table_);
            role->addItem(QStringLiteral("Sans rôle"), QString{});
            for (const auto& item : document_->roles()) role->addItem(qstr(item.name), qstr(item.id));
            int selected_role = role->findData(qstr(line.role_id));
            if (selected_role < 0 && !line.role_id.empty()) {
                role->addItem(qstr(line.role_id), qstr(line.role_id));
                selected_role = role->count() - 1;
            }
            role->setCurrentIndex(std::max(0, selected_role));
            role->setStyleSheet(QStringLiteral(
                "QComboBox { border-radius:0; padding:4px 6px; min-height:20px; }"
                "QComboBox::drop-down { border:0; border-radius:0; width:18px; }"));
            connect(role, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, row, role](int index) {
                if (populating_table_ || !document_ || row < 0 || row >= static_cast<int>(document_->lines().size()) || index < 0) return;
                auto& changed = document_->lines()[static_cast<std::size_t>(row)];
                changed.role_id = utf8(role->itemData(index).toString());
                changed.element->set_attribute("role", changed.role_id);
                setDirty(true);
                refreshTimeline();
            });
            lines_table_->setCellWidget(row, 2, role);
            lines_table_->setItem(row, 3, track); lines_table_->setItem(row, 4, text);
        }
    }
    populating_table_ = false;
    refreshTimeline();
}

void MainWindow::refreshTimeline() {
    if (!timeline_) return;
    const auto nominal = document_ ? document_->fps() : 25;
    const auto duration = player_ ? player_->duration() : 0;
    timeline_->setDocument(document_.get(), video_start_, nominal,
                            media_fps_ > 0.0 ? media_fps_ : nominal, duration);
}

void MainWindow::onCellChanged(int row, int column) {
    if (populating_table_ || !document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
    try {
        auto& line = document_->lines()[static_cast<std::size_t>(row)];
        auto* item = lines_table_->item(row, column);
        if (!item) return;
        if (column == 4) {
            document_->set_line_text(line, utf8(item->text()));
        } else if (column == 3) {
            bool ok = false; const int value = item->text().toInt(&ok);
            if (!ok || value < 0 || value > 3) throw std::invalid_argument("La piste doit être comprise entre 0 et 3.");
            line.track = value; line.element->set_attribute("track", std::to_string(value));
        } else if (column == 0 || column == 1) {
            const auto parsed = Timecode::parse(utf8(item->text().trimmed()));
            Lipsync* target = nullptr;
            for (auto& sign : line.lipsyncs) {
                const bool is_target = column == 0 ? sign.kind.rfind("in_", 0) == 0 : sign.kind.rfind("out_", 0) == 0;
                if (is_target) { target = &sign; if (column == 0) break; }
            }
            if (!target) throw std::invalid_argument("Aucun signe d’entrée/sortie modifiable dans cette réplique.");
            document_->set_lipsync_timecode(*target, parsed);
        }
        setDirty(true);
        refreshTimeline();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Modification refusée", QString::fromUtf8(error.what()));
        refreshTable();
    }
}

void MainWindow::addLine() {
    addLineAt(selected_timeline_track_, current_position_ms_);
}

void MainWindow::addLineAt(int track, qint64 position_ms) {
    if (!document_) { QMessageBox::information(this, "Aucun document", "Créez ou ouvrez d’abord un document DetX."); return; }
    track = std::clamp(track, 0, 3);
    selected_timeline_track_ = track;
    if (document_->roles().empty()) {
        QMessageBox::information(this, QStringLiteral("Aucun rôle"), QStringLiteral("Ajoutez d’abord un rôle pour attribuer cette réplique."));
        manageRoles();
        if (!document_ || document_->roles().empty()) return;
    }
    try {
        const int fps = document_->fps();
        const auto base = video_start_.to_frames(fps);
        const auto elapsed = static_cast<std::int64_t>(std::llround(static_cast<long double>(std::max<qint64>(0, position_ms)) * fps / 1000.0L));
        const auto in = Timecode::from_frames(std::max<std::int64_t>(0, base + elapsed), fps);
        const auto start = in.to_frames(document_->fps());
        const auto out = Timecode::from_frames(start + 2LL * document_->fps(), document_->fps());
        std::vector<std::string> lane_roles;
        for (const auto& line : document_->lines()) {
            if (line.track == track && !line.role_id.empty() &&
                std::find(lane_roles.begin(), lane_roles.end(), line.role_id) == lane_roles.end()) lane_roles.push_back(line.role_id);
        }
        std::string role;
        if (lane_roles.size() == 1) {
            role = lane_roles.front();
        } else {
            QStringList names;
            for (const auto& item : document_->roles()) {
                const QString name = qstr(item.name);
                const auto duplicates = std::count_if(document_->roles().begin(), document_->roles().end(),
                    [&](const Role& other) { return other.name == item.name; });
                names << (duplicates > 1 ? name + QStringLiteral(" (") + qstr(item.id) + QLatin1Char(')') : name);
            }
            int initial = 0;
            const int selected = lines_table_->currentRow();
            if (selected >= 0 && selected < static_cast<int>(document_->lines().size())) {
                for (int i = 0; i < static_cast<int>(document_->roles().size()); ++i)
                    if (document_->roles()[static_cast<std::size_t>(i)].id == document_->lines()[static_cast<std::size_t>(selected)].role_id) initial = i;
            }
            bool accepted = false;
            const auto chosen = QInputDialog::getItem(this, QStringLiteral("Nouvelle réplique"),
                QStringLiteral("Personnage qui parle :"), names, initial, false, &accepted);
            if (!accepted) return;
            const int chosen_index = names.indexOf(chosen);
            if (chosen_index < 0 || chosen_index >= static_cast<int>(document_->roles().size())) return;
            role = document_->roles()[static_cast<std::size_t>(chosen_index)].id;
        }
        auto& inserted = document_->add_line(role, selected_timeline_track_, in, out);
        const int inserted_row = static_cast<int>(&inserted - document_->lines().data());
        setDirty(true); refreshTable();
        lines_table_->selectRow(inserted_row);
        lines_table_->scrollToItem(lines_table_->item(inserted_row, 4));
        timeline_->setSelection(inserted_row, selected_timeline_track_);
        status_label_->setText(QStringLiteral("Réplique créée sur la piste %1 à %2.")
                               .arg(selected_timeline_track_ + 1).arg(qstr(in.format())));
        editTimelineLine(inserted_row);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Création impossible", QString::fromUtf8(error.what()));
    }
}

void MainWindow::editTimelineLine(int row) {
    if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
    const auto& original = document_->lines()[static_cast<std::size_t>(row)];
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Modifier la réplique"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* role = new QComboBox(&dialog);
    role->addItem(QStringLiteral("Sans rôle"), QString{});
    for (const auto& item : document_->roles()) role->addItem(qstr(item.name), qstr(item.id));
    int current = role->findData(qstr(original.role_id));
    if (current < 0 && !original.role_id.empty()) { role->addItem(qstr(original.role_id), qstr(original.role_id)); current = role->count() - 1; }
    role->setCurrentIndex(std::max(0, current));
    auto* text = new QLineEdit(&dialog);
    QString initial_text = qstr(original.text);
    initial_text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
    text->setText(initial_text);
    text->setMinimumWidth(480);
    layout->addWidget(new QLabel(QStringLiteral("Personnage"), &dialog));
    layout->addWidget(role);
    layout->addWidget(new QLabel(QStringLiteral("Texte de la réplique"), &dialog));
    layout->addWidget(text, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(text, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    try {
        auto& line = document_->lines()[static_cast<std::size_t>(row)];
        document_->set_line_text(line, utf8(text->text()));
        auto& updated = document_->lines()[static_cast<std::size_t>(row)];
        updated.role_id = utf8(role->currentData().toString());
        updated.element->set_attribute("role", updated.role_id);
        setDirty(true);
        refreshTable();
        if (row < lines_table_->rowCount()) lines_table_->selectRow(row);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Modification impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::splitSelectedLine() {
    if (!document_) return;
    const int row = lines_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(document_->lines().size())) {
        status_label_->setText("Sélectionnez une réplique avant de la scinder.");
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Scinder la réplique"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Saisissez le timecode du point de coupe (HH:MM:SS:FF)."), &dialog));
    auto* cut_timecode = new QLineEdit(qstr(currentTimecode().format()), &dialog);
    layout->addWidget(cut_timecode);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Scinder"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    try {
        const auto cut = Timecode::parse(utf8(cut_timecode->text().trimmed()));
        document_->split_line_at(document_->lines()[static_cast<std::size_t>(row)], cut);
        setDirty(true);
        refreshTable();
        lines_table_->selectRow(row);
        timeline_->setSelection(row, document_->lines()[static_cast<std::size_t>(row)].track);
        timeline_->setFocus(Qt::OtherFocusReason);
        status_label_->setText("Réplique scindée au timecode choisi. Chaque partie a son propre intervalle rythmo.");
    } catch (const std::exception& error) {
        QMessageBox::information(this, "Scission impossible", QString::fromUtf8(error.what()));
    }
}

void MainWindow::toggleRhythmFullscreen() {
    if (!full_screen_host_) {
        full_screen_host_ = new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
        full_screen_host_->setWindowTitle(QStringLiteral("Rythmo by Dubia Productions | Vidéo + bande rythmo"));
        full_screen_layout_ = new QVBoxLayout(full_screen_host_);
        full_screen_layout_->setContentsMargins(0, 0, 0, 0);
        full_screen_layout_->setSpacing(0);
        auto* video_canvas = new FullscreenVideoCanvas(full_screen_host_);
        full_screen_video_canvas_ = video_canvas;
        full_screen_layout_->addWidget(video_canvas, 3);
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), full_screen_host_);
        connect(escape, &QShortcut::activated, this, [this] {
            if (full_screen_active_) toggleRhythmFullscreen();
        });
        auto* f11 = new QShortcut(QKeySequence(Qt::Key_F11), full_screen_host_);
        connect(f11, &QShortcut::activated, this, [this] {
            if (full_screen_active_) toggleRhythmFullscreen();
        });
        auto* space_play = new QShortcut(QKeySequence(Qt::Key_Space), full_screen_host_);
        space_play->setContext(Qt::WidgetWithChildrenShortcut);
        connect(space_play, &QShortcut::activated, this, [this] {
            if (!full_screen_active_) return;
            if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
            else player_->play();
        });
    }

    if (!full_screen_active_) {
        right_layout_->removeWidget(timeline_);
        full_screen_layout_->addWidget(timeline_, 2);
        static_cast<FullscreenVideoCanvas*>(full_screen_video_canvas_)->setForegroundVideo(video_widget_);
        timeline_->setCleanPresentation(true);
        full_screen_active_ = true;
        hide();
        full_screen_host_->showFullScreen();
        timeline_->setFocus(Qt::OtherFocusReason);
    } else {
        full_screen_layout_->removeWidget(timeline_);
        timeline_->setCleanPresentation(false);
        static_cast<FullscreenVideoCanvas*>(full_screen_video_canvas_)->setForegroundVideo(nullptr);
        video_widget_->setParent(right_layout_->parentWidget());
        right_layout_->insertWidget(1, timeline_, 2);
        right_layout_->insertWidget(0, video_widget_, 3);
        video_widget_->show();
        full_screen_active_ = false;
        full_screen_host_->hide();
        showNormal();
        raise();
        activateWindow();
    }
}

void MainWindow::exportVideoWithRhythm() {
    if (export_active_) return;
    if (!document_ || video_path_.isEmpty() || player_->duration() <= 0) {
        QMessageBox::information(this, "Export vidéo", "Ouvrez un document DetX et sa vidéo source avant l’export.");
        return;
    }
    const QString ffmpeg = findBundledOrSystemTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(this, "FFmpeg introuvable",
            "Installez une version complète de FFmpeg et ajoutez son dossier bin au PATH. "
            "L’export utilise FFmpeg pour détecter les encodeurs et créer la vidéo.");
        return;
    }

    QProcess encoder_probe;
    encoder_probe.setProcessChannelMode(QProcess::MergedChannels);
    encoder_probe.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
    if (!encoder_probe.waitForStarted(3000) || !encoder_probe.waitForFinished(10000)) {
        QMessageBox::warning(this, "FFmpeg indisponible", "Impossible d’interroger FFmpeg.");
        return;
    }
    const QString encoder_listing = QString::fromLocal8Bit(encoder_probe.readAll());
    const auto has_encoder = [&](const QString& name) {
        for (const auto& line : encoder_listing.split('\n')) {
            if (line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(name)) return true;
        }
        return false;
    };

    const QString ffprobe = findBundledOrSystemTool(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) {
        QMessageBox::warning(this, "ffprobe introuvable", "L’export a besoin de ffprobe, fourni avec FFmpeg, pour lire la définition et la cadence de la vidéo.");
        return;
    }
    QProcess media_probe;
    media_probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=width,height"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=0"), video_path_});
    if (!media_probe.waitForStarted(3000) || !media_probe.waitForFinished(10000) || media_probe.exitCode() != 0) {
        QMessageBox::warning(this, "Lecture vidéo impossible", "ffprobe n’a pas pu analyser la vidéo source.");
        return;
    }
    QMap<QString, QString> metadata;
    const auto metadata_lines = QString::fromLocal8Bit(media_probe.readAllStandardOutput()).split('\n');
    for (const auto& line : metadata_lines) {
        const int separator = line.indexOf('=');
        if (separator > 0) metadata.insert(line.left(separator).trimmed(), line.mid(separator + 1).trimmed());
    }
    bool width_ok = false, height_ok = false;
    int source_width = metadata.value(QStringLiteral("width")).toInt(&width_ok);
    int source_height = metadata.value(QStringLiteral("height")).toInt(&height_ok);
    if (!width_ok || !height_ok || source_width < 16 || source_height < 16 || source_width > 8192 || source_height > 8192) {
        QMessageBox::warning(this, "Définition non prise en charge", "La définition vidéo est absente ou invalide.");
        return;
    }
    constexpr double fps = 60.0;
    const QString fps_rational = QStringLiteral("60/1");

    QString selected_filter;
    QSettings export_settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    QString export_directory = export_settings.value(QStringLiteral("lastVideoExportDirectory")).toString();
    if (!export_directory.isEmpty() && QDir(export_directory).exists()) {
        QTemporaryFile remembered_directory_probe(QDir(export_directory).filePath(QStringLiteral(".rythmo-export-check-XXXXXX")));
        remembered_directory_probe.setAutoRemove(true);
        if (!remembered_directory_probe.open()) export_directory.clear();
    }
    if (export_directory.isEmpty() || !QDir(export_directory).exists()) {
        QString base_directory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (base_directory.isEmpty()) base_directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (base_directory.isEmpty()) base_directory = QDir::homePath();
        const QString proposed_directory = QDir(base_directory).filePath(QStringLiteral("Rythmo Exports"));
        export_directory = QDir().mkpath(proposed_directory) ? proposed_directory : base_directory;
    }
    const QString suggested_name = QFileInfo(video_path_).completeBaseName() + QStringLiteral("_rythmo.mp4");
    const QString suggested_path = QDir(export_directory).filePath(suggested_name);
    QString output = QFileDialog::getSaveFileName(this, "Exporter", suggested_path,
        "MP4 (*.mp4);;MOV QuickTime (*.mov);;AVI (*.avi)", &selected_filter);
    if (output.isEmpty()) return;
    // Resolve relative paths returned by a native file dialog so FFmpeg writes
    // beside the file selected by the user, not into the app's working folder.
    output = QFileInfo(output).absoluteFilePath();
    QString extension = QFileInfo(output).suffix().toLower();
    if (extension.isEmpty()) {
        extension = selected_filter.contains("MOV", Qt::CaseInsensitive) ? "mov" :
                    (selected_filter.contains("AVI", Qt::CaseInsensitive) ? "avi" : "mp4");
        output += QLatin1Char('.') + extension;
    }
    if (extension != "mp4" && extension != "mov" && extension != "avi") {
        QMessageBox::warning(this, "Format non pris en charge", "Choisissez un nom de fichier en .mp4, .mov ou .avi.");
        return;
    }
    if (QFileInfo(output).absoluteFilePath().compare(QFileInfo(video_path_).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        QMessageBox::warning(this, "Destination invalide", "Le fichier vidéo source ne peut pas être remplacé par son export.");
        return;
    }
    const QFileInfo output_info(output);
    if (!output_info.dir().exists()) {
        QMessageBox::warning(this, "Dossier de destination introuvable",
            QStringLiteral("Le dossier choisi n’existe pas :\n%1").arg(output_info.absolutePath()));
        return;
    }
    const QStorageInfo output_storage(output_info.absolutePath());
    if (output_storage.isValid() && output_storage.isReady() && output_storage.isReadOnly()) {
        QMessageBox::warning(this, "Destination en lecture seule",
            QStringLiteral("Le disque ou le dossier choisi est monté en lecture seule :\n%1\n\nChoisissez un dossier où vous pouvez créer des fichiers.")
                .arg(output_info.absolutePath()));
        return;
    }
    QTemporaryFile write_probe(output_info.dir().filePath(QStringLiteral(".rythmo-export-check-XXXXXX")));
    write_probe.setAutoRemove(true);
    if (!write_probe.open() || write_probe.write("Rythmo", 6) != 6 || !write_probe.flush()) {
        QMessageBox::warning(this, "Destination non accessible en écriture",
            QStringLiteral("Rythmo ne peut pas créer de fichiers dans le dossier choisi :\n%1\n\nDétail système : %2\n\nSur macOS, essayez le dossier Films/Rythmo Exports dans votre dossier utilisateur, puis vérifiez Réglages Système > Confidentialité et sécurité si le refus persiste.")
                .arg(output_info.absolutePath(), write_probe.errorString()));
        return;
    }
    write_probe.close();
    export_settings.setValue(QStringLiteral("lastVideoExportDirectory"), output_info.absolutePath());

    struct EncoderOption { QString label; QString name; bool allowed; };
    const std::vector<EncoderOption> encoder_options{
        {"H.264 (logiciel, libx264)", "libx264", true},
        {"H.264 (NVIDIA NVENC)", "h264_nvenc", true},
        {"H.264 (AMD AMF)", "h264_amf", true},
        {"H.264 (Intel Quick Sync)", "h264_qsv", true},
        {"H.264 (Apple VideoToolbox)", "h264_videotoolbox", true},
        {"H.265 / HEVC (logiciel, libx265)", "libx265", extension != "avi"},
        {"H.265 / HEVC (NVIDIA NVENC)", "hevc_nvenc", extension != "avi"},
        {"H.265 / HEVC (AMD AMF)", "hevc_amf", extension != "avi"},
        {"H.265 / HEVC (Intel Quick Sync)", "hevc_qsv", extension != "avi"},
        {"H.265 / HEVC (Apple VideoToolbox)", "hevc_videotoolbox", extension != "avi"},
        {"AV1 (logiciel, SVT-AV1)", "libsvtav1", extension != "avi"},
        {"AV1 (logiciel, libaom)", "libaom-av1", extension != "avi"},
        {"AV1 (NVIDIA NVENC)", "av1_nvenc", extension != "avi"},
        {"AV1 (AMD AMF)", "av1_amf", extension != "avi"},
        {"AV1 (Intel Quick Sync)", "av1_qsv", extension != "avi"}
    };
    QDialog codec_dialog(this);
    codec_dialog.setWindowTitle("Qualité et vitesse de l’export");
    auto* codec_layout = new QVBoxLayout(&codec_dialog);
    auto* hardware_info = new QLabel(QStringLiteral("Vérification des encodeurs utilisables sur cet appareil…"), &codec_dialog);
    hardware_info->setWordWrap(true);
    codec_layout->addWidget(hardware_info);
    codec_dialog.setMinimumWidth(620);
    auto* codec_form = new QFormLayout;
    codec_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    codec_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    codec_form->setVerticalSpacing(14);
    const auto prepare_export_combo = [](QComboBox* combo) {
        // macOS native combo metrics do not account for the stylesheet padding.
        // Give both the label and its content rect explicit, font-derived room.
        if (auto* style = QStyleFactory::create(QStringLiteral("Fusion"))) {
            style->setParent(combo);
            combo->setStyle(style);
        }
        combo->setStyleSheet(QStringLiteral(
            "QComboBox { min-height:1.4em; padding:6px 32px 6px 12px; color:#EEF3F8; }"
            "QComboBox::drop-down { width:26px; border:0; }"
            "QComboBox QAbstractItemView { color:#EEF3F8; background:#151F29; selection-color:#FFFFFF; }"));
        combo->ensurePolished();
        combo->setMinimumHeight(combo->fontMetrics().height() + 20);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };
    auto* resolution_combo = new QComboBox(&codec_dialog);
    prepare_export_combo(resolution_combo);
    resolution_combo->addItem(QStringLiteral("1280 × 720 · 60 i/s"), QSize(1280, 720));
    resolution_combo->addItem(QStringLiteral("1920 × 1080 · 60 i/s"), QSize(1920, 1080));
    resolution_combo->addItem(QStringLiteral("2560 × 1440 · 60 i/s"), QSize(2560, 1440));
    resolution_combo->addItem(QStringLiteral("3840 × 2160 · 60 i/s (4K)"), QSize(3840, 2160));
    resolution_combo->setCurrentIndex(1);
    codec_form->addRow(QStringLiteral("Résolution de sortie :"), resolution_combo);
    auto* codec_combo = new QComboBox(&codec_dialog);
    prepare_export_combo(codec_combo);
    codec_form->addRow("Encodeur :", codec_combo);
    auto* export_effects = new QCheckBox(QStringLiteral("Reproduire les effets du mode plein écran"), &codec_dialog);
    export_effects->setChecked(true);
    export_effects->setToolTip(QStringLiteral(
        "Ajoutez le fond vidéo flouté, le vignettage et les ombres latérales. Désactivez cette option pour accélérer l’export."));
    codec_form->addRow(QStringLiteral("Effets visuels :"), export_effects);
    codec_layout->addLayout(codec_form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &codec_dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Continuer");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText("Annuler");
    connect(buttons, &QDialogButtonBox::accepted, &codec_dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &codec_dialog, &QDialog::reject);
    codec_layout->addWidget(buttons);
    // A compiled encoder is not necessarily usable on this GPU/driver. Probe
    // the actual size, rate, quality options and muxer once, then persist the
    // result for this machine and FFmpeg build between application launches.
    static QMap<QString, bool> tested_encoders;
    const QFileInfo ffmpeg_info(ffmpeg);
    const QByteArray machine_id = QSysInfo::machineUniqueId();
    const QString machine_identity = QString::fromLatin1(
        (machine_id.isEmpty() ? QSysInfo::machineHostName().toUtf8() : machine_id).toHex());
    const QString probe_identity = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(ffmpeg_info.canonicalFilePath(), QString::number(ffmpeg_info.size()),
             QString::number(ffmpeg_info.lastModified().toMSecsSinceEpoch()),
             machine_identity, QSysInfo::currentCpuArchitecture());
    QSettings codec_cache(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    codec_cache.beginGroup(QStringLiteral("encoderProbeCache/v1"));
    QTemporaryDir probe_directory;
    if (!probe_directory.isValid()) {
        QMessageBox::warning(this, "Export indisponible", "Impossible de créer le dossier temporaire de validation des encodeurs.");
        return;
    }
    const auto refresh_encoders = [&] {
        const QString previous = codec_combo->currentData().toString();
        const QSize size = resolution_combo->currentData().toSize();
        codec_combo->clear();
        codec_combo->setEnabled(false);
        resolution_combo->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        QProgressDialog progress("Vérification des encodeurs sur cet appareil…", "Annuler", 0,
                                 static_cast<int>(encoder_options.size()), &codec_dialog);
        progress.setWindowModality(Qt::WindowModal);
        // Cached capabilities populate immediately without flashing a modal
        // progress window on every export.
        progress.setMinimumDuration(500);
        progress.setAutoClose(false);
        int checked = 0;
        int preferred = -1;
        for (const auto& option : encoder_options) {
            progress.setValue(checked++);
            if (progress.wasCanceled()) break;
            if (!option.allowed || !has_encoder(option.name)) continue;
            const QString raw_key = probe_identity + QStringLiteral("|%1|%2x%3|%4")
                .arg(option.name).arg(size.width()).arg(size.height()).arg(extension);
            const QString key = QString::fromLatin1(
                QCryptographicHash::hash(raw_key.toUtf8(), QCryptographicHash::Sha256).toHex());
            bool available = tested_encoders.value(key, false);
            if (!tested_encoders.contains(key) && codec_cache.contains(key)) {
                available = codec_cache.value(key).toBool();
                tested_encoders.insert(key, available);
            } else if (!tested_encoders.contains(key)) {
                progress.setLabelText(QStringLiteral("Vérification : %1 · %2 × %3").arg(option.label).arg(size.width()).arg(size.height()));
                QProcess test;
                test.setProcessChannelMode(QProcess::MergedChannels);
                // Drain output continuously, including driver diagnostics.
                connect(&test, &QProcess::readyRead, &test, [&test] { test.readAll(); });
                QEventLoop loop;
                QTimer timeout;
                timeout.setSingleShot(true);
                connect(&test, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
                connect(&test, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
                connect(&timeout, &QTimer::timeout, &test, &QProcess::kill);
                connect(&progress, &QProgressDialog::canceled, &test, &QProcess::kill);
                QStringList probe_args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                    "-f", "lavfi", "-i", QStringLiteral("color=c=black:s=%1x%2:r=60").arg(size.width()).arg(size.height()),
                    "-frames:v", "1", "-pix_fmt", "yuv420p"};
                probe_args += videoExportCodecArguments(option.name, size.width(), size.height());
                probe_args << probe_directory.filePath(QStringLiteral("probe.%1").arg(extension));
                timeout.start(12000);
                test.start(ffmpeg, probe_args);
                loop.exec();
                available = timeout.isActive() && !progress.wasCanceled() &&
                    test.state() == QProcess::NotRunning && test.exitStatus() == QProcess::NormalExit && test.exitCode() == 0;
                if (test.state() != QProcess::NotRunning) { test.kill(); test.waitForFinished(1000); }
                if (!progress.wasCanceled()) {
                    tested_encoders.insert(key, available);
                    codec_cache.setValue(key, available);
                }
            }
            if (progress.wasCanceled()) break;
            if (!available) continue;
            codec_combo->addItem(option.label, option.name);
            const bool hardware = !option.name.startsWith(QStringLiteral("lib"));
            if (preferred < 0 && hardware) preferred = codec_combo->count() - 1;
        }
        codec_cache.sync();
        const bool canceled = progress.wasCanceled();
        progress.hide();
        const int previous_index = codec_combo->findData(previous);
        codec_combo->setCurrentIndex(previous_index >= 0 ? previous_index : (preferred >= 0 ? preferred : 0));
        resolution_combo->setEnabled(true);
        codec_combo->setEnabled(codec_combo->count() > 0);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(codec_combo->count() > 0 && !canceled);
        hardware_info->setText(canceled
            ? QStringLiteral("Vérification annulée. Change la résolution pour relancer la détection.")
            : codec_combo->count() == 0
                ? QStringLiteral("Aucun encodeur utilisable à cette résolution dans ce format. Essayez une autre résolution ou un autre format de fichier.")
                : QStringLiteral("Seuls les encodeurs validés sur cet appareil à cette résolution et à 60 i/s sont proposés. Un encodeur matériel est sélectionné en priorité."));
    };
    connect(resolution_combo, qOverload<int>(&QComboBox::currentIndexChanged), &codec_dialog, refresh_encoders);
    QTimer::singleShot(0, &codec_dialog, refresh_encoders);
    if (codec_dialog.exec() != QDialog::Accepted) return;
    const QString encoder = codec_combo->currentData().toString();
    const bool include_export_effects = export_effects->isChecked();
    const QSize output_resolution = resolution_combo->currentData().toSize();
    const int output_width = output_resolution.width();
    const int output_height = output_resolution.height();
    const int output_video_height = output_height * 3 / 5;
    const int output_band_height = output_height - output_video_height;

    bool use_coreimage = false;
#ifdef Q_OS_MACOS
    // Keep Apple's accelerated blur when this FFmpeg build supports it. Probe
    // the same downscale/blur/upscale stages used by the export graph; the
    // shared Qt overlay still draws the exact fullscreen vignette and shadows.
    if (include_export_effects) {
        const QSize probe_canvas(320, 180);
        const QSize probe_backdrop = fullscreenBackdropSize(probe_canvas);
        const int probe_radius = fullscreenBackdropBlurRadius(probe_backdrop.width());
        const QString probe_size = QStringLiteral("%1:%2").arg(probe_backdrop.width()).arg(probe_backdrop.height());
        const QString coreimage_chain = QStringLiteral("scale=%1:force_original_aspect_ratio=increase:flags=bilinear,crop=%1,"
            "format=rgba,coreimage=filter=CIGaussianBlur@inputRadius=%2,scale=%3:flags=bilinear")
            .arg(probe_size).arg(probe_radius).arg(QStringLiteral("320:180"));
        QProcess coreimage_test;
        coreimage_test.setProcessChannelMode(QProcess::MergedChannels);
        coreimage_test.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=c=black:s=160x90:d=0.1"),
            QStringLiteral("-vf"), coreimage_chain, QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
        use_coreimage = coreimage_test.waitForStarted(3000) && coreimage_test.waitForFinished(10000)
            && coreimage_test.exitStatus() == QProcess::NormalExit && coreimage_test.exitCode() == 0;
    }
#endif

    export_frame_width_ = output_width;
    export_video_height_ = output_video_height;
    export_band_height_ = output_band_height;
    export_effects_enabled_ = include_export_effects;
    export_encoder_ = encoder;
    export_fps_ = fps;
    if (!export_temp_dir_.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Dossier temporaire indisponible"),
            QStringLiteral("Rythmo ne peut pas préparer un fichier temporaire local pour l’export."));
        return;
    }
    export_output_path_ = output;
    // FFmpeg is a child process and may not inherit macOS's file-panel access
    // to protected folders. Encode inside Rythmo's private temp directory, then
    // let the GUI process copy the finished file to the selected destination.
    export_temp_path_ = QDir(export_temp_dir_.path()).filePath(
        QStringLiteral("%1.rythmo-partial.%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), extension));
    QFile::remove(export_temp_path_);
    export_frame_index_ = 0;
    export_frame_count_ = static_cast<qint64>(std::ceil(player_->duration() * fps / 1000.0));
    if (export_frame_count_ <= 0 || export_frame_count_ > 5'000'000) {
        QMessageBox::warning(this, "Durée non prise en charge", "La durée vidéo dépasse la limite d’export.");
        return;
    }

    if (include_export_effects && !export_temp_dir_.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Espace temporaire indisponible"),
            QStringLiteral("Impossible de préparer les effets plein écran dans le dossier temporaire du système."));
        return;
    }
    export_backdrop_overlay_path_.clear();
    if (include_export_effects) {
        export_backdrop_overlay_path_ = QDir(export_temp_dir_.path()).filePath(
            QStringLiteral("fullscreen-overlay-%1.rgba").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const QImage effect_overlay = fullscreenVideoOverlay(QSize(export_frame_width_, export_video_height_),
                                                              QSize(source_width, source_height))
            .convertToFormat(QImage::Format_RGBA8888);
        QFile overlay_file(export_backdrop_overlay_path_);
        if (effect_overlay.isNull() || !overlay_file.open(QIODevice::WriteOnly) ||
            overlay_file.write(reinterpret_cast<const char*>(effect_overlay.constBits()),
                               static_cast<qint64>(effect_overlay.sizeInBytes())) !=
                static_cast<qint64>(effect_overlay.sizeInBytes()) || !overlay_file.flush()) {
            overlay_file.close();
            QFile::remove(export_backdrop_overlay_path_);
            export_backdrop_overlay_path_.clear();
            QMessageBox::warning(this, QStringLiteral("Préparation des effets impossible"),
                QStringLiteral("Impossible de préparer les effets visuels de l’export."));
            return;
        }
        overlay_file.close();
    }

    const QString raw_size = QStringLiteral("%1x%2").arg(export_frame_width_).arg(export_band_height_);
    const QString filter = videoExportFilter(export_frame_width_, export_video_height_, fps_rational,
                                             use_coreimage, include_export_effects);
    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-hide_banner")};
#ifdef Q_OS_MACOS
    args << QStringLiteral("-hwaccel") << QStringLiteral("auto");
#endif
    args << QStringLiteral("-i") << video_path_;
    if (include_export_effects) {
        args << QStringLiteral("-f") << QStringLiteral("rawvideo")
            << QStringLiteral("-pixel_format") << QStringLiteral("rgba")
            << QStringLiteral("-video_size") << QStringLiteral("%1x%2").arg(export_frame_width_).arg(export_video_height_)
            << QStringLiteral("-framerate") << fps_rational
            << QStringLiteral("-i") << export_backdrop_overlay_path_;
    }
    args
        // The effects overlay is a single still image. FFmpeg's overlay filter
        // repeats its last frame by default. A raw RGBA frame avoids image codec
        // setup, compression/decompression, and repeated still-image decoding.
        << QStringLiteral("-f") << QStringLiteral("rawvideo")
        << QStringLiteral("-pixel_format") << QStringLiteral("rgba")
        << QStringLiteral("-video_size") << raw_size << QStringLiteral("-framerate") << fps_rational
        << QStringLiteral("-i") << QStringLiteral("-") << QStringLiteral("-filter_complex") << filter
        << QStringLiteral("-map") << QStringLiteral("[v]") << QStringLiteral("-map") << QStringLiteral("0:a?")
        << QStringLiteral("-shortest") << QStringLiteral("-r") << fps_rational
        << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    args += videoExportCodecArguments(encoder, output_width, output_height);
    if (extension == "avi") {
        args << "-c:a" << (has_encoder(QStringLiteral("libmp3lame")) ? QStringLiteral("libmp3lame") : QStringLiteral("pcm_s16le"));
    } else {
        args << "-c:a" << "aac";
    }
    if (extension == "mp4") args << "-movflags" << "+faststart";
    args << export_temp_path_;

    export_frame_image_ = QImage(export_frame_width_, export_band_height_, QImage::Format_RGBA8888);
    if (export_frame_image_.isNull()) {
        export_frame_width_ = export_video_height_ = export_band_height_ = 0;
        QFile::remove(export_backdrop_overlay_path_);
        export_backdrop_overlay_path_.clear();
        QMessageBox::warning(this, "Mémoire insuffisante", QStringLiteral("Impossible d’allouer le tampon du rendu %1 × %2.").arg(output_width).arg(output_height));
        return;
    }
    export_timeline_ = new RhythmTimelineWidget(this);
    export_timeline_->resize(export_frame_width_, export_band_height_);
    export_timeline_->setDocument(document_.get(), video_start_, document_->fps(), fps, player_->duration());
    export_timeline_->copyDisplayStateFrom(*timeline_);
    export_timeline_->setCleanPresentation(true);
    export_timeline_->ensurePolished();
    export_timeline_->hide();
    if (!export_frame_timer_) {
        export_frame_timer_ = new QTimer(this);
        export_frame_timer_->setInterval(0);
        connect(export_frame_timer_, &QTimer::timeout, this, [this] { writeNextExportFrame(); });
    }
    if (!export_process_) {
        export_process_ = new QProcess(this);
        export_process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(export_process_, &QProcess::bytesWritten, this, [this](qint64) {
            if (export_active_ && !export_cancelled_ && export_frame_index_ < export_frame_count_ &&
                !export_frame_timer_->isActive()) export_frame_timer_->start();
        });
        connect(export_process_, &QProcess::readyRead, this, [this] {
            export_log_tail_ += QString::fromLocal8Bit(export_process_->readAll());
            if (export_log_tail_.size() > 6000) export_log_tail_.remove(0, export_log_tail_.size() - 6000);
        });
        connect(export_process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { finishVideoExport(code, status); });
        connect(export_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finishVideoExport(-1, QProcess::CrashExit);
        });
    }
    export_progress_ = new QProgressDialog(QStringLiteral("Préparation du rendu %1 × %2 · 60 i/s…").arg(output_width).arg(output_height), "Annuler", 0, 1000, this);
    export_progress_->setLabelText(QStringLiteral("Préparation du rendu %1 × %2 · 60 i/s…").arg(output_width).arg(output_height));
    export_progress_->setWindowTitle(QStringLiteral("Export %1 × %2 · 60 i/s").arg(output_width).arg(output_height));
    export_progress_->setWindowModality(Qt::WindowModal);
    export_progress_->setMinimumDuration(0);
    connect(export_progress_, &QProgressDialog::canceled, this, [this] {
        if (!export_active_) return;
        export_cancelled_ = true;
        export_frame_timer_->stop();
        export_process_->kill();
    });

    export_log_tail_.clear();
    export_cancelled_ = false;
    export_active_ = true;
    export_process_->start(ffmpeg, args);
    if (!export_process_->waitForStarted(3000)) {
        finishVideoExport(-1, QProcess::CrashExit);
        return;
    }
    export_progress_clock_.start();
    export_frame_timer_->start();
}

void MainWindow::writeNextExportFrame() {
    if (export_pumping_ || !export_active_ || !export_process_ || export_process_->state() != QProcess::Running) return;
    QScopedValueRollback<bool> pumping(export_pumping_, true);
    const qint64 frame_bytes = static_cast<qint64>(export_frame_width_) * export_band_height_ * 4;
    QElapsedTimer batch_clock;
    batch_clock.start();
    const qint64 queue_limit = std::max<qint64>(frame_bytes, std::min<qint64>(frame_bytes * 3, 32 * 1024 * 1024));
    while (export_frame_index_ < export_frame_count_ && export_process_->bytesToWrite() < queue_limit) {

        // paintEvent fills the entire widget rectangle with an opaque background;
        // clearing the full 4K-sized buffer here only adds an avoidable memory pass.
        const qint64 position_ms = static_cast<qint64>(std::llround(export_frame_index_ * 1000.0 / export_fps_));
        export_timeline_->setPlayheadForRender(position_ms);
        QPainter painter(&export_frame_image_);
        export_timeline_->render(&painter);
        painter.end();
        const qint64 image_bytes = export_frame_image_.sizeInBytes();
        const qint64 written = export_process_->write(reinterpret_cast<const char*>(export_frame_image_.constBits()), image_bytes);
        if (written != image_bytes) {
            export_frame_timer_->stop();
            export_process_->kill();
            export_log_tail_ += QStringLiteral("\nÉchec de l’écriture des images rythmo vers FFmpeg.");
            return;
        }
        ++export_frame_index_;
        if (batch_clock.elapsed() >= 6) break;
    }
    if (export_frame_index_ >= export_frame_count_) {
        export_frame_timer_->stop();
        export_process_->closeWriteChannel();
        if (export_progress_) export_progress_->setLabelText("Finalisation de l’encodage…");
        return;
    }
    // Stop polling a full pipe; resume when FFmpeg has consumed buffered bytes.
    if (export_process_->bytesToWrite() >= queue_limit) export_frame_timer_->stop();
    if (export_progress_ && export_progress_clock_.elapsed() >= 100) {
        export_progress_clock_.restart();
        const QString pipeline = export_effects_enabled_
            ? export_encoder_ + QStringLiteral(" + effets visuels") : export_encoder_;
        export_progress_->setLabelText(QStringLiteral("Rendu bande rythmo : %1 / %2 images · %3")
            .arg(export_frame_index_).arg(export_frame_count_).arg(pipeline));
        export_progress_->setValue(static_cast<int>(export_frame_index_ * 1000 / export_frame_count_));
    }
}

void MainWindow::finishVideoExport(int exit_code, QProcess::ExitStatus status) {
    if (!export_active_) return;
    export_active_ = false;
    if (export_process_) export_log_tail_ += QString::fromLocal8Bit(export_process_->readAll());
    if (export_frame_timer_) export_frame_timer_->stop();
    if (!export_backdrop_overlay_path_.isEmpty()) {
        QFile::remove(export_backdrop_overlay_path_);
        export_backdrop_overlay_path_.clear();
    }
    const bool success = exit_code == 0 && status == QProcess::NormalExit;
    if (export_timeline_) {
        delete export_timeline_;
        export_timeline_ = nullptr;
    }
    export_frame_image_ = QImage{};
    if (export_progress_) {
        export_progress_->reset();
        export_progress_->deleteLater();
        export_progress_ = nullptr;
    }
    if (success) {
        QFile staged_file(export_temp_path_);
        QSaveFile destination(export_output_path_);
        destination.setDirectWriteFallback(false);
        bool committed = staged_file.open(QIODevice::ReadOnly) && staged_file.size() > 0
            && destination.open(QIODevice::WriteOnly);
        QString transfer_error;
        if (!committed) {
            transfer_error = staged_file.isOpen() ? destination.errorString() : staged_file.errorString();
        } else {
            QByteArray buffer(1024 * 1024, Qt::Uninitialized);
            while (!staged_file.atEnd()) {
                const qint64 bytes_read = staged_file.read(buffer.data(), buffer.size());
                if (bytes_read <= 0 || destination.write(buffer.constData(), bytes_read) != bytes_read) {
                    committed = false;
                    transfer_error = bytes_read <= 0 ? staged_file.errorString() : destination.errorString();
                    break;
                }
            }
            committed = committed && destination.commit();
            if (!committed && transfer_error.isEmpty()) transfer_error = destination.errorString();
        }
        if (!committed) destination.cancelWriting();
        QFile::remove(export_temp_path_);
        if (committed) {
            status_label_->setText("Export terminé : " + QFileInfo(export_output_path_).fileName());
            QMessageBox::information(this, "Export terminé", "La vidéo avec sa bande rythmo a été créée :\n" + export_output_path_);
        } else {
            QMessageBox::warning(this, "Enregistrement impossible",
                QStringLiteral("FFmpeg a terminé l’encodage, mais Rythmo n’a pas pu enregistrer le fichier dans le dossier choisi :\n%1\n\nDétail système : %2\n\nLe fichier existant, s’il y en avait un, a été conservé.")
                    .arg(QFileInfo(export_output_path_).absolutePath(), transfer_error));
        }
    } else if (export_cancelled_) {
        QFile::remove(export_temp_path_);
        status_label_->setText("Export vidéo annulé.");
    } else if (exit_code != 0 || status == QProcess::CrashExit) {
        QFile::remove(export_temp_path_);
        QMessageBox::warning(this, "Export échoué", QStringLiteral("FFmpeg n’a pas terminé l’export. Vérifiez l’encodeur, les pilotes et l’espace disque.\n\n%1")
            .arg(export_log_tail_.right(3000)));
    }
    export_cancelled_ = false;
}

void MainWindow::removeSelectedLine() {
    if (!document_ || lines_table_->currentRow() < 0) return;
    const int row = lines_table_->currentRow();
    if (QMessageBox::question(this, "Supprimer la réplique", "Supprimer la réplique sélectionnée ?") != QMessageBox::Yes) return;
    try {
        document_->remove_line(document_->lines()[static_cast<std::size_t>(row)]);
        setDirty(true); refreshTable();
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Suppression impossible", QString::fromUtf8(error.what()));
    }
}

void MainWindow::addRole() {
    if (!document_) return;
    bool ok = false;
    const auto name = QInputDialog::getText(this, "Ajouter un rôle", "Nom du rôle :", QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QString id = name.toLower();
    id.replace(QRegularExpression("[^a-z0-9]+"), "_");
    id.remove(QRegularExpression("^_+|_+$"));
    if (id.isEmpty()) id = "role";
    QString base = id; int suffix = 2;
    auto exists = [&](const QString& candidate) {
        return std::any_of(document_->roles().begin(), document_->roles().end(), [&](const Role& role) { return qstr(role.id) == candidate; });
    };
    while (exists(id)) id = base + "_" + QString::number(suffix++);
    try {
        document_->add_role(utf8(id), utf8(name)); setDirty(true); refreshTable();
        status_label_->setText("Rôle ajouté : " + name);
    } catch (const std::exception& error) { QMessageBox::warning(this, "Ajout impossible", QString::fromUtf8(error.what())); }
}

void MainWindow::manageRoles() {
    if (!document_) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Rôles du document"));
    dialog.setMinimumSize(360, 300);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    auto populate = [this, list] {
        list->clear();
        if (!document_) return;
        for (const auto& role : document_->roles()) {
            auto* item = new QListWidgetItem(qstr(role.name), list);
            item->setData(Qt::UserRole, qstr(role.id));
            item->setToolTip(qstr(role.id));
        }
    };
    populate();
    layout->addWidget(list, 1);
    auto* controls = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Ajouter"), &dialog);
    auto* remove = new QPushButton(QStringLiteral("Supprimer"), &dialog);
    auto* close = new QPushButton(QStringLiteral("Fermer"), &dialog);
    controls->addWidget(add); controls->addWidget(remove); controls->addStretch(1); controls->addWidget(close);
    layout->addLayout(controls);
    connect(add, &QPushButton::clicked, &dialog, [this, populate] { addRole(); populate(); });
    connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, list, populate] {
        auto* item = list->currentItem();
        if (!item || !document_) return;
        const QString name = item->text();
        const std::string id = utf8(item->data(Qt::UserRole).toString());
        const auto usage = std::count_if(document_->lines().begin(), document_->lines().end(),
                                         [&](const Line& line) { return line.role_id == id; });
        if (usage > 0) {
            QMessageBox::warning(&dialog, QStringLiteral("Rôle utilisé"),
                QStringLiteral("Impossible de supprimer « %1 » : ce rôle est encore utilisé par %2 réplique(s) de la bande rythmo.")
                    .arg(item->text()).arg(static_cast<qulonglong>(usage)));
            return;
        }
        try {
            document_->remove_role(id);
            setDirty(true);
            refreshTable();
            populate();
            status_label_->setText(QStringLiteral("Rôle supprimé : %1").arg(name));
        } catch (const std::exception& error) {
            QMessageBox::warning(&dialog, QStringLiteral("Suppression impossible"), QString::fromUtf8(error.what()));
        }
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::updateMediaMetadata() {
    if (!player_) return;
    const auto value = player_->metaData().value(QMediaMetaData::VideoFrameRate);
    const double detected = value.toDouble();
    if (std::isfinite(detected) && detected >= 1.0 && detected <= 240.0) {
        media_fps_ = detected;
        if (document_ && document_->fps_is_assumed()) {
            const int nominal = std::max(1, qRound(detected));
            try {
                document_->set_fps(nominal, true);
                timecode_fps_ = nominal;
                setDirty(true);
                status_label_->setText(QString("FPS source détecté : %1 ; cadence DetX réglée sur %2 i/s. Enregistrez pour conserver ce réglage.")
                                       .arg(detected, 0, 'f', 3).arg(nominal));
            } catch (const std::exception& error) {
                status_label_->setText("FPS vidéo détecté, mais impossible de l’appliquer : " + QString::fromUtf8(error.what()));
            }
        } else if (document_) {
            const auto difference = std::abs(detected - document_->fps());
            status_label_->setText(QString("Vidéo : %1 i/s · DetX : %2 i/s%3 | timecodes conservés, bandes calées sur la cadence du document.")
                .arg(detected, 0, 'f', 3).arg(document_->fps())
                .arg(document_->fps_is_assumed() ? " (cadence DetX implicite)" : (difference > 0.1 ? " (cadences différentes)" : "")));
        } else {
            status_label_->setText(QString("Vidéo chargée | %1 i/s").arg(detected, 0, 'f', 3));
        }
    }
    refreshTimeline();
    updatePosition(player_->position());
}

void MainWindow::updatePosition(qint64 position_ms) {
    if (!player_) return;
    current_position_ms_ = std::max<qint64>(0, position_ms);
    timeline_->setPlayhead(current_position_ms_);
    const qint64 duration = player_->duration();
    if (duration > 0) {
        const QSignalBlocker blocker(position_slider_);
        position_slider_->setValue(qBound(0, qRound(static_cast<double>(position_ms) * 1000.0 / duration), 1000));
    }
    const auto tc = currentTimecode();
    timecode_label_->setText("TC " + qstr(tc.format()));
}

void MainWindow::seekTimeline(qint64 position_ms) {
    if (!player_) return;
    const qint64 target = std::max<qint64>(0, player_->duration() > 0
        ? std::min(position_ms, player_->duration()) : position_ms);
    playback_anchor_position_ms_ = target;
    playback_seek_pending_ = true;
    if (player_->playbackState() == QMediaPlayer::PlayingState) playback_clock_.restart();
    player_->setPosition(target);
    updatePosition(target);
}

void MainWindow::seekFromTableRow(int row) {
    if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
    const auto& line = document_->lines()[static_cast<std::size_t>(row)];
    if (!line.lipsyncs.empty()) seekTimeline(timeline_->positionForTimecode(line.lipsyncs.front().timecode));
}

Timecode MainWindow::currentTimecode() const {
    if (!document_) return {};
    const auto elapsed_frames = qRound64(static_cast<double>(current_position_ms_) * timecode_fps_ / 1000.0);
    const auto base = video_start_.to_frames(timecode_fps_);
    return Timecode::from_frames(base + elapsed_frames, timecode_fps_);
}

void MainWindow::setDirty(bool dirty) {
    if (document_) {
        const std::string snapshot = document_->serialize();
        if (!undo_snapshot_initialized_) {
            current_document_snapshot_ = snapshot;
            undo_snapshot_initialized_ = true;
        } else if (!restoring_history_ && dirty && snapshot != current_document_snapshot_) {
            if (!resizing_timeline_edge_) {
                undo_history_.push_back(current_document_snapshot_);
                if (undo_history_.size() > 100) undo_history_.erase(undo_history_.begin());
                redo_history_.clear();
            }
            current_document_snapshot_ = snapshot;
        } else {
            current_document_snapshot_ = snapshot;
        }
        if (!dirty) saved_document_snapshot_ = snapshot;
        dirty_ = snapshot != saved_document_snapshot_;
    } else {
        dirty_ = dirty;
    }
    const auto name = document_path_.isEmpty() ? QString("Sans titre") : QFileInfo(document_path_).fileName();
    setWindowTitle((dirty_ ? "*" : "") + name + " | Rythmo by Dubia Productions");
}

void MainWindow::resetUndoHistory(bool saved_state) {
    undo_history_.clear();
    redo_history_.clear();
    undo_snapshot_initialized_ = document_ != nullptr;
    current_document_snapshot_ = document_ ? document_->serialize() : std::string{};
    saved_document_snapshot_ = saved_state ? current_document_snapshot_ : std::string{};
}

void MainWindow::undoLastChange() {
    if (!document_ || undo_history_.empty()) return;
    redo_history_.push_back(current_document_snapshot_);
    current_document_snapshot_ = std::move(undo_history_.back());
    undo_history_.pop_back();
    try {
        restoring_history_ = true;
        document_ = std::make_unique<DetxDocument>(DetxDocument::from_string(current_document_snapshot_));
        refreshTable();
        refreshTimeline();
        setDirty(true);
        restoring_history_ = false;
        status_label_->setText(QStringLiteral("Dernière modification annulée."));
    } catch (const std::exception& error) {
        restoring_history_ = false;
        QMessageBox::warning(this, QStringLiteral("Annulation impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::redoLastChange() {
    if (!document_ || redo_history_.empty()) return;
    undo_history_.push_back(current_document_snapshot_);
    current_document_snapshot_ = std::move(redo_history_.back());
    redo_history_.pop_back();
    try {
        restoring_history_ = true;
        document_ = std::make_unique<DetxDocument>(DetxDocument::from_string(current_document_snapshot_));
        refreshTable();
        refreshTimeline();
        setDirty(true);
        restoring_history_ = false;
        status_label_->setText(QStringLiteral("Modification rétablie."));
    } catch (const std::exception& error) {
        restoring_history_ = false;
        QMessageBox::warning(this, QStringLiteral("Rétablissement impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (confirmDiscardChanges()) event->accept(); else event->ignore();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched && watched->objectName() == QStringLiteral("voiceDropZone")) {
        if (event->type() == QEvent::DragEnter) {
            auto* drag = static_cast<QDragEnterEvent*>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            auto* sources = qobject_cast<QListWidget*>(watched->property("sourceListWidget").value<QObject*>());
            if (sources && drop->mimeData()->hasUrls()) {
                for (const auto& url : drop->mimeData()->urls()) {
                    if (!url.isLocalFile()) continue;
                    const QFileInfo info(url.toLocalFile());
                    if (!info.exists() || !info.isFile()) continue;
                    bool duplicate = false;
                    for (int i = 0; i < sources->count(); ++i) {
                        const auto existing = sources->item(i)->data(Qt::UserRole).toString();
                        if (QDir::cleanPath(existing).compare(QDir::cleanPath(info.absoluteFilePath()), Qt::CaseInsensitive) == 0) duplicate = true;
                    }
                    if (duplicate) continue;
                    auto* item = new QListWidgetItem(info.fileName(), sources);
                    item->setToolTip(info.absoluteFilePath());
                    item->setData(Qt::UserRole, info.absoluteFilePath());
                }
                drop->acceptProposedAction();
                return true;
            }
        }
    }
    if (event->type() == QEvent::KeyPress && !QApplication::activeModalWidget()) {
        QWidget* focus = QApplication::focusWidget();
        const bool in_this_window = focus && (focus == this || isAncestorOf(focus));
        const bool editing_text = qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) ||
                                  qobject_cast<QPlainTextEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                                  qobject_cast<QComboBox*>(focus);
        if (in_this_window && !editing_text) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::Undo)) {
                undoLastChange();
                event->accept();
                return true;
            }
            if (key->matches(QKeySequence::Redo)) {
                redoLastChange();
                event->accept();
                return true;
            }
        }
    }
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space) {
            QWidget* focus = QApplication::focusWidget();
            const bool editing_text = qobject_cast<QLineEdit*>(focus) ||
                                      qobject_cast<QTextEdit*>(focus) ||
                                      qobject_cast<QPlainTextEdit*>(focus) ||
                                      qobject_cast<QAbstractSpinBox*>(focus) ||
                                      qobject_cast<QComboBox*>(focus);
            if (editing_text) {
                // Let the focused editor receive Space as text instead of activating playback.
                event->accept();
                return false;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace rythmo

