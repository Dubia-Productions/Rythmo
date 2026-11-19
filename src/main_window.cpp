#include "rythmo/main_window.hpp"
#include "rythmo/rhythm_timeline.hpp"
#include "rythmo/video_export_filter.hpp"
#include "rythmo/video_backdrop.hpp"

#include <QAudioOutput>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSpacerItem>
#include <QMap>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenu>
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
#include <QSpinBox>
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
#include <QStyledItemDelegate>
#include <QCursor>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QToolBar>
#include <QTimer>
#include <QThread>
#include <QThreadPool>
#include <QRegion>
#include <QUrl>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStandardItemModel>
#include <QUuid>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
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

bool isSupportedVideoSuffix(const QString& suffix) {
    static const QStringList suffixes{
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("wmv"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("m2v"), QStringLiteral("ts"), QStringLiteral("mts"),
        QStringLiteral("m2ts"), QStringLiteral("mxf"), QStringLiteral("flv"), QStringLiteral("3gp"),
        QStringLiteral("ogv"), QStringLiteral("vob"), QStringLiteral("ivf"), QStringLiteral("av1")};
    return suffixes.contains(suffix.toLower());
}

const std::array<QString, 10>& studioRolePalette() {
    static const std::array<QString, 10> palette{
        QStringLiteral("#60A5FA"), QStringLiteral("#34D399"), QStringLiteral("#F472B6"),
        QStringLiteral("#FBBF24"), QStringLiteral("#A78BFA"), QStringLiteral("#FB7185"),
        QStringLiteral("#38BDF8"), QStringLiteral("#FB923C"), QStringLiteral("#4ADE80"),
        QStringLiteral("#E879F9")};
    return palette;
}

QColor displayRoleColor(const std::string& hex) {
    const QColor parsed(qstr(hex));
    if (!parsed.isValid() || parsed == QColor(Qt::black)) return QColor(QStringLiteral("#60A5FA"));
    return parsed;
}

QIcon makeRoleSwatchIcon(const QColor& color, int logical_size = 12) {
    const QColor resolved = color.isValid() ? color : QColor(QStringLiteral("#60A5FA"));
    const quint64 key = (static_cast<quint64>(resolved.rgba()) << 16) ^ static_cast<quint64>(logical_size);
    static QHash<quint64, QIcon> cache;
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    const int px = logical_size * 2;
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1.0));
    p.setBrush(resolved);
    p.drawEllipse(QRectF(1.5, 1.5, logical_size - 3.0, logical_size - 3.0));
    p.end();
    QIcon icon(pixmap);
    cache.insert(key, icon);
    return icon;
}

enum class StudioIcon {
    Home,
    Plus,
    Folder,
    Save,
    Undo,
    Redo,
    Play,
    Pause,
    StepBack,
    StepForward,
    PrevCue,
    NextCue,
    Split,
    Marker,
    Trash,
    Users,
    Font,
    Fullscreen,
    Export,
    Sparkles,
    Film,
    Volume,
    Mute,
    ZoomIn,
    ZoomOut
};

QIcon makeStudioIcon(StudioIcon kind, const QColor& color = QColor(QStringLiteral("#D4D4D8")), int size = 16) {
    const int px = size * 2;
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal s = static_cast<qreal>(size);
    switch (kind) {
    case StudioIcon::Home: {
        QPainterPath roof;
        roof.moveTo(2.2, s * 0.48);
        roof.lineTo(s * 0.5, 2.2);
        roof.lineTo(s - 2.2, s * 0.48);
        p.drawPath(roof);
        QPainterPath body;
        body.moveTo(4.0, s * 0.44);
        body.lineTo(4.0, s - 2.5);
        body.lineTo(s - 4.0, s - 2.5);
        body.lineTo(s - 4.0, s * 0.44);
        p.drawPath(body);
        break;
    }
    case StudioIcon::Plus: {
        p.drawLine(QPointF(s * 0.5, 3.0), QPointF(s * 0.5, s - 3.0));
        p.drawLine(QPointF(3.0, s * 0.5), QPointF(s - 3.0, s * 0.5));
        break;
    }
    case StudioIcon::Folder: {
        QPainterPath folder;
        folder.moveTo(2.2, 4.5);
        folder.lineTo(6.2, 4.5);
        folder.lineTo(7.8, 6.2);
        folder.lineTo(s - 2.2, 6.2);
        folder.lineTo(s - 2.2, s - 3.0);
        folder.lineTo(2.2, s - 3.0);
        folder.closeSubpath();
        p.drawPath(folder);
        break;
    }
    case StudioIcon::Save: {
        p.drawRoundedRect(QRectF(2.5, 2.5, s - 5.0, s - 5.0), 2.2, 2.2);
        p.drawLine(QPointF(5.2, 2.5), QPointF(5.2, 6.4));
        p.drawLine(QPointF(s - 5.2, 2.5), QPointF(s - 5.2, 6.4));
        p.drawLine(QPointF(5.2, 6.4), QPointF(s - 5.2, 6.4));
        p.drawRoundedRect(QRectF(4.8, s - 6.8, s - 9.6, 4.3), 1.0, 1.0);
        break;
    }
    case StudioIcon::Undo: {
        QPainterPath arc;
        arc.moveTo(3.2, 6.5);
        arc.lineTo(9.5, 6.5);
        arc.arcTo(QRectF(6.5, 6.5, 6.5, 6.5), 90.0, -180.0);
        arc.lineTo(5.5, 13.0);
        p.drawPath(arc);
        p.drawLine(QPointF(3.0, 6.5), QPointF(6.0, 3.7));
        p.drawLine(QPointF(3.0, 6.5), QPointF(6.0, 9.3));
        break;
    }
    case StudioIcon::Redo: {
        QPainterPath arc;
        arc.moveTo(s - 3.2, 6.5);
        arc.lineTo(s - 9.5, 6.5);
        arc.arcTo(QRectF(s - 13.0, 6.5, 6.5, 6.5), 90.0, 180.0);
        arc.lineTo(s - 5.5, 13.0);
        p.drawPath(arc);
        p.drawLine(QPointF(s - 3.0, 6.5), QPointF(s - 6.0, 3.7));
        p.drawLine(QPointF(s - 3.0, 6.5), QPointF(s - 6.0, 9.3));
        break;
    }
    case StudioIcon::Play: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF tri;
        tri << QPointF(4.5, 3.0) << QPointF(s - 3.0, s * 0.5) << QPointF(4.5, s - 3.0);
        p.drawPolygon(tri);
        break;
    }
    case StudioIcon::Pause: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(4.0, 3.0, 2.8, s - 6.0), 1.0, 1.0);
        p.drawRoundedRect(QRectF(s - 6.8, 3.0, 2.8, s - 6.0), 1.0, 1.0);
        break;
    }
    case StudioIcon::StepBack: {
        p.drawLine(QPointF(4.0, 3.8), QPointF(4.0, s - 3.8));
        p.drawLine(QPointF(s - 4.2, 3.8), QPointF(6.8, s * 0.5));
        p.drawLine(QPointF(6.8, s * 0.5), QPointF(s - 4.2, s - 3.8));
        break;
    }
    case StudioIcon::StepForward: {
        p.drawLine(QPointF(s - 4.0, 3.8), QPointF(s - 4.0, s - 3.8));
        p.drawLine(QPointF(4.2, 3.8), QPointF(s - 6.8, s * 0.5));
        p.drawLine(QPointF(s - 6.8, s * 0.5), QPointF(4.2, s - 3.8));
        break;
    }
    case StudioIcon::PrevCue: {
        p.drawLine(QPointF(3.0, 3.8), QPointF(3.0, s - 3.8));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF tri;
        tri << QPointF(s - 3.2, 3.8) << QPointF(5.5, s * 0.5) << QPointF(s - 3.2, s - 3.8);
        p.drawPolygon(tri);
        break;
    }
    case StudioIcon::NextCue: {
        p.drawLine(QPointF(s - 3.0, 3.8), QPointF(s - 3.0, s - 3.8));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF tri;
        tri << QPointF(3.2, 3.8) << QPointF(s - 5.5, s * 0.5) << QPointF(3.2, s - 3.8);
        p.drawPolygon(tri);
        break;
    }
    case StudioIcon::Split: {
        p.drawEllipse(QPointF(4.5, 4.8), 2.1, 2.1);
        p.drawEllipse(QPointF(4.5, s - 4.8), 2.1, 2.1);
        p.drawLine(QPointF(6.3, 5.8), QPointF(s - 2.8, s - 3.8));
        p.drawLine(QPointF(6.3, s - 5.8), QPointF(s - 2.8, 3.8));
        break;
    }
    case StudioIcon::Marker: {
        QPolygonF diamond;
        diamond << QPointF(s * 0.5, 2.5) << QPointF(s - 3.2, s * 0.45)
                << QPointF(s * 0.5, s - 5.0) << QPointF(3.2, s * 0.45);
        p.drawPolygon(diamond);
        p.drawLine(QPointF(s * 0.5, s - 5.0), QPointF(s * 0.5, s - 2.0));
        break;
    }
    case StudioIcon::Trash: {
        p.drawLine(QPointF(3.0, 4.8), QPointF(s - 3.0, 4.8));
        p.drawLine(QPointF(6.0, 2.8), QPointF(s - 6.0, 2.8));
        p.drawRoundedRect(QRectF(4.5, 4.8, s - 9.0, s - 7.5), 1.5, 1.5);
        p.drawLine(QPointF(6.8, 7.2), QPointF(6.8, s - 4.8));
        p.drawLine(QPointF(s - 6.8, 7.2), QPointF(s - 6.8, s - 4.8));
        break;
    }
    case StudioIcon::Users: {
        p.drawEllipse(QPointF(6.5, 5.5), 2.4, 2.4);
        QPainterPath body;
        body.moveTo(2.5, s - 3.0);
        body.cubicTo(2.5, s - 6.8, 10.5, s - 6.8, 10.5, s - 3.0);
        p.drawPath(body);
        p.drawEllipse(QPointF(11.3, 6.2), 1.8, 1.8);
        QPainterPath side;
        side.moveTo(10.8, s - 6.0);
        side.cubicTo(12.6, s - 6.0, 13.8, s - 4.8, 13.8, s - 3.0);
        p.drawPath(side);
        break;
    }
    case StudioIcon::Font: {
        QFont f = QApplication::font();
        f.setPixelSize(size - 4);
        f.setBold(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, QStringLiteral("Aa"));
        break;
    }
    case StudioIcon::Fullscreen: {
        p.drawLine(QPointF(2.8, 6.2), QPointF(2.8, 2.8));
        p.drawLine(QPointF(2.8, 2.8), QPointF(6.2, 2.8));
        p.drawLine(QPointF(s - 6.2, 2.8), QPointF(s - 2.8, 2.8));
        p.drawLine(QPointF(s - 2.8, 2.8), QPointF(s - 2.8, 6.2));
        p.drawLine(QPointF(2.8, s - 6.2), QPointF(2.8, s - 2.8));
        p.drawLine(QPointF(2.8, s - 2.8), QPointF(6.2, s - 2.8));
        p.drawLine(QPointF(s - 6.2, s - 2.8), QPointF(s - 2.8, s - 2.8));
        p.drawLine(QPointF(s - 2.8, s - 6.2), QPointF(s - 2.8, s - 2.8));
        break;
    }
    case StudioIcon::Export: {
        p.drawLine(QPointF(s * 0.5, s - 5.2), QPointF(s * 0.5, 3.0));
        p.drawLine(QPointF(s * 0.5, 3.0), QPointF(s * 0.5 - 3.0, 6.0));
        p.drawLine(QPointF(s * 0.5, 3.0), QPointF(s * 0.5 + 3.0, 6.0));
        QPainterPath tray;
        tray.moveTo(3.0, s - 6.2);
        tray.lineTo(3.0, s - 2.8);
        tray.lineTo(s - 3.0, s - 2.8);
        tray.lineTo(s - 3.0, s - 6.2);
        p.drawPath(tray);
        break;
    }
    case StudioIcon::Sparkles: {
        p.drawLine(QPointF(3.0, s * 0.5), QPointF(3.0, s * 0.5 + 0.1));
        p.drawLine(QPointF(5.5, s * 0.32), QPointF(5.5, s * 0.68));
        p.drawLine(QPointF(8.0, 2.6), QPointF(8.0, s - 2.6));
        p.drawLine(QPointF(10.5, s * 0.25), QPointF(10.5, s * 0.75));
        p.drawLine(QPointF(13.0, s * 0.4), QPointF(13.0, s * 0.6));
        break;
    }
    case StudioIcon::Film: {
        p.drawRoundedRect(QRectF(2.2, 3.2, s - 4.4, s - 6.4), 2.0, 2.0);
        p.drawLine(QPointF(5.4, 3.2), QPointF(5.4, s - 3.2));
        p.drawLine(QPointF(s - 5.4, 3.2), QPointF(s - 5.4, s - 3.2));
        p.drawLine(QPointF(2.2, s * 0.5), QPointF(5.4, s * 0.5));
        p.drawLine(QPointF(s - 5.4, s * 0.5), QPointF(s - 2.2, s * 0.5));
        break;
    }
    case StudioIcon::Volume: {
        QPolygonF spk;
        spk << QPointF(2.8, 6.2) << QPointF(5.5, 6.2) << QPointF(8.8, 3.5)
            << QPointF(8.8, s - 3.5) << QPointF(5.5, s - 6.2) << QPointF(2.8, s - 6.2);
        p.drawPolygon(spk);
        QPainterPath w1;
        w1.arcMoveTo(QRectF(7.8, 5.0, 4.5, s - 10.0), -55.0);
        w1.arcTo(QRectF(7.8, 5.0, 4.5, s - 10.0), -55.0, 110.0);
        p.drawPath(w1);
        break;
    }
    case StudioIcon::Mute: {
        QPolygonF spk;
        spk << QPointF(2.8, 6.2) << QPointF(5.5, 6.2) << QPointF(8.8, 3.5)
            << QPointF(8.8, s - 3.5) << QPointF(5.5, s - 6.2) << QPointF(2.8, s - 6.2);
        p.drawPolygon(spk);
        p.drawLine(QPointF(10.8, 5.8), QPointF(s - 2.5, s - 5.8));
        p.drawLine(QPointF(s - 2.5, 5.8), QPointF(10.8, s - 5.8));
        break;
    }
    case StudioIcon::ZoomIn: {
        p.drawEllipse(QPointF(7.0, 7.0), 4.2, 4.2);
        p.drawLine(QPointF(10.1, 10.1), QPointF(s - 2.6, s - 2.6));
        p.drawLine(QPointF(7.0, 5.0), QPointF(7.0, 9.0));
        p.drawLine(QPointF(5.0, 7.0), QPointF(9.0, 7.0));
        break;
    }
    case StudioIcon::ZoomOut: {
        p.drawEllipse(QPointF(7.0, 7.0), 4.2, 4.2);
        p.drawLine(QPointF(10.1, 10.1), QPointF(s - 2.6, s - 2.6));
        p.drawLine(QPointF(5.0, 7.0), QPointF(9.0, 7.0));
        break;
    }
    }
    p.end();
    return QIcon(pixmap);
}

QPixmap roundedLogoPixmap(int logical_size) {
    static const QPixmap raw(QStringLiteral(":/app/rythmo.png"));
    if (raw.isNull()) return {};
    const int px = std::max(16, logical_size * 2);
    QPixmap scaled = raw.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(2.0);
    return scaled;
}

QString studioStyleSheet() {
    return QStringLiteral(R"QSS(
        QWidget {
            background: #121214;
            color: #EDEDEF;
            font-size: 10.5pt;
        }
        QVideoWidget {
            background: #000000;
            border: 0;
        }
        QLabel {
            background: transparent;
            color: #EDEDEF;
        }
        QMainWindow, QDialog {
            background: #121214;
        }
        QMenuBar {
            background: #141417;
            border-bottom: 1px solid #232328;
            padding: 3px 8px;
            font-size: 10.5pt;
        }
        QMenuBar::item {
            background: transparent;
            color: #B4B4BC;
            padding: 5px 10px;
            border-radius: 5px;
        }
        QMenuBar::item:selected {
            background: #232329;
            color: #FFFFFF;
        }
        QMenu {
            background: #1A1A1E;
            border: 1px solid #2C2C33;
            border-radius: 7px;
            padding: 5px;
            font-size: 10.5pt;
        }
        QMenu::item {
            padding: 7px 26px 7px 12px;
            border-radius: 4px;
            color: #E4E4E7;
        }
        QMenu::item:selected {
            background: #2C2C34;
            color: #FFFFFF;
        }
        QMenu::separator {
            height: 1px;
            background: #26262C;
            margin: 4px 6px;
        }
        QPushButton {
            background: #1E1E22;
            border: 1px solid #2C2C33;
            border-radius: 6px;
            padding: 7px 14px;
            color: #E4E4E7;
            font-size: 10pt;
            font-weight: 600;
        }
        QPushButton:hover {
            background: #26262C;
            border-color: #3A3A44;
            color: #FFFFFF;
        }
        QPushButton:pressed {
            background: #18181C;
        }
        QPushButton:disabled {
            color: #52525B;
            background: #17171A;
            border-color: #222227;
        }
        QPushButton#primaryButton {
            background: #ECECEF;
            border: 1px solid #ECECEF;
            border-radius: 6px;
            padding: 7px 16px;
            color: #111113;
            font-size: 10pt;
            font-weight: 600;
        }
        QPushButton#primaryButton:hover {
            background: #FFFFFF;
            border-color: #FFFFFF;
            color: #09090B;
        }
        QPushButton#primaryButton:pressed {
            background: #D4D4D8;
            border-color: #D4D4D8;
        }
        QPushButton#primaryButton:disabled {
            background: #242429;
            border-color: #2C2C33;
            color: #63636B;
        }
        QPushButton#ghostButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 6px;
            padding: 6px 11px;
            color: #A8A8B2;
            font-size: 10pt;
            font-weight: 500;
        }
        QPushButton#ghostButton:hover {
            background: #222227;
            color: #F4F4F6;
        }
        QPushButton#ghostButton:pressed {
            background: #1B1B20;
        }
        QPushButton#ghostButton:disabled {
            color: #484850;
            background: transparent;
        }
        QPushButton#dangerGhostButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 6px;
            padding: 6px 11px;
            color: #A8A8B2;
            font-size: 10pt;
            font-weight: 500;
        }
        QPushButton#dangerGhostButton:hover {
            background: rgba(229, 72, 77, 0.12);
            color: #F87171;
        }
        QPushButton#transportPlayButton {
            background: #ECECEF;
            border: 1px solid #ECECEF;
            border-radius: 7px;
            padding: 0px;
            color: #111113;
        }
        QPushButton#transportPlayButton:hover {
            background: #FFFFFF;
            border-color: #FFFFFF;
        }
        QPushButton#transportPlayButton:pressed {
            background: #D4D4D8;
        }
        QPushButton#transportIconButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 6px;
            padding: 0px;
        }
        QPushButton#transportIconButton:hover {
            background: #232329;
            border-color: #2F2F37;
        }
        QPushButton#trackPill {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 6px;
            padding: 4px 11px;
            color: #92929C;
            font-size: 9.5pt;
            font-weight: 600;
        }
        QPushButton#trackPill:hover {
            background: #202026;
            color: #E4E4E7;
        }
        QPushButton#trackPill:checked {
            background: #282830;
            border-color: #3A3A44;
            color: #F4F4F6;
        }
        QFrame#editorTopBar {
            background: #161619;
            border-bottom: 1px solid #242429;
        }
        QFrame#paneHeader {
            background: #161619;
            border-bottom: 1px solid #242429;
        }
        QFrame#timelineStrip {
            background: #161619;
            border-bottom: 1px solid #242429;
        }
        QFrame#transportBar {
            background: #161619;
            border-top: 1px solid #242429;
        }
        QFrame#studioCard {
            background: #17171B;
            border: 1px solid #26262C;
            border-radius: 8px;
        }
        QFrame#vDivider {
            background: #27272E;
            border: 0;
            max-width: 1px;
            min-width: 1px;
        }
        QLabel#sectionCaption {
            color: #92929C;
            font-size: 10pt;
            font-weight: 600;
        }
        QLabel#timecodeReadout {
            color: #EDEDEF;
            background: transparent;
            font-family: "Menlo", "Consolas";
            font-size: 11pt;
            font-weight: 600;
            padding: 0px 6px;
        }
        QTableWidget {
            background: #121215;
            alternate-background-color: #151519;
            gridline-color: #1E1E24;
            border: 0;
            selection-background-color: rgba(255, 255, 255, 0.08);
            selection-color: #FFFFFF;
            outline: 0;
            font-size: 10.5pt;
        }
        QTableWidget::item {
            padding: 6px 10px;
            border-bottom: 1px solid #1C1C21;
        }
        QTableWidget::item:hover {
            background: #19191E;
        }
        QHeaderView::section {
            background: #161619;
            color: #92929C;
            border: 0;
            border-bottom: 1px solid #242429;
            padding: 8px 10px;
            font-size: 9.5pt;
            font-weight: 600;
        }
        QLineEdit, QComboBox, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {
            background: #19191D;
            border: 1px solid #2B2B32;
            border-radius: 6px;
            padding: 6px 10px;
            color: #EDEDEF;
            font-size: 10pt;
            selection-background-color: #3F3F46;
        }
        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #52525B;
            background: #1C1C21;
        }
        QComboBox::drop-down {
            border: 0;
            width: 22px;
        }
        QComboBox QAbstractItemView {
            background: #1A1A1E;
            color: #EDEDEF;
            border: 1px solid #2E2E36;
            selection-background-color: #2C2C34;
            selection-color: #FFFFFF;
            padding: 4px;
            font-size: 10pt;
        }
        QListWidget {
            background: #151518;
            border: 1px solid #25252B;
            border-radius: 7px;
            padding: 4px;
            outline: 0;
            font-size: 10.5pt;
        }
        QListWidget::item {
            padding: 9px 12px;
            border-radius: 5px;
            color: #E4E4E7;
        }
        QListWidget::item:hover {
            background: #1D1D22;
        }
        QListWidget::item:selected {
            background: #26262D;
            border: 1px solid #363640;
            color: #FFFFFF;
        }
        QCheckBox, QRadioButton {
            spacing: 8px;
            color: #D4D4D8;
            font-size: 10pt;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #3F3F46;
            border-radius: 4px;
            background: #18181C;
        }
        QCheckBox::indicator:checked {
            background: #E4E4E7;
            border-color: #E4E4E7;
        }
        QProgressBar {
            background: #18181C;
            border: 1px solid #28282F;
            border-radius: 5px;
            text-align: center;
            color: #EDEDEF;
            min-height: 16px;
        }
        QProgressBar::chunk {
            background: #D4D4D8;
            border-radius: 4px;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 10px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #2C2C34;
            min-height: 28px;
            border-radius: 4px;
        }
        QScrollBar::handle:vertical:hover {
            background: #454550;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 10px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal {
            background: #2C2C34;
            min-width: 28px;
            border-radius: 4px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #454550;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }
        QSlider::groove:horizontal {
            background: #28282F;
            height: 4px;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #D4D4D8;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #EDEDEF;
            border: 0;
            width: 12px;
            height: 12px;
            margin: -4px 0;
            border-radius: 6px;
        }
        QSlider::handle:horizontal:hover {
            background: #FFFFFF;
        }
        QSplitter::handle {
            background: #242429;
        }
        QSplitter::handle:horizontal {
            width: 1px;
        }
        QSplitter::handle:vertical {
            height: 1px;
        }
        QSplitter::handle:hover {
            background: #52525B;
        }
        QStatusBar {
            background: #141417;
            color: #8A8A94;
            border-top: 1px solid #222227;
            min-height: 26px;
            font-size: 9.5pt;
        }
        QStatusBar::item {
            border: 0;
        }
        QToolTip {
            background: #1C1C21;
            color: #EDEDEF;
            border: 1px solid #32323A;
            border-radius: 5px;
            padding: 5px 9px;
            font-size: 9.5pt;
        }
    )QSS");
}

inline QRgb yuvToRgbFast(int y, int u, int v) noexcept {
    // Fast integer BT.709 YCbCr -> RGB conversion (zero floating-point ops)
    const int c = std::max(0, y - 16) * 298;
    const int d = u - 128;
    const int e = v - 128;
    const int r = std::clamp((c + 459 * e + 128) >> 8, 0, 255);
    const int g = std::clamp((c - 55 * d - 136 * e + 128) >> 8, 0, 255);
    const int b = std::clamp((c + 541 * d + 128) >> 8, 0, 255);
    return qRgb(r, g, b);
}

// Smooth 2x2 area-filtered YUV/RGB downsample directly to the exact backdrop target size
// (eliminates full-4K frame.toImage() readback while matching v0.4.8's exact spatial & temporal smoothness!)
QImage sampleVideoFrameForBackdrop(QVideoFrame frame, QSize target) {
    const int src_w = frame.width();
    const int src_h = frame.height();
    if (src_w <= 0 || src_h <= 0 || target.isEmpty()) return {};

    // Aspect-ratio-expanding crop rectangle in source coordinates (matches Qt::KeepAspectRatioByExpanding + center crop)
    const double scale = std::max(static_cast<double>(target.width()) / src_w,
                                  static_cast<double>(target.height()) / src_h);
    const double crop_w = std::min<double>(src_w, target.width() / scale);
    const double crop_h = std::min<double>(src_h, target.height() / scale);
    const double crop_x0 = (src_w - crop_w) * 0.5;
    const double crop_y0 = (src_h - crop_h) * 0.5;

    const int out_w = target.width();
    const int out_h = target.height();
    const auto fmt = frame.pixelFormat();

    if (frame.map(QVideoFrame::ReadOnly)) {
        const auto unmap_guard = qScopeGuard([&frame] { frame.unmap(); });
        QImage out(out_w, out_h, QImage::Format_RGB32);
        if (out.isNull()) return {};

        const int step_x = std::max(1, static_cast<int>(crop_w / (out_w * 2.0)));
        const int step_y = std::max(1, static_cast<int>(crop_h / (out_h * 2.0)));

        if ((fmt == QVideoFrameFormat::Format_NV12 || fmt == QVideoFrameFormat::Format_NV21) &&
            frame.planeCount() >= 2 && frame.bits(0) && frame.bits(1)) {
            const uchar* y_plane = frame.bits(0);
            const uchar* uv_plane = frame.bits(1);
            const int y_stride = frame.bytesPerLine(0);
            const int uv_stride = frame.bytesPerLine(1);
            const bool nv21 = (fmt == QVideoFrameFormat::Format_NV21);
            for (int oy = 0; oy < out_h; ++oy) {
                const int sy0 = std::clamp(static_cast<int>(crop_y0 + (oy + 0.25) * crop_h / out_h), 0, src_h - 1);
                const int sy1 = std::min(src_h - 1, sy0 + step_y);
                const uchar* yr0 = y_plane + sy0 * y_stride;
                const uchar* yr1 = y_plane + sy1 * y_stride;
                const uchar* uvr = uv_plane + (sy0 >> 1) * uv_stride;
                auto* dst = reinterpret_cast<QRgb*>(out.scanLine(oy));
                for (int ox = 0; ox < out_w; ++ox) {
                    const int sx0 = std::clamp(static_cast<int>(crop_x0 + (ox + 0.25) * crop_w / out_w), 0, src_w - 1);
                    const int sx1 = std::min(src_w - 1, sx0 + step_x);
                    const int y_avg = (yr0[sx0] + yr0[sx1] + yr1[sx0] + yr1[sx1] + 2) >> 2;
                    const int uv_idx = (sx0 >> 1) << 1;
                    const int u = uvr[nv21 ? (uv_idx + 1) : uv_idx];
                    const int v = uvr[nv21 ? uv_idx : (uv_idx + 1)];
                    dst[ox] = yuvToRgbFast(y_avg, u, v);
                }
            }
            return out;
        }

        if ((fmt == QVideoFrameFormat::Format_YUV420P || fmt == QVideoFrameFormat::Format_YV12) &&
            frame.planeCount() >= 3 && frame.bits(0) && frame.bits(1) && frame.bits(2)) {
            const bool yv12 = (fmt == QVideoFrameFormat::Format_YV12);
            const uchar* y_plane = frame.bits(0);
            const uchar* u_plane = frame.bits(yv12 ? 2 : 1);
            const uchar* v_plane = frame.bits(yv12 ? 1 : 2);
            const int y_stride = frame.bytesPerLine(0);
            const int u_stride = frame.bytesPerLine(yv12 ? 2 : 1);
            const int v_stride = frame.bytesPerLine(yv12 ? 1 : 2);
            for (int oy = 0; oy < out_h; ++oy) {
                const int sy0 = std::clamp(static_cast<int>(crop_y0 + (oy + 0.25) * crop_h / out_h), 0, src_h - 1);
                const int sy1 = std::min(src_h - 1, sy0 + step_y);
                const uchar* yr0 = y_plane + sy0 * y_stride;
                const uchar* yr1 = y_plane + sy1 * y_stride;
                const uchar* ur = u_plane + (sy0 >> 1) * u_stride;
                const uchar* vr = v_plane + (sy0 >> 1) * v_stride;
                auto* dst = reinterpret_cast<QRgb*>(out.scanLine(oy));
                for (int ox = 0; ox < out_w; ++ox) {
                    const int sx0 = std::clamp(static_cast<int>(crop_x0 + (ox + 0.25) * crop_w / out_w), 0, src_w - 1);
                    const int sx1 = std::min(src_w - 1, sx0 + step_x);
                    const int y_avg = (yr0[sx0] + yr0[sx1] + yr1[sx0] + yr1[sx1] + 2) >> 2;
                    dst[ox] = yuvToRgbFast(y_avg, ur[sx0 >> 1], vr[sx0 >> 1]);
                }
            }
            return out;
        }

        if ((fmt == QVideoFrameFormat::Format_P010 || fmt == QVideoFrameFormat::Format_P016) &&
            frame.planeCount() >= 2 && frame.bits(0) && frame.bits(1)) {
            const uchar* y_plane = frame.bits(0);
            const uchar* uv_plane = frame.bits(1);
            const int y_stride = frame.bytesPerLine(0);
            const int uv_stride = frame.bytesPerLine(1);
            for (int oy = 0; oy < out_h; ++oy) {
                const int sy0 = std::clamp(static_cast<int>(crop_y0 + (oy + 0.25) * crop_h / out_h), 0, src_h - 1);
                const int sy1 = std::min(src_h - 1, sy0 + step_y);
                const auto* yr0 = reinterpret_cast<const quint16*>(y_plane + sy0 * y_stride);
                const auto* yr1 = reinterpret_cast<const quint16*>(y_plane + sy1 * y_stride);
                const auto* uvr = reinterpret_cast<const quint16*>(uv_plane + (sy0 >> 1) * uv_stride);
                auto* dst = reinterpret_cast<QRgb*>(out.scanLine(oy));
                for (int ox = 0; ox < out_w; ++ox) {
                    const int sx0 = std::clamp(static_cast<int>(crop_x0 + (ox + 0.25) * crop_w / out_w), 0, src_w - 1);
                    const int sx1 = std::min(src_w - 1, sx0 + step_x);
                    const int y_avg = ((yr0[sx0] >> 8) + (yr0[sx1] >> 8) + (yr1[sx0] >> 8) + (yr1[sx1] >> 8) + 2) >> 2;
                    const int uv_idx = (sx0 >> 1) << 1;
                    dst[ox] = yuvToRgbFast(y_avg, uvr[uv_idx] >> 8, uvr[uv_idx + 1] >> 8);
                }
            }
            return out;
        }

        if ((fmt == QVideoFrameFormat::Format_BGRA8888 ||
             fmt == QVideoFrameFormat::Format_BGRA8888_Premultiplied ||
             fmt == QVideoFrameFormat::Format_BGRX8888) &&
            frame.bits(0)) {
            const uchar* base = frame.bits(0);
            const int stride = frame.bytesPerLine(0);
            for (int oy = 0; oy < out_h; ++oy) {
                const int sy = std::clamp(static_cast<int>(crop_y0 + (oy + 0.5) * crop_h / out_h), 0, src_h - 1);
                const uchar* srow = base + sy * stride;
                auto* dst = reinterpret_cast<QRgb*>(out.scanLine(oy));
                for (int ox = 0; ox < out_w; ++ox) {
                    const int sx = std::clamp(static_cast<int>(crop_x0 + (ox + 0.5) * crop_w / out_w), 0, src_w - 1);
                    const uchar* p = srow + sx * 4;
                    dst[ox] = qRgb(p[2], p[1], p[0]);
                }
            }
            return out;
        }
    }

    return frame.toImage();
}

class FullscreenVideoCanvas final : public QWidget {
public:
    explicit FullscreenVideoCanvas(QWidget* parent = nullptr)
        : QWidget(parent),
          mailbox_(std::make_shared<BackdropMailbox>()) {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setMouseTracking(true);
        setMinimumHeight(200);
    }

    ~FullscreenVideoCanvas() override {
        std::lock_guard<std::mutex> lock(mailbox_->mutex);
        mailbox_->alive = false;
        mailbox_->has_pending = false;
        mailbox_->latest_frame = QVideoFrame{};
    }

    void setForegroundVideo(QVideoWidget* video) {
        if (frame_connection_) disconnect(frame_connection_);
        foreground_video_ = video;
        if (foreground_video_) {
            foreground_video_->setParent(this);
            foreground_video_->setMouseTracking(true);
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
        layoutForeground();
        if (!fitted_covers_canvas_) {
            rebuildEffectOverlay();
            if (foreground_video_ && foreground_video_->videoSink()) {
                const QVideoFrame current_frame = foreground_video_->videoSink()->videoFrame();
                if (current_frame.isValid()) updateFromFrame(current_frame);
            }
        }
    }

    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        const QRegion paint_region = exposed_region_.isEmpty()
            ? event->region()
            : event->region().intersected(exposed_region_);
        if (paint_region.isEmpty()) return;
        painter.setClipRegion(paint_region);

        if (!fitted_covers_canvas_ && !blurred_frame_.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(rect(), blurred_frame_);
            if (!effect_overlay_.isNull()) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                painter.drawImage(0, 0, effect_overlay_);
            }
        } else {
            painter.fillRect(rect(), Qt::black);
        }
    }

private:
    struct BackdropMailbox {
        std::mutex mutex;
        QVideoFrame latest_frame;
        QSize target_size;
        bool has_pending{false};
        bool worker_running{false};
        bool alive{true};
    };

    QMetaObject::Connection frame_connection_;
    std::shared_ptr<BackdropMailbox> mailbox_;
    QImage blurred_frame_;
    QImage effect_overlay_;
    QRegion exposed_region_;
    QSize source_size_{16, 9};
    QVideoWidget* foreground_video_{};
    bool fitted_covers_canvas_{};

    void updateFromFrame(const QVideoFrame& frame) {
        if (!frame.isValid()) return;
        const QSize incoming_size = frame.size();
        if (incoming_size.isValid() && incoming_size != source_size_) {
            source_size_ = incoming_size;
            layoutForeground();
            if (!fitted_covers_canvas_) {
                rebuildEffectOverlay();
            }
        }
        if (fitted_covers_canvas_ || width() <= 0 || height() <= 0) return;

        // Zero timer throttle: match 100% of the playing video's native framerate (24/25/30/50/60 fps)!
        // Logical 1x backdrop grid has zero frequencies above Nyquist after Gaussian-like blur and runs in ~0.35ms!
        const QSize target = fullscreenBackdropSize(size(), 1.0);
        bool need_spawn = false;
        {
            std::lock_guard<std::mutex> lock(mailbox_->mutex);
            if (!mailbox_->alive) return;
            mailbox_->latest_frame = frame;
            mailbox_->target_size = target;
            mailbox_->has_pending = true;
            if (!mailbox_->worker_running) {
                mailbox_->worker_running = true;
                need_spawn = true;
            }
        }
        if (!need_spawn) return;

        auto mb = mailbox_;
        QWidget* self = this;
        QThreadPool::globalInstance()->start([self, mb]() {
            for (;;) {
                QVideoFrame next_frame;
                QSize next_target;
                {
                    std::lock_guard<std::mutex> lock(mb->mutex);
                    if (!mb->alive || !mb->has_pending) {
                        mb->worker_running = false;
                        return;
                    }
                    next_frame = std::move(mb->latest_frame);
                    next_target = mb->target_size;
                    mb->has_pending = false;
                }

                QImage sampled = sampleVideoFrameForBackdrop(std::move(next_frame), next_target);
                QImage blurred = sampled.isNull() ? QImage{} : softVideoBackdrop(sampled, next_target);
                if (blurred.isNull()) continue;

                {
                    std::lock_guard<std::mutex> lock(mb->mutex);
                    if (!mb->alive) {
                        mb->worker_running = false;
                        return;
                    }
                }
                QMetaObject::invokeMethod(self, [self, mb, blurred = std::move(blurred)]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(mb->mutex);
                        if (!mb->alive) return;
                    }
                    auto* canvas = static_cast<FullscreenVideoCanvas*>(self);
                    canvas->blurred_frame_ = std::move(blurred);
                    if (!canvas->fitted_covers_canvas_ && !canvas->exposed_region_.isEmpty()) {
                        canvas->update(canvas->exposed_region_);
                    }
                }, Qt::QueuedConnection);
            }
        });
    }

    void rebuildEffectOverlay() {
        if (fitted_covers_canvas_ || width() <= 0 || height() <= 0) {
            effect_overlay_ = QImage{};
            return;
        }
        effect_overlay_ = fullscreenVideoOverlay(size(), source_size_);
    }

    void layoutForeground() {
        if (!foreground_video_ || source_size_.isEmpty() || width() <= 0 || height() <= 0) return;
        const qreal scale = std::min(width() / static_cast<qreal>(source_size_.width()),
                                     height() / static_cast<qreal>(source_size_.height()));
        const QSize fitted(qRound(source_size_.width() * scale), qRound(source_size_.height() * scale));
        fitted_covers_canvas_ = (fitted.width() >= width() - 2) && (fitted.height() >= height() - 2);
        const QRect geometry((width() - fitted.width()) / 2, (height() - fitted.height()) / 2,
                             fitted.width(), fitted.height());
        foreground_video_->setGeometry(geometry);
        foreground_video_->raise();
        exposed_region_ = fitted_covers_canvas_ ? QRegion{} : (QRegion(rect()) - QRegion(geometry));
    }
};

class LinesTableDelegate final : public QStyledItemDelegate {
public:
    explicit LinesTableDelegate(std::function<const DetxDocument*()> doc_provider, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), doc_provider_(std::move(doc_provider)) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        const int col = index.column();
        if (col == 2) {
            auto* combo = new QComboBox(parent);
            combo->addItem(makeRoleSwatchIcon(QColor(QStringLiteral("#52525B")), 11),
                           QStringLiteral("Sans rôle"), QString{});
            if (const DetxDocument* doc = doc_provider_ ? doc_provider_() : nullptr) {
                for (const auto& r : doc->roles()) {
                    combo->addItem(makeRoleSwatchIcon(displayRoleColor(r.color), 11),
                                   qstr(r.name), qstr(r.id));
                }
            }
            const QString current_id = index.data(Qt::UserRole).toString();
            int idx = combo->findData(current_id);
            if (idx < 0 && !current_id.isEmpty()) {
                combo->addItem(makeRoleSwatchIcon(QColor(QStringLiteral("#60A5FA")), 11), current_id, current_id);
                idx = combo->count() - 1;
            }
            combo->setCurrentIndex(std::max(0, idx));
            connect(combo, qOverload<int>(&QComboBox::activated), this, [this, combo](int) {
                auto* self = const_cast<LinesTableDelegate*>(this);
                emit self->commitData(combo);
                emit self->closeEditor(combo);
            });
            QTimer::singleShot(0, combo, &QComboBox::showPopup);
            return combo;
        }
        if (col == 3) {
            auto* combo = new QComboBox(parent);
            for (int t = 0; t < 4; ++t) {
                combo->addItem(QStringLiteral("Piste %1").arg(t + 1), t);
            }
            combo->setCurrentIndex(std::clamp(index.data(Qt::UserRole).toInt(), 0, 3));
            connect(combo, qOverload<int>(&QComboBox::activated), this, [this, combo](int) {
                auto* self = const_cast<LinesTableDelegate*>(this);
                emit self->commitData(combo);
                emit self->closeEditor(combo);
            });
            QTimer::singleShot(0, combo, &QComboBox::showPopup);
            return combo;
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (index.column() == 2 || index.column() == 3) return;
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            if (index.column() == 2) {
                const QString role_id = combo->currentData().toString();
                model->setData(index, role_id, Qt::UserRole);
                model->setData(index, combo->currentText(), Qt::DisplayRole);
                model->setData(index, combo->itemIcon(combo->currentIndex()), Qt::DecorationRole);
                return;
            }
            if (index.column() == 3) {
                const int track = std::clamp(combo->currentData().toInt(), 0, 3);
                model->setData(index, track, Qt::UserRole);
                model->setData(index, QStringLiteral("P%1").arg(track + 1), Qt::DisplayRole);
                return;
            }
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }

private:
    std::function<const DetxDocument*()> doc_provider_;
};

} // namespace

MainWindow::MainWindow(const QString& initial_document, QWidget* parent) : QMainWindow(parent) {
    static QString cached_ui_family;
    if (cached_ui_family.isEmpty()) {
        const int regular_font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-Regular.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-SemiBold.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Montserrat-Bold.ttf"));
        const QStringList loaded_families = regular_font_id >= 0
            ? QFontDatabase::applicationFontFamilies(regular_font_id)
            : QStringList{};
        cached_ui_family = loaded_families.isEmpty()
            ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()
            : loaded_families.front();
    }
    QFont ui_font(cached_ui_family, 11);
    ui_font.setStyleHint(QFont::SansSerif);
    qApp->setFont(ui_font);
    setStyleSheet(studioStyleSheet());
    setAcceptDrops(true);
    createUi();
    createMenus();
    qApp->installEventFilter(this);
    setWindowTitle(QStringLiteral("Rythmo by Dubia Productions"));
    resize(1540, 940);
    if (!initial_document.isEmpty()) loadDocument(initial_document);
    else showHome();
}

MainWindow::~MainWindow() {
    if (poster_frame_process_ && poster_frame_process_->state() != QProcess::NotRunning) {
        poster_frame_process_->kill();
        poster_frame_process_->waitForFinished(500);
    }
    if (decode_fallback_process_ && decode_fallback_process_->state() != QProcess::NotRunning) {
        decode_fallback_process_->kill();
        decode_fallback_process_->waitForFinished(1500);
    }
    if (player_) {
        player_->stop();
        player_->setSource(QUrl{});
    }
    if (!decode_proxy_path_.isEmpty() && !decode_proxy_is_persistent_) {
        QFile::remove(decode_proxy_path_);
    }
}

void MainWindow::createUi() {
    player_ = new QMediaPlayer(this);
    audio_output_ = new QAudioOutput(this);
    playback_visual_timer_ = new QTimer(this);
    playback_visual_timer_->setInterval(16);
    playback_visual_timer_->setTimerType(Qt::PreciseTimer);
    seek_coalesce_timer_ = new QTimer(this);
    seek_coalesce_timer_->setSingleShot(true);
    seek_coalesce_timer_->setInterval(90);
    connect(seek_coalesce_timer_, &QTimer::timeout, this, [this] {
        flushPendingMediaSeek();
    });
    video_decode_watchdog_timer_ = new QTimer(this);
    video_decode_watchdog_timer_->setSingleShot(true);
    video_decode_watchdog_timer_->setInterval(340);
    connect(video_decode_watchdog_timer_, &QTimer::timeout, this, [this] {
        if (video_path_.isEmpty() || decode_proxy_active_ || decode_fallback_attempted_ || received_video_frames_ > 0) {
            return;
        }
        const bool has_video_stream = player_->hasVideo() ||
            player_->metaData().value(QMediaMetaData::Resolution).isValid() ||
            media_fps_ > 0.0;
        if (!has_video_stream) return;
        fallback_resume_pos_ms_ = current_position_ms_;
        fallback_resume_playing_ = (player_->playbackState() == QMediaPlayer::PlayingState);
        requestInstantPosterFrame(video_path_, current_position_ms_);
        startFfmpegDecodeFallback(QStringLiteral("Codec vidéo non décodé nativement (ex. AV1/VP9)"));
    });
    player_->setAudioOutput(audio_output_);

    QSettings app_settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
    const int saved_volume = std::clamp(app_settings.value(QStringLiteral("audioVolume"), 100).toInt(), 0, 100);
    audio_output_->setVolume(saved_volume / 100.0f);

    auto* root = new QWidget(this);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto make_divider = [](QWidget* parent) {
        auto* div = new QFrame(parent);
        div->setObjectName(QStringLiteral("vDivider"));
        div->setFixedSize(1, 18);
        return div;
    };

    // 1. Minimal Header Bar
    auto* top_bar = new QFrame(root);
    top_bar->setObjectName(QStringLiteral("editorTopBar"));
    top_bar->setFixedHeight(48);
    auto* top_layout = new QHBoxLayout(top_bar);
    top_layout->setContentsMargins(14, 0, 14, 0);
    top_layout->setSpacing(10);

    auto* home_btn = new QPushButton(QStringLiteral("Accueil"), top_bar);
    home_btn->setObjectName(QStringLiteral("ghostButton"));
    home_btn->setIcon(makeStudioIcon(StudioIcon::Home, QColor(QStringLiteral("#A8A8B2")), 15));
    home_btn->setToolTip(QStringLiteral("Retourner à l’accueil"));

    document_title_label_ = new QLabel(QStringLiteral("Sans titre.detx"), top_bar);
    document_title_label_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11pt; color: #EDEDEF;"));

    project_meta_label_ = new QLabel(QStringLiteral("25 i/s"), top_bar);
    project_meta_label_->setStyleSheet(QStringLiteral("color: #82828C; font-size: 10pt; font-weight: 500;"));

    roles_button_ = new QPushButton(QStringLiteral("Rôles"), top_bar);
    roles_button_->setObjectName(QStringLiteral("ghostButton"));
    roles_button_->setToolTip(QStringLiteral("Gérer les personnages et leurs couleurs"));

    auto* fullscreen_btn = new QPushButton(QStringLiteral("Plein écran"), top_bar);
    fullscreen_btn->setObjectName(QStringLiteral("ghostButton"));
    fullscreen_btn->setToolTip(QStringLiteral("Vidéo et bande rythmo en plein écran (F11)"));

    auto* export_btn = new QPushButton(QStringLiteral("Exporter…"), top_bar);
    export_btn->setObjectName(QStringLiteral("primaryButton"));
    export_btn->setFixedHeight(32);
    export_btn->setStyleSheet(QStringLiteral("padding: 5px 15px; font-size: 10pt;"));
    export_btn->setToolTip(QStringLiteral("Exporter la vidéo avec sa bande rythmo (720p à 4K · 60 i/s)"));

    top_layout->addWidget(home_btn, 0, Qt::AlignVCenter);
    top_layout->addWidget(make_divider(top_bar), 0, Qt::AlignVCenter);
    top_layout->addSpacing(2);
    top_layout->addWidget(document_title_label_, 0, Qt::AlignVCenter);
    top_layout->addSpacing(6);
    top_layout->addWidget(project_meta_label_, 0, Qt::AlignVCenter);
    top_layout->addStretch(1);
    top_layout->addWidget(roles_button_, 0, Qt::AlignVCenter);
    top_layout->addWidget(fullscreen_btn, 0, Qt::AlignVCenter);
    top_layout->addSpacing(2);
    top_layout->addWidget(export_btn, 0, Qt::AlignVCenter);
    root_layout->addWidget(top_bar);

    // 2. Edge-to-Edge 3-Pane Workspace (1px hairline splitters)
    auto* vertical_splitter = new QSplitter(Qt::Vertical, root);
    vertical_splitter->setChildrenCollapsible(false);
    vertical_splitter->setHandleWidth(1);

    auto* upper_splitter = new QSplitter(Qt::Horizontal, vertical_splitter);
    upper_splitter->setChildrenCollapsible(false);
    upper_splitter->setHandleWidth(1);

    // 2A. Left Pane: Script / Répliques
    auto* cue_pane = new QWidget(upper_splitter);
    cue_pane->setMinimumWidth(460);
    auto* cue_pane_layout = new QVBoxLayout(cue_pane);
    cue_pane_layout->setContentsMargins(0, 0, 0, 0);
    cue_pane_layout->setSpacing(0);

    auto* cue_header = new QFrame(cue_pane);
    cue_header->setObjectName(QStringLiteral("paneHeader"));
    cue_header->setFixedHeight(44);
    auto* cue_header_layout = new QHBoxLayout(cue_header);
    cue_header_layout->setContentsMargins(14, 0, 12, 0);
    cue_header_layout->setSpacing(10);

    auto* cue_title = new QLabel(QStringLiteral("Répliques"), cue_header);
    cue_title->setStyleSheet(QStringLiteral("color: #E4E4E7; font-size: 10.5pt; font-weight: 600;"));
    lines_count_badge_ = new QLabel(QStringLiteral("0"), cue_header);
    lines_count_badge_->setStyleSheet(QStringLiteral("color: #82828C; font-size: 10pt; font-weight: 500;"));

    lines_search_edit_ = new QLineEdit(cue_header);
    lines_search_edit_->setPlaceholderText(QStringLiteral("Filtrer…"));
    lines_search_edit_->setClearButtonEnabled(true);
    lines_search_edit_->setMinimumWidth(140);
    lines_search_edit_->setMaximumWidth(240);
    lines_search_edit_->setFixedHeight(30);
    lines_search_edit_->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #121215; border: 1px solid #292930; border-radius: 6px; padding: 3px 10px; font-size: 10pt; }"
        "QLineEdit:focus { border-color: #52525B; }"));

    auto* add_line_button = new QPushButton(QStringLiteral("+ Réplique"), cue_header);
    add_line_button->setFixedHeight(30);
    add_line_button->setStyleSheet(QStringLiteral("padding: 4px 12px; font-size: 10pt; border-radius: 6px;"));
    add_line_button->setToolTip(QStringLiteral("Ajouter une réplique au curseur (Entrée sur la bande)"));

    cue_header_layout->addWidget(cue_title, 0, Qt::AlignVCenter);
    cue_header_layout->addWidget(lines_count_badge_, 0, Qt::AlignVCenter);
    cue_header_layout->addStretch(1);
    cue_header_layout->addWidget(lines_search_edit_, 0, Qt::AlignVCenter);
    cue_header_layout->addWidget(add_line_button, 0, Qt::AlignVCenter);
    cue_pane_layout->addWidget(cue_header);

    table_stack_ = new QStackedWidget(cue_pane);
    auto* empty_cues_page = new QWidget(table_stack_);
    empty_cues_page->setStyleSheet(QStringLiteral("background: #121215;"));
    auto* empty_cues_layout = new QVBoxLayout(empty_cues_page);
    empty_cues_layout->setContentsMargins(36, 36, 36, 36);
    empty_cues_layout->setSpacing(10);
    empty_cues_layout->addStretch(1);
    lines_empty_title_ = new QLabel(QStringLiteral("Aucune réplique"), empty_cues_page);
    lines_empty_title_->setAlignment(Qt::AlignCenter);
    lines_empty_title_->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: 600; color: #E4E4E7;"));
    lines_empty_subtitle_ = new QLabel(
        QStringLiteral("Ajoutez vos personnages puis cliquez sur « + Réplique » ou appuyez sur Entrée sur la bande rythmo."),
        empty_cues_page);
    lines_empty_subtitle_->setAlignment(Qt::AlignCenter);
    lines_empty_subtitle_->setWordWrap(true);
    lines_empty_subtitle_->setStyleSheet(QStringLiteral("color: #82828C; font-size: 10pt;"));
    auto* empty_cues_actions = new QHBoxLayout;
    empty_cues_actions->setSpacing(10);
    empty_cues_actions->addStretch(1);
    auto* empty_roles_btn = new QPushButton(QStringLiteral("Rôles…"), empty_cues_page);
    auto* empty_add_line_btn = new QPushButton(QStringLiteral("Ajouter une réplique"), empty_cues_page);
    empty_add_line_btn->setObjectName(QStringLiteral("primaryButton"));
    empty_cues_actions->addWidget(empty_roles_btn);
    empty_cues_actions->addWidget(empty_add_line_btn);
    empty_cues_actions->addStretch(1);
    empty_cues_layout->addWidget(lines_empty_title_);
    empty_cues_layout->addWidget(lines_empty_subtitle_);
    empty_cues_layout->addSpacing(8);
    empty_cues_layout->addLayout(empty_cues_actions);
    empty_cues_layout->addStretch(1);
    table_stack_->addWidget(empty_cues_page);

    lines_table_ = new QTableWidget(0, 5, table_stack_);
    lines_table_->setHorizontalHeaderLabels({
        QStringLiteral("Entrée"), QStringLiteral("Sortie"), QStringLiteral("Rôle"),
        QStringLiteral("Piste"), QStringLiteral("Texte")});
    lines_table_->setItemDelegate(new LinesTableDelegate([this] { return document_.get(); }, lines_table_));
    lines_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lines_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    lines_table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    lines_table_->setTextElideMode(Qt::ElideRight);
    lines_table_->setAlternatingRowColors(true);
    lines_table_->setShowGrid(false);
    lines_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    lines_table_->verticalHeader()->setDefaultSectionSize(38);
    lines_table_->verticalHeader()->setVisible(false);
    lines_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    lines_table_->setColumnWidth(0, 118);
    lines_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    lines_table_->setColumnWidth(1, 118);
    lines_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    lines_table_->setColumnWidth(2, 152);
    lines_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    lines_table_->setColumnWidth(3, 64);
    lines_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table_stack_->addWidget(lines_table_);
    cue_pane_layout->addWidget(table_stack_, 1);

    // 2B. Right Pane: Video Monitor + Single-Row Transport Bar
    auto* monitor_pane = new QWidget(upper_splitter);
    monitor_pane->setMinimumWidth(460);
    auto* monitor_layout = new QVBoxLayout(monitor_pane);
    monitor_layout->setContentsMargins(0, 0, 0, 0);
    monitor_layout->setSpacing(0);

    empty_video_page_ = new QWidget(monitor_pane);
    empty_video_page_->setMinimumHeight(220);
    empty_video_page_->setStyleSheet(QStringLiteral("background: #0A0A0C;"));
    auto* empty_video_layout = new QVBoxLayout(empty_video_page_);
    empty_video_layout->setContentsMargins(36, 28, 36, 28);
    empty_video_layout->setSpacing(10);
    empty_video_layout->addStretch(1);
    auto* empty_video_icon = new QLabel(empty_video_page_);
    empty_video_icon->setAlignment(Qt::AlignCenter);
    empty_video_icon->setPixmap(makeStudioIcon(StudioIcon::Film, QColor(QStringLiteral("#5C5C66")), 32).pixmap(32, 32));
    auto* empty_video_title = new QLabel(QStringLiteral("Aucune vidéo associée"), empty_video_page_);
    empty_video_title->setAlignment(Qt::AlignCenter);
    empty_video_title->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: 600; color: #E4E4E7;"));
    auto* empty_video_sub = new QLabel(
        QStringLiteral("Glissez-déposez un fichier média ou sélectionnez la vidéo du projet."),
        empty_video_page_);
    empty_video_sub->setAlignment(Qt::AlignCenter);
    empty_video_sub->setWordWrap(true);
    empty_video_sub->setStyleSheet(QStringLiteral("color: #82828C; font-size: 10pt;"));
    auto* empty_pick_video_btn = new QPushButton(QStringLiteral("Ouvrir une vidéo…"), empty_video_page_);
    empty_video_layout->addWidget(empty_video_icon);
    empty_video_layout->addWidget(empty_video_title);
    empty_video_layout->addWidget(empty_video_sub);
    empty_video_layout->addSpacing(6);
    empty_video_layout->addWidget(empty_pick_video_btn, 0, Qt::AlignHCenter);
    empty_video_layout->addStretch(1);
    monitor_layout->addWidget(empty_video_page_, 1);

    // Direct QVideoWidget host (not inside a nested QStackedWidget, preserving native GPU surfaces on Win/Linux/macOS)
    video_host_ = new QWidget(monitor_pane);
    video_host_->setMinimumHeight(220);
    video_host_->setStyleSheet(QStringLiteral("background: #000000;"));
    video_host_layout_ = new QVBoxLayout(video_host_);
    video_host_layout_->setContentsMargins(0, 0, 0, 0);
    video_host_layout_->setSpacing(0);
    video_widget_ = new QVideoWidget(video_host_);
    video_widget_->setMinimumHeight(200);
    video_widget_->setAspectRatioMode(Qt::KeepAspectRatio);
    player_->setVideoOutput(video_widget_);
    if (QVideoSink* sink = video_widget_->videoSink()) {
        connect(sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
            if (!frame.isValid()) return;
            if (!decode_fallback_running_ && !player_->source().isEmpty()) {
                ++received_video_frames_;
                if (video_decode_watchdog_timer_ && video_decode_watchdog_timer_->isActive()) {
                    video_decode_watchdog_timer_->stop();
                }
            }
            // Frame-acked non-cancelling seek handshake: as soon as the GPU/CPU finishes decoding
            // the in-flight seek frame, immediately dispatch the latest pending trackpad seek!
            media_seek_in_flight_ = false;
            if (pending_media_seek_ms_ >= 0 && !flush_seek_scheduled_) {
                flush_seek_scheduled_ = true;
                QTimer::singleShot(0, this, [this] {
                    flush_seek_scheduled_ = false;
                    flushPendingMediaSeek();
                });
            }
        });
    }
    video_host_layout_->addWidget(video_widget_, 1);
    video_host_->hide();
    monitor_layout->addWidget(video_host_, 1);

    // Single-Row Transport Bar
    auto* transport_bar = new QFrame(monitor_pane);
    transport_bar->setObjectName(QStringLiteral("transportBar"));
    transport_bar->setFixedHeight(46);
    auto* controls_row = new QHBoxLayout(transport_bar);
    controls_row->setContentsMargins(12, 0, 12, 0);
    controls_row->setSpacing(8);

    auto* prev_frame_btn = new QPushButton(transport_bar);
    prev_frame_btn->setObjectName(QStringLiteral("transportIconButton"));
    prev_frame_btn->setIcon(makeStudioIcon(StudioIcon::StepBack, QColor(QStringLiteral("#D4D4D8")), 15));
    prev_frame_btn->setFixedSize(30, 30);
    prev_frame_btn->setToolTip(QStringLiteral("Image précédente (Alt+←)"));

    play_button_ = new QPushButton(transport_bar);
    play_button_->setObjectName(QStringLiteral("transportPlayButton"));
    play_button_->setIcon(makeStudioIcon(StudioIcon::Play, QColor(QStringLiteral("#111113")), 15));
    play_button_->setFixedSize(34, 34);
    play_button_->setToolTip(QStringLiteral("Lecture / Pause (Espace)"));

    auto* next_frame_btn = new QPushButton(transport_bar);
    next_frame_btn->setObjectName(QStringLiteral("transportIconButton"));
    next_frame_btn->setIcon(makeStudioIcon(StudioIcon::StepForward, QColor(QStringLiteral("#D4D4D8")), 15));
    next_frame_btn->setFixedSize(30, 30);
    next_frame_btn->setToolTip(QStringLiteral("Image suivante (Alt+→)"));

    position_slider_ = new QSlider(Qt::Horizontal, transport_bar);
    position_slider_->setRange(0, 1000);

    timecode_label_ = new QLabel(QStringLiteral("00:00:00:00"), transport_bar);
    timecode_label_->setObjectName(QStringLiteral("timecodeReadout"));
    timecode_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timecode_label_->setMinimumWidth(104);

    mute_button_ = new QPushButton(transport_bar);
    mute_button_->setObjectName(QStringLiteral("transportIconButton"));
    mute_button_->setIcon(makeStudioIcon(StudioIcon::Volume, QColor(QStringLiteral("#A8A8B2")), 16));
    mute_button_->setFixedSize(30, 30);
    mute_button_->setToolTip(QStringLiteral("Couper / rétablir le son"));

    volume_slider_ = new QSlider(Qt::Horizontal, transport_bar);
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(saved_volume);
    volume_slider_->setFixedWidth(64);
    volume_slider_->setToolTip(QStringLiteral("Volume (%1 %)").arg(saved_volume));

    video_source_button_ = new QPushButton(QStringLiteral("Vidéo…"), transport_bar);
    video_source_button_->setObjectName(QStringLiteral("ghostButton"));
    video_source_button_->setFixedHeight(30);
    video_source_button_->setStyleSheet(QStringLiteral("padding: 3px 10px; font-size: 9.5pt;"));
    video_source_button_->setToolTip(QStringLiteral("Ouvrir ou remplacer la vidéo source"));

    controls_row->addWidget(prev_frame_btn, 0, Qt::AlignVCenter);
    controls_row->addWidget(play_button_, 0, Qt::AlignVCenter);
    controls_row->addWidget(next_frame_btn, 0, Qt::AlignVCenter);
    controls_row->addSpacing(4);
    controls_row->addWidget(position_slider_, 1, Qt::AlignVCenter);
    controls_row->addSpacing(4);
    controls_row->addWidget(timecode_label_, 0, Qt::AlignVCenter);
    controls_row->addWidget(make_divider(transport_bar), 0, Qt::AlignVCenter);
    controls_row->addWidget(mute_button_, 0, Qt::AlignVCenter);
    controls_row->addWidget(volume_slider_, 0, Qt::AlignVCenter);
    controls_row->addWidget(video_source_button_, 0, Qt::AlignVCenter);
    monitor_layout->addWidget(transport_bar);

    upper_splitter->addWidget(cue_pane);
    upper_splitter->addWidget(monitor_pane);
    upper_splitter->setStretchFactor(0, 11);
    upper_splitter->setStretchFactor(1, 12);
    upper_splitter->setSizes({700, 800});

    // 2C. Lower Full-Width Pane: Bande Rythmo
    auto* timeline_pane = new QWidget(vertical_splitter);
    timeline_pane->setMinimumHeight(260);
    auto* timeline_pane_layout = new QVBoxLayout(timeline_pane);
    timeline_pane_layout->setContentsMargins(0, 0, 0, 0);
    timeline_pane_layout->setSpacing(0);

    auto* timeline_strip = new QFrame(timeline_pane);
    timeline_strip->setObjectName(QStringLiteral("timelineStrip"));
    timeline_strip->setFixedHeight(42);
    auto* strip_layout = new QHBoxLayout(timeline_strip);
    strip_layout->setContentsMargins(14, 0, 12, 0);
    strip_layout->setSpacing(6);

    for (int t = 0; t < 4; ++t) {
        const QString label = t == 0 ? QStringLiteral("Piste 1") : QString::number(t + 1);
        auto* pill = new QPushButton(label, timeline_strip);
        pill->setObjectName(QStringLiteral("trackPill"));
        pill->setCheckable(true);
        pill->setChecked(t == 0);
        pill->setToolTip(QStringLiteral("Activer la Piste %1 pour l’insertion").arg(t + 1));
        track_buttons_[static_cast<std::size_t>(t)] = pill;
        strip_layout->addWidget(pill);
        connect(pill, &QPushButton::clicked, this, [this, t] {
            selected_timeline_track_ = t;
            updateTrackPills();
            if (timeline_) timeline_->setSelection(timeline_->selectedLine(), selected_timeline_track_);
            status_label_->setText(QStringLiteral("Piste %1 active")
                .arg(selected_timeline_track_ + 1));
        });
    }

    strip_layout->addSpacing(6);
    strip_layout->addWidget(make_divider(timeline_strip));
    strip_layout->addSpacing(6);

    auto* strip_split = new QPushButton(QStringLiteral("Scinder"), timeline_strip);
    strip_split->setObjectName(QStringLiteral("ghostButton"));
    strip_split->setToolTip(QStringLiteral("Scinder la réplique sélectionnée (%1)")
        .arg(QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText)));

    auto* strip_shot = new QPushButton(QStringLiteral("Plan"), timeline_strip);
    strip_shot->setObjectName(QStringLiteral("ghostButton"));
    strip_shot->setToolTip(QStringLiteral("Marquer un changement de plan au curseur (%1)")
        .arg(QKeySequence(Qt::CTRL | Qt::Key_M).toString(QKeySequence::NativeText)));

    auto* delete_line_button = new QPushButton(QStringLiteral("Supprimer"), timeline_strip);
    delete_line_button->setObjectName(QStringLiteral("dangerGhostButton"));
    delete_line_button->setToolTip(QStringLiteral("Supprimer la réplique sélectionnée (Suppr)"));

    auto* strip_font = new QPushButton(QStringLiteral("Police…"), timeline_strip);
    strip_font->setObjectName(QStringLiteral("ghostButton"));
    strip_font->setToolTip(QStringLiteral("Choisir la police de la bande rythmo"));

    auto* zoom_out_btn = new QPushButton(timeline_strip);
    zoom_out_btn->setObjectName(QStringLiteral("transportIconButton"));
    zoom_out_btn->setIcon(makeStudioIcon(StudioIcon::ZoomOut, QColor(QStringLiteral("#A8A8B2")), 15));
    zoom_out_btn->setFixedSize(28, 28);
    zoom_out_btn->setToolTip(QStringLiteral("Dézoomer (−)"));

    zoom_reset_button_ = new QPushButton(QStringLiteral("8,0 s"), timeline_strip);
    zoom_reset_button_->setObjectName(QStringLiteral("ghostButton"));
    zoom_reset_button_->setFixedHeight(28);
    zoom_reset_button_->setStyleSheet(QStringLiteral(
        "QPushButton#ghostButton { color: #92929C; font-size: 9.5pt; padding: 2px 9px; }"
        "QPushButton#ghostButton:hover { color: #EDEDEF; }"));
    zoom_reset_button_->setToolTip(QStringLiteral("Fenêtre temporelle visible · Cliquer pour réinitialiser à 8,0 s"));

    auto* zoom_in_btn = new QPushButton(timeline_strip);
    zoom_in_btn->setObjectName(QStringLiteral("transportIconButton"));
    zoom_in_btn->setIcon(makeStudioIcon(StudioIcon::ZoomIn, QColor(QStringLiteral("#A8A8B2")), 15));
    zoom_in_btn->setFixedSize(28, 28);
    zoom_in_btn->setToolTip(QStringLiteral("Zoomer (+)"));

    strip_layout->addWidget(strip_split);
    strip_layout->addWidget(strip_shot);
    strip_layout->addWidget(delete_line_button);
    strip_layout->addSpacing(2);
    strip_layout->addWidget(make_divider(timeline_strip));
    strip_layout->addSpacing(2);
    strip_layout->addWidget(strip_font);
    strip_layout->addStretch(1);
    strip_layout->addWidget(zoom_out_btn);
    strip_layout->addWidget(zoom_reset_button_);
    strip_layout->addWidget(zoom_in_btn);
    timeline_pane_layout->addWidget(timeline_strip);

    timeline_host_ = new QWidget(timeline_pane);
    timeline_host_layout_ = new QVBoxLayout(timeline_host_);
    timeline_host_layout_->setContentsMargins(0, 0, 0, 0);
    timeline_host_layout_->setSpacing(0);

    timeline_ = new RhythmTimelineWidget(timeline_host_);
    {
        imported_timeline_font_path_ = app_settings.value(QStringLiteral("timelineFontFile")).toString();
        if (!imported_timeline_font_path_.isEmpty() && QFileInfo::exists(imported_timeline_font_path_))
            QFontDatabase::addApplicationFont(imported_timeline_font_path_);
        const QString requested_family = app_settings.value(
            QStringLiteral("timelineFontFamily"), qApp->font().family()).toString();
        QFont band_font(QFontDatabase::hasFamily(requested_family) ? requested_family : qApp->font().family());
        band_font.setBold(true);
        timeline_->setTextFont(band_font);
    }
    timeline_host_layout_->addWidget(timeline_, 1);
    timeline_pane_layout->addWidget(timeline_host_, 1);

    vertical_splitter->addWidget(upper_splitter);
    vertical_splitter->addWidget(timeline_pane);
    vertical_splitter->setStretchFactor(0, 5);
    vertical_splitter->setStretchFactor(1, 5);
    vertical_splitter->setSizes({460, 400});

    root_layout->addWidget(vertical_splitter, 1);

    editor_widget_ = root;
    page_stack_ = new QStackedWidget(this);
    page_stack_->addWidget(root);
    setCentralWidget(page_stack_);

    // Background optional update check
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
                QStringLiteral("Annuler"), 0, 1000, this);
            progress->setFont(qApp->font());
            progress->setWindowIcon(windowIcon());
            progress->setStyleSheet(studioStyleSheet());
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
                    const QString updater_log_path = QDir(update_dir).filePath(
                        QStringLiteral("apply-update-%1.log").arg(tag));
                    bool updater_started = false;
#ifdef Q_OS_WIN
                    auto quote = [](QString value) { value.replace(QStringLiteral("'"), QStringLiteral("''")); return QStringLiteral("'") + value + QStringLiteral("'"); };
                    const QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Rythmo.exe"));
                    const QString script = QStringLiteral(R"PS($ErrorActionPreference = 'Stop'
$updateLog = %5
$pidToWait = %1
$root = %2
$package = %3
$exe = %4
$expectedVersion = %6
$parent = Split-Path -Parent $root
Set-Location -LiteralPath $parent
[Environment]::CurrentDirectory = $parent
"[$(Get-Date -Format o)] Waiting for Rythmo process $pidToWait to exit." | Add-Content -LiteralPath $updateLog
while (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }
$transaction = Join-Path $parent ('.Rythmo-update-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $transaction 'payload'
$backup = Join-Path $transaction 'previous'
$newProcess = $null
New-Item -ItemType Directory -Force -Path $transaction | Out-Null
try {
  Expand-Archive -LiteralPath $package -DestinationPath $payload -Force
  $candidate = Join-Path $payload 'Rythmo.exe'
  if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw 'Rythmo.exe is missing from the update package.' }
  Push-Location $payload
  try {
    $versionOutput = (& $candidate --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch [regex]::Escape($expectedVersion)) {
      throw "Package version check failed. Expected $expectedVersion; received: $versionOutput"
    }
  } finally { Pop-Location }
  if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Installed application folder was not found: $root" }
  "[$(Get-Date -Format o)] Replacing application folder: $root" | Add-Content -LiteralPath $updateLog
  $moveDirectory = { param($source, $destination)
    $lastError = $null
    for ($attempt = 1; $attempt -le 20; $attempt++) {
      try { Move-Item -LiteralPath $source -Destination $destination -ErrorAction Stop; return }
      catch {
        $lastError = $_
        if ($attempt -lt 20) { Start-Sleep -Milliseconds 500 }
      }
    }
    throw $lastError
  }
  & $moveDirectory $root $backup
  & $moveDirectory $payload $root
  "[$(Get-Date -Format o)] Replacement complete; starting $exe" | Add-Content -LiteralPath $updateLog
  $newProcess = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 500
    $newProcess.Refresh()
    if ($newProcess.HasExited) { throw "Updated Rythmo exited during startup (code $($newProcess.ExitCode))." }
  } while ([DateTime]::UtcNow -lt $deadline)
  $running = Get-CimInstance Win32_Process -Filter "ProcessId = $($newProcess.Id)"
  if (-not $running -or $running.ExecutablePath -ine $exe) { throw 'The updated executable could not be verified after restart.' }
  "Successfully installed Rythmo $expectedVersion and verified process $($newProcess.Id)." | Add-Content -LiteralPath $updateLog
  Remove-Item -LiteralPath $backup -Recurse -Force
} catch {
  ($_ | Out-String) | Add-Content -LiteralPath $updateLog -ErrorAction SilentlyContinue
  if ($newProcess) {
    $newProcess.Refresh()
    if (-not $newProcess.HasExited) { Stop-Process -Id $newProcess.Id -Force -ErrorAction SilentlyContinue }
  }
  if (Test-Path -LiteralPath $backup -PathType Container) {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    Move-Item -LiteralPath $backup -Destination $root
  }
  if (Test-Path -LiteralPath (Join-Path $root 'Rythmo.exe')) {
    Start-Process -FilePath $exe -WorkingDirectory $root -ErrorAction SilentlyContinue
  }
} finally {
  Remove-Item -LiteralPath $transaction -Recurse -Force -ErrorAction SilentlyContinue
}
)PS")
                        .arg(process_id)
                        .arg(quote(QCoreApplication::applicationDirPath()), quote(package_path), quote(executable),
                             quote(updater_log_path), quote(available.toString()));
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
                    updater_started = QProcess::startDetached(QStringLiteral("powershell.exe"),
                        {QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                         QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                         QStringLiteral("-File"), script_path},
                        QFileInfo(QCoreApplication::applicationDirPath()).absolutePath());
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
                    const QString authorization_marker = script_path + QStringLiteral(".authorized");
                    const QString apple_script_path = script_path + QStringLiteral(".applescript");
                    const QFileInfo application_parent_info(QFileInfo(bundle).absolutePath());
                    const bool needs_admin = !application_parent_info.isWritable();
                    auto shell_quote = [](QString value) {
                        value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
                        return QStringLiteral("'") + value + QStringLiteral("'");
                    };
                    auto apple_script_quote = [](QString value) {
                        value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
                        value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
                        return QStringLiteral("\"") + value + QStringLiteral("\"");
                    };
                    QFile::remove(authorization_marker);
                    const QString script = QStringLiteral(
                        "#!/bin/sh\n"
                        "set -eu\n"
                        "pid=\"$1\"; package=\"$2\"; old_app=\"$3\"; update_dir=\"$4\"; expected_version=\"$5\"; login_uid=\"$6\"; authorized_marker=\"$7\"; log=\"$8\"\n"
                        "/bin/mkdir -p \"$update_dir\"\n"
                        "exec >>\"$log\" 2>&1\n"
                        "backup=\"\"; transaction_dir=\"\"\n"
                        "launch_app() {\n"
                        "  if [ \"$(/usr/bin/id -u)\" -eq 0 ]; then\n"
                        "    /bin/launchctl asuser \"$login_uid\" /usr/bin/sudo -u \"#$login_uid\" -H /usr/bin/open -a \"$old_app\"\n"
                        "  else\n"
                        "    /usr/bin/open -a \"$old_app\"\n"
                        "  fi\n"
                        "}\n"
                        "on_exit() {\n"
                        "  result=$?\n"
                        "  if [ \"$result\" -ne 0 ]; then\n"
                        "    echo \"Update failed with status $result at $(/bin/date)\"\n"
                        "    if [ -n \"$backup\" ] && [ -d \"$backup\" ]; then\n"
                        "      if [ -e \"$old_app\" ]; then /bin/rm -rf \"$old_app\"; fi\n"
                        "      /bin/mv \"$backup\" \"$old_app\" || true\n"
                        "    fi\n"
                        "    if [ -d \"$old_app\" ]; then launch_app || true; fi\n"
                        "    if [ -n \"$transaction_dir\" ]; then echo \"Recovery files retained at $transaction_dir\"; fi\n"
                        "  fi\n"
                        "  /bin/rm -f \"$authorized_marker\" || true\n"
                        "  exit \"$result\"\n"
                        "}\n"
                        "trap on_exit EXIT\n"
                        ": > \"$authorized_marker\"\n"
                        "case \"$login_uid\" in ''|*[!0-9]*) echo \"Invalid login user id\"; exit 1;; esac\n"
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
                        "launch_app\n"
                        "new_pid=\"\"\n"
                        "attempt=0\n"
                        "stable=0\n"
                        "while [ \"$attempt\" -lt 40 ] && [ \"$stable\" -lt 3 ]; do\n"
                        "  new_pid=$(/usr/bin/pgrep -x Rythmo | /usr/bin/head -n 1 || true)\n"
                        "  if [ -n \"$new_pid\" ] && /bin/kill -0 \"$new_pid\" 2>/dev/null; then stable=$((stable + 1)); else stable=0; fi\n"
                        "  [ \"$stable\" -ge 3 ] || { /bin/sleep 0.5; attempt=$((attempt + 1)); }\n"
                        "done\n"
                        "[ \"$stable\" -ge 3 ] || { echo \"Updated app did not remain running\"; exit 1; }\n"
                        "/bin/rm -rf \"$backup\"\n"
                        "backup=\"\"\n"
                        "/bin/rm -rf \"$transaction_dir\"\n"
                        "transaction_dir=\"\"\n"
                        "trap - EXIT\n"
                        "/bin/rm -f \"$authorized_marker\"\n"
                    );
                    QFile script_file(script_path);
                    if (!script_file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        || script_file.write(script.toUtf8()) != script.toUtf8().size()
                        || !script_file.flush()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"),
                            QStringLiteral("Le programme d’installation différé n’a pas pu être préparé."));
                        return;
                    }
                    script_file.close();
                    const QString command = QStringLiteral("/usr/bin/nohup /bin/sh %1 %2 %3 %4 %5 %6 %7 %8 %9 </dev/null >/dev/null 2>&1 &")
                        .arg(shell_quote(script_path), shell_quote(QString::number(process_id)),
                             shell_quote(package_path), shell_quote(bundle), shell_quote(update_dir),
                             shell_quote(available.toString()),
                             shell_quote(QString::number(static_cast<qulonglong>(QFileInfo(QDir::homePath()).ownerId()))),
                             shell_quote(authorization_marker), shell_quote(updater_log_path));
                    const QString admin_suffix = needs_admin ? QStringLiteral(" with administrator privileges") : QString{};
                    const QString apple_script = QStringLiteral(
                        "try\n"
                        "  do shell script %1%2\n"
                        "on error errorMessage number errorNumber\n"
                        "  error errorMessage number errorNumber\n"
                        "end try")
                        .arg(apple_script_quote(command), admin_suffix);
                    progress->setLabelText(QStringLiteral("En attente de l’autorisation macOS pour installer Rythmo %1…").arg(tag));
                    QSaveFile apple_script_file(apple_script_path);
                    if (!apple_script_file.open(QIODevice::WriteOnly)
                        || apple_script_file.write(apple_script.toUtf8()) != apple_script.toUtf8().size()
                        || !apple_script_file.commit()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"),
                            QStringLiteral("Le script d’autorisation macOS ne peut pas être préparé."));
                        return;
                    }
                    auto* authorization_process = new QProcess(this);
                    authorization_process->setProgram(QStringLiteral("/usr/bin/osascript"));
                    authorization_process->setArguments({apple_script_path});
                    authorization_process->setProcessChannelMode(QProcess::MergedChannels);
                    authorization_process->start();
                    updater_started = authorization_process->waitForStarted(3000);
                    if (updater_started) {
                        auto handled = std::make_shared<bool>(false);
                        auto* authorization_watch = new QTimer(progress);
                        authorization_watch->setInterval(250);
                        connect(authorization_watch, &QTimer::timeout, this,
                            [this, authorization_process, authorization_watch, authorization_marker,
                             restore_main_window, progress, tag, handled] {
                                if (*handled) return;
                                if (QFileInfo::exists(authorization_marker)
                                    && authorization_process->state() == QProcess::NotRunning) {
                                    *handled = true;
                                    authorization_watch->stop();
                                    authorization_process->deleteLater();
                                    QFile::remove(authorization_marker);
                                    progress->setLabelText(QStringLiteral("Autorisation reçue. Installation et redémarrage de Rythmo %1…").arg(tag));
                                    QTimer::singleShot(700, progress, [progress] {
                                        progress->close();
                                        QCoreApplication::quit();
                                    });
                                    return;
                                }
                                if (authorization_process->state() == QProcess::NotRunning
                                    && authorization_process->exitCode() != 0
                                    && !QFileInfo::exists(authorization_marker)) {
                                    *handled = true;
                                    authorization_watch->stop();
                                    const QString detail = QString::fromLocal8Bit(authorization_process->readAll()).trimmed();
                                    authorization_process->deleteLater();
                                    restore_main_window();
                                    QMessageBox::warning(this, QStringLiteral("Mise à jour annulée"),
                                        detail.isEmpty()
                                            ? QStringLiteral("L’autorisation macOS a été refusée ou annulée. Rythmo n’a pas été modifié.")
                                            : QStringLiteral("L’autorisation macOS n’a pas abouti. Rythmo n’a pas été modifié.\n\n%1").arg(detail.right(1200)));
                                }
                            });
                        authorization_watch->start();
                        QTimer::singleShot(180000, progress, [this, authorization_process, authorization_watch,
                                                              authorization_marker, restore_main_window, handled] {
                            if (*handled || QFileInfo::exists(authorization_marker)) return;
                            *handled = true;
                            authorization_watch->stop();
                            if (authorization_process->state() != QProcess::NotRunning)
                                authorization_process->terminate();
                            authorization_process->deleteLater();
                            restore_main_window();
                            QMessageBox::warning(this, QStringLiteral("Mise à jour expirée"),
                                QStringLiteral("L’autorisation n’a pas été confirmée à temps. Rythmo n’a pas été modifié."));
                        });
                        return;
                    }
                    authorization_process->deleteLater();
#else
                    auto shell_quote = [](QString value) { value.replace(QStringLiteral("'"), QStringLiteral("'\\''")); return QStringLiteral("'") + value + QStringLiteral("'"); };
                    const QString script = QStringLiteral("#!/bin/sh\nset -eu\npid=%1\nroot=%2\npackage=%3\nupdate_dir=%4\nlog=%5\n/bin/mkdir -p \"$update_dir\"\nexec >>\"$log\" 2>&1\nparent=$(/usr/bin/dirname \"$root\")\ntransaction=\"\"\nbackup=\"\"\nrollback() { code=$?; trap - EXIT; if [ \"$code\" -ne 0 ]; then echo \"Update failed with status $code at $(/bin/date)\"; if [ -n \"$backup\" ] && [ -d \"$backup\" ]; then /bin/rm -rf \"$root\"; /bin/mv \"$backup\" \"$root\"; fi; if [ -x \"$root/Rythmo\" ]; then \"$root/Rythmo\" >/dev/null 2>&1 & fi; if [ -n \"$transaction\" ]; then echo \"Recovery files retained at $transaction\"; fi; fi; exit \"$code\"; }\ntrap rollback EXIT\nwhile /bin/kill -0 \"$pid\" 2>/dev/null; do /bin/sleep 0.5; done\ntransaction=$(/usr/bin/mktemp -d \"$parent/.Rythmo-update.XXXXXX\")\ntmp=\"$transaction/extracted\"\nbackup=\"$transaction/previous\"\n/bin/mkdir -p \"$tmp\"\n/usr/bin/tar -xzf \"$package\" -C \"$tmp\"\nnew_root=$(/usr/bin/find \"$tmp\" -mindepth 1 -maxdepth 1 -type d -name 'Rythmo-linux-*' -print -quit)\n[ -n \"$new_root\" ] && [ -x \"$new_root/Rythmo\" ] || { echo \"Portable package is invalid\"; exit 1; }\n/bin/mv \"$root\" \"$backup\"\n/bin/mv \"$new_root\" \"$root\"\n\"$root/Rythmo\" >/dev/null 2>&1 &\nnew_pid=$!\n/bin/sleep 2\n/bin/kill -0 \"$new_pid\" >/dev/null 2>&1 || { echo \"Updated Rythmo did not start\"; exit 1; }\n/bin/rm -rf \"$backup\" \"$transaction\"\ntrap - EXIT\n")
                        .arg(process_id).arg(shell_quote(QFileInfo(running_app_path).absolutePath()),
                                             shell_quote(package_path), shell_quote(update_dir),
                                             shell_quote(updater_log_path));
                    QFile script_file(script_path);
                    if (!script_file.open(QIODevice::WriteOnly | QIODevice::Truncate) || script_file.write(script.toUtf8()) != script.toUtf8().size()) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"), QStringLiteral("Le programme d’installation différé n’a pas pu être préparé."));
                        return;
                    }
                    script_file.close();
                    updater_started = QProcess::startDetached(QStringLiteral("/bin/sh"), {script_path});
#endif
                    if (!updater_started) {
                        restore_main_window();
                        QMessageBox::warning(this, QStringLiteral("Mise à jour impossible"),
                            QStringLiteral("Le programme d’installation ne peut pas démarrer. Le journal détaillé est conservé dans :\n%1")
                                .arg(updater_log_path));
                        return;
                    }
                    progress->setLabelText(QStringLiteral("Rythmo %1 est prêt. Fermeture puis redémarrage…").arg(tag));
                    QTimer::singleShot(1200, progress, [progress] {
                        progress->close();
                        QCoreApplication::quit();
                    });
                });
        });
    });

    auto* space_play = new QShortcut(QKeySequence(Qt::Key_Space), root);
    space_play->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space_play, &QShortcut::activated, this, [this] {
        if (timeline_ && timeline_->isEditingText()) return;
        togglePlayPause();
    });

    status_label_ = new QLabel(QStringLiteral("Prêt"), this);
    status_label_->setStyleSheet(QStringLiteral("color: #8A8A94; font-size: 9.5pt; padding-left: 8px;"));
    statusBar()->setSizeGripEnabled(true);
    statusBar()->addWidget(status_label_, 1);

    // Timeline callbacks
    timeline_->setSelectionHandler([this](int line_index, int track) {
        selected_timeline_track_ = std::clamp(track, 0, 3);
        updateTrackPills();
        if (line_index >= 0 && line_index < lines_table_->rowCount()) {
            if (lines_table_->isRowHidden(line_index) && lines_search_edit_) {
                lines_search_edit_->clear();
            }
            lines_table_->selectRow(line_index);
            lines_table_->setCurrentCell(line_index, 4);
            lines_table_->scrollToItem(lines_table_->item(line_index, 4));
        } else {
            lines_table_->clearSelection();
            lines_table_->setCurrentCell(-1, -1);
        }
        timeline_->setSelection(line_index, selected_timeline_track_);
        status_label_->setText(QStringLiteral("Piste %1 active")
                               .arg(selected_timeline_track_ + 1));
    });
    timeline_->setInsertAtHandler([this](int track, qint64 position_ms) {
        addLineAt(track, position_ms);
        timeline_->setFocus(Qt::OtherFocusReason);
    });
    timeline_->setEditHandler([this](int row, int cue) { editTimelineLine(row, cue); });
    timeline_->setTextEditHandler([this](int row, std::size_t first, std::size_t count, const QString& text) {
        if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
        try {
            auto& line = document_->lines()[static_cast<std::size_t>(row)];
            if (count > 0) document_->set_text_segments_text(line, first, count, utf8(text));
            else document_->set_line_text(line, utf8(text));
            setDirty(true);
            updateTableRow(row);
            timeline_->updateLineCache(row);
            if (row < lines_table_->rowCount()) lines_table_->selectRow(row);
            status_label_->setText(QStringLiteral("Réplique mise à jour."));
        } catch (const std::exception& error) {
            QMessageBox::warning(this, QStringLiteral("Modification impossible"), QString::fromUtf8(error.what()));
        }
    });
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
            if (boundary.kind.rfind("in_", 0) == 0 && lines_table_->item(row, 0))
                lines_table_->item(row, 0)->setText(qstr(adjusted.format()));
            if (boundary.kind.rfind("out_", 0) == 0 && lines_table_->item(row, 1))
                lines_table_->item(row, 1)->setText(qstr(adjusted.format()));
            timeline_->updateLineCache(row);
        } catch (const std::exception& error) {
            status_label_->setText(QStringLiteral("Redimensionnement impossible : ") + QString::fromUtf8(error.what()));
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
        }
        current_document_snapshot_ = after;
        undo_snapshot_initialized_ = true;
        dirty_ = (after != saved_document_snapshot_);
        const auto name = document_path_.isEmpty() ? QStringLiteral("Sans titre") : QFileInfo(document_path_).fileName();
        setWindowTitle((dirty_ ? QStringLiteral("* ") : QString{}) + name + QStringLiteral(" | Rythmo by Dubia Productions"));
        updateEditorHeader();
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
    timeline_->setZoomChangedHandler([this](qint64 visible_ms) {
        if (zoom_reset_button_) {
            zoom_reset_button_->setText(QStringLiteral("%1 s").arg(visible_ms / 1000.0, 0, 'f', 1).replace(QLatin1Char('.'), QLatin1Char(',')));
        }
    });

    // Top bar & pane connections
    connect(home_btn, &QPushButton::clicked, this, [this] { showHome(); });
    connect(fullscreen_btn, &QPushButton::clicked, this, [this] { toggleRhythmFullscreen(); });
    connect(export_btn, &QPushButton::clicked, this, [this] { exportVideoWithRhythm(); });

    connect(add_line_button, &QPushButton::clicked, this, [this] { addLine(); });
    connect(empty_add_line_btn, &QPushButton::clicked, this, [this] { addLine(); });
    connect(roles_button_, &QPushButton::clicked, this, [this] { manageRoles(); });
    connect(empty_roles_btn, &QPushButton::clicked, this, [this] { manageRoles(); });
    connect(strip_split, &QPushButton::clicked, this, [this] { splitSelectedLine(); });
    connect(strip_shot, &QPushButton::clicked, this, [this] { addShotMarker(); });
    connect(delete_line_button, &QPushButton::clicked, this, [this] { removeSelectedLine(); });
    connect(strip_font, &QPushButton::clicked, this, [this] { chooseTimelineFont(); });

    connect(zoom_in_btn, &QPushButton::clicked, this, [this] { if (timeline_) timeline_->zoomIn(); });
    connect(zoom_out_btn, &QPushButton::clicked, this, [this] { if (timeline_) timeline_->zoomOut(); });
    connect(zoom_reset_button_, &QPushButton::clicked, this, [this] { if (timeline_) timeline_->resetZoom(); });

    connect(lines_search_edit_, &QLineEdit::textChanged, this, [this](const QString&) { applyLinesFilter(); });

    connect(video_source_button_, &QPushButton::clicked, this, [this] { openVideo(); });
    connect(empty_pick_video_btn, &QPushButton::clicked, this, [this] { openVideo(); });
    connect(mute_button_, &QPushButton::clicked, this, [this] {
        const bool next_muted = !audio_output_->isMuted();
        audio_output_->setMuted(next_muted);
        mute_button_->setIcon(makeStudioIcon(
            next_muted ? StudioIcon::Mute : StudioIcon::Volume,
            next_muted ? QColor(QStringLiteral("#E5484D")) : QColor(QStringLiteral("#A1A1AA")), 14));
    });
    connect(volume_slider_, &QSlider::valueChanged, this, [this](int value) {
        audio_output_->setVolume(std::clamp(value, 0, 100) / 100.0f);
        if (value > 0 && audio_output_->isMuted()) {
            audio_output_->setMuted(false);
            mute_button_->setIcon(makeStudioIcon(StudioIcon::Volume, QColor(QStringLiteral("#A1A1AA")), 14));
        }
        volume_slider_->setToolTip(QStringLiteral("Volume (%1 %)").arg(value));
    });
    connect(volume_slider_, &QSlider::sliderReleased, this, [this] {
        QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
        settings.setValue(QStringLiteral("audioVolume"), volume_slider_->value());
    });

    connect(prev_frame_btn, &QPushButton::clicked, this, [this] { if (timeline_) timeline_->stepFrame(-1); });
    connect(next_frame_btn, &QPushButton::clicked, this, [this] { if (timeline_) timeline_->stepFrame(1); });
    connect(play_button_, &QPushButton::clicked, this, [this] {
        if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
        togglePlayPause();
    });
    connect(position_slider_, &QSlider::sliderMoved, this, [this](int value) {
        if (player_->duration() > 0) seekTimeline(qRound64(static_cast<double>(value) * player_->duration() / 1000.0));
    });

    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !video_path_.isEmpty()) {
            // Prime first decoded frame immediately so the video monitor is ready on launch & trackpad scrub!
            if (fallback_resume_pending_) {
                fallback_resume_pending_ = false;
                const qint64 resume_pos = fallback_resume_pos_ms_;
                const bool resume_play = fallback_resume_playing_;
                fallback_resume_pos_ms_ = 0;
                fallback_resume_playing_ = false;
                QTimer::singleShot(0, this, [this, resume_pos, resume_play] {
                    if (!player_ || video_path_.isEmpty()) return;
                    if (resume_play) player_->play();
                    else player_->pause();
                    const qint64 target_pos = std::max(resume_pos, current_position_ms_);
                    if (target_pos > 0 && (player_->duration() <= 0 || target_pos < player_->duration() - 40)) {
                        dispatchMediaSeek(target_pos, true);
                    }
                });
            } else {
                const qint64 target_pos = current_position_ms_;
                QTimer::singleShot(0, this, [this, target_pos] {
                    if (!player_ || video_path_.isEmpty()) return;
                    if (player_->playbackState() == QMediaPlayer::StoppedState) {
                        player_->pause();
                    }
                    if (target_pos > 0) {
                        dispatchMediaSeek(target_pos, true);
                    }
                });
                if (!decode_proxy_active_ && received_video_frames_ == 0 &&
                    !decode_fallback_attempted_ && video_decode_watchdog_timer_) {
                    video_decode_watchdog_timer_->start(340);
                }
            }
        }
    });

    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        const auto playback_state = player_->playbackState();
        if (playback_state == QMediaPlayer::PlayingState && position > 280 &&
            received_video_frames_ == 0 && !decode_proxy_active_ && !decode_fallback_attempted_) {
            const bool has_video_stream = player_->hasVideo() ||
                player_->metaData().value(QMediaMetaData::Resolution).isValid() ||
                media_fps_ > 0.0;
            if (has_video_stream) {
                fallback_resume_pos_ms_ = position;
                fallback_resume_playing_ = true;
                requestInstantPosterFrame(video_path_, position);
                startFfmpegDecodeFallback(QStringLiteral("Codec vidéo non décodé nativement"));
                return;
            }
        }
        // Only advance playhead from QMediaPlayer during active playback; while paused/scrubbing,
        // current_position_ms_ is authoritative so lagged decoder echoes never snap the trackpad!
        if (playback_state != QMediaPlayer::PlayingState) {
            return;
        }
        if (!playback_seek_pending_ && position < playback_anchor_position_ms_) {
            return;
        }
        if (pending_media_seek_ms_ < 0 && !media_seek_in_flight_) {
            playback_seek_pending_ = false;
        }
        playback_anchor_position_ms_ = position;
        playback_clock_.start();
        if (position >= current_position_ms_ || playback_seek_pending_) {
            updatePosition(position);
        }
    });
    connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64) {
        refreshTimeline();
    });
    connect(player_, &QMediaPlayer::metaDataChanged, this, [this] { updateMediaMetadata(); });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        const bool playing = state == QMediaPlayer::PlayingState;
        play_button_->setIcon(makeStudioIcon(
            playing ? StudioIcon::Pause : StudioIcon::Play, QColor(QStringLiteral("#111113")), 15));
        if (playing) {
            if (decode_fallback_running_ || decode_encoder_probe_process_) {
                fallback_resume_playing_ = true;
            }
            playback_seek_pending_ = false;
            media_seek_in_flight_ = false;
            const qint64 media_position = player_->position();
            playback_anchor_position_ms_ = current_position_ms_;
            if (std::abs(current_position_ms_ - media_position) > 25) {
                playback_seek_pending_ = true;
                last_dispatched_seek_ms_ = current_position_ms_;
                player_->setPosition(current_position_ms_);
            }
            playback_clock_.start();
            const QScreen* active_screen = timeline_->screen();
            const qreal refresh_rate = active_screen ? active_screen->refreshRate() : 0.0;
            // Cap visual timer between 8ms (120Hz) and 33ms (30Hz) so 144Hz/240Hz monitors never starve low-end CPUs
            const int interval_ms = std::isfinite(refresh_rate) && refresh_rate > 0.0
                ? qBound(8, qRound(1000.0 / refresh_rate), 33)
                : 16;
            playback_visual_timer_->setInterval(interval_ms);
            playback_visual_timer_->start();
        } else {
            playback_visual_timer_->stop();
            if (state == QMediaPlayer::PausedState) {
                const qint64 media_position = player_->position();
                if (std::abs(current_position_ms_ - media_position) > 25 && pending_media_seek_ms_ < 0 && !media_seek_in_flight_) {
                    dispatchMediaSeek(current_position_ms_, true);
                }
                playback_anchor_position_ms_ = current_position_ms_;
                updatePosition(current_position_ms_);
            } else {
                playback_seek_pending_ = false;
                media_seek_in_flight_ = false;
                if (player_->duration() > 0 && player_->position() >= player_->duration() - 50) {
                    updatePosition(player_->position());
                }
            }
        }
    });
    connect(playback_visual_timer_, &QTimer::timeout, this, [this] {
        const qint64 elapsed_ms = playback_clock_.isValid()
            ? qRound64(playback_clock_.elapsed() * player_->playbackRate()) : 0;
        qint64 position = playback_anchor_position_ms_ + elapsed_ms;
        if (player_->duration() > 0) position = std::min(position, player_->duration());
        position = std::max(position, current_position_ms_);
        if (position != current_position_ms_) updatePosition(position);
    });
    connect(player_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& error) {
                if (!decode_proxy_active_ && !decode_fallback_attempted_) {
                    fallback_resume_pos_ms_ = current_position_ms_;
                    fallback_resume_playing_ = (player_->playbackState() == QMediaPlayer::PlayingState);
                    startFfmpegDecodeFallback(error);
                } else if (decode_proxy_active_) {
                    status_label_->setText(QStringLiteral("Erreur de lecture de l’aperçu FFmpeg : ") + error);
                }
            });

    connect(lines_table_, &QTableWidget::cellChanged, this, [this](int row, int column) { onCellChanged(row, column); });
    connect(lines_table_, &QTableWidget::cellClicked, this, [this](int row, int column) {
        if (column == 2 || column == 3) {
            if (auto* item = lines_table_->item(row, column)) lines_table_->editItem(item);
        }
    });
    connect(lines_table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
                selected_timeline_track_ = std::clamp(document_->lines()[static_cast<std::size_t>(row)].track, 0, 3);
                updateTrackPills();
                timeline_->setSelection(row, selected_timeline_track_);
            });
    connect(lines_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (column == 4) {
            seekFromTableRow(row);
            editTimelineLine(row, -1);
        } else if (column != 2 && column != 3) {
            seekFromTableRow(row);
        }
    });
    connect(lines_table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = lines_table_->indexAt(pos);
        if (!index.isValid() || !document_) return;
        const int row = index.row();
        if (row < 0 || row >= static_cast<int>(document_->lines().size())) return;
        lines_table_->selectRow(row);
        QMenu menu(this);
        auto* jump = menu.addAction(QStringLiteral("Aller au timecode de la réplique"));
        auto* edit = menu.addAction(QStringLiteral("Modifier le texte…"));
        auto* split = menu.addAction(QStringLiteral("Scinder la réplique…"));
        menu.addSeparator();
        auto* del = menu.addAction(QStringLiteral("Supprimer la réplique"));
        connect(jump, &QAction::triggered, this, [this, row] { seekFromTableRow(row); });
        connect(edit, &QAction::triggered, this, [this, row] { editTimelineLine(row, -1); });
        connect(split, &QAction::triggered, this, [this] { splitSelectedLine(); });
        connect(del, &QAction::triggered, this, [this] { removeSelectedLine(); });
        menu.exec(lines_table_->viewport()->mapToGlobal(pos));
    });

    updateEditorHeader();
}

void MainWindow::createNewDocument(bool prompt_video) {
    if (!confirmDiscardChanges()) return;
    if (seek_coalesce_timer_) seek_coalesce_timer_->stop();
    if (video_decode_watchdog_timer_) video_decode_watchdog_timer_->stop();
    pending_media_seek_ms_ = -1;
    media_seek_in_flight_ = false;
    last_dispatched_seek_ms_ = -1;
    last_dispatched_seek_frame_ = -1;
    received_video_frames_ = 0;
    player_->stop();
    player_->setSource(QUrl{});
    document_ = std::make_unique<DetxDocument>(DetxDocument::create());
    resetUndoHistory(false);
    document_path_.clear();
    video_path_.clear();
    video_start_ = {};
    media_fps_ = 0.0;
    timecode_fps_ = 25;
    current_position_ms_ = 0;
    last_slider_permille_ = -1;
    last_displayed_timecode_ = Timecode{-1, -1, -1, -1};
    refreshTable();
    refreshTimeline();
    updatePosition(0);
    setDirty(true);
    showEditor();
    status_label_->setText(QStringLiteral("Nouveau projet DetX prêt (25 i/s)."));
    if (prompt_video) openVideo();
}

void MainWindow::updateTrackPills() {
    for (int t = 0; t < 4; ++t) {
        if (auto* btn = track_buttons_[static_cast<std::size_t>(t)]) {
            const QSignalBlocker blocker(btn);
            btn->setChecked(t == selected_timeline_track_);
        }
    }
}

void MainWindow::updateEditorHeader() {
    const QString doc_name = document_path_.isEmpty()
        ? QStringLiteral("Sans titre.detx")
        : QFileInfo(document_path_).fileName();
    if (document_title_label_) {
        document_title_label_->setText(dirty_ && document_
            ? doc_name + QStringLiteral(" (Modifié)")
            : doc_name);
        document_title_label_->setToolTip(document_path_.isEmpty()
            ? QStringLiteral("Projet non enregistré sur le disque")
            : document_path_);
    }

    const std::size_t role_count = document_ ? document_->roles().size() : 0;
    const std::size_t line_count = document_ ? document_->lines().size() : 0;
    const int fps = document_ ? document_->fps() : timecode_fps_;

    if (project_meta_label_) {
        if (media_fps_ > 0.0) {
            project_meta_label_->setText(QStringLiteral("%1 i/s")
                .arg(media_fps_, 0, 'f', std::abs(media_fps_ - std::round(media_fps_)) < 0.01 ? 0 : 2));
        } else {
            project_meta_label_->setText(QStringLiteral("%1 i/s").arg(fps));
        }
    }
    if (lines_count_badge_) {
        lines_count_badge_->setText(QString::number(static_cast<qulonglong>(line_count)));
    }
    if (roles_button_) {
        roles_button_->setText(role_count > 0
            ? QStringLiteral("Rôles (%1)").arg(static_cast<qulonglong>(role_count))
            : QStringLiteral("Rôles"));
    }

    if (!full_screen_active_) {
        const bool has_video = !video_path_.isEmpty();
        if (empty_video_page_) empty_video_page_->setVisible(!has_video);
        if (video_host_) {
            video_host_->setVisible(has_video);
            if (has_video && video_widget_ && !video_widget_->isVisible()) video_widget_->show();
        }
    }
    if (video_source_button_) {
        if (video_path_.isEmpty()) {
            video_source_button_->setText(QStringLiteral("Vidéo…"));
            video_source_button_->setToolTip(QStringLiteral("Ouvrir la vidéo source"));
        } else {
            const QString file_name = QFileInfo(video_path_).fileName();
            const QString elided = video_source_button_->fontMetrics().elidedText(file_name, Qt::ElideMiddle, 140);
            video_source_button_->setText(elided);
            video_source_button_->setToolTip(video_path_);
        }
    }
    if (table_stack_) {
        table_stack_->setCurrentIndex(line_count == 0 ? 0 : 1);
    }
}

void MainWindow::applyLinesFilter() {
    if (!lines_table_) return;
    const QString query = lines_search_edit_ ? lines_search_edit_->text().trimmed() : QString{};
    int visible_rows = 0;
    for (int row = 0; row < lines_table_->rowCount(); ++row) {
        if (query.isEmpty()) {
            lines_table_->setRowHidden(row, false);
            ++visible_rows;
            continue;
        }
        bool match = false;
        for (int col = 0; col < 5; ++col) {
            if (const auto* item = lines_table_->item(row, col)) {
                if (item->text().contains(query, Qt::CaseInsensitive)) { match = true; break; }
            }
        }
        lines_table_->setRowHidden(row, !match);
        if (match) ++visible_rows;
    }
    if (lines_count_badge_) {
        const int total = lines_table_->rowCount();
        lines_count_badge_->setText(query.isEmpty()
            ? QString::number(total)
            : QStringLiteral("%1/%2").arg(visible_rows).arg(total));
    }
}

void MainWindow::showHome() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    active_project_module_.clear();
    if (!home_widget_) {
        auto* page = new QWidget(this);
        page->setObjectName(QStringLiteral("homePage"));
        page->setStyleSheet(QStringLiteral(
            "QWidget#homePage { background: #121214; }"
            "QPushButton#recentRow { text-align: left; background: transparent; "
            "border: 1px solid transparent; border-radius: 7px; padding: 0px; }"
            "QPushButton#recentRow:hover { background: #1B1B20; border-color: #2A2A32; }"
            "QPushButton#recentRow:pressed { background: #17171B; }"));

        home_outer_layout_ = new QVBoxLayout(page);
        home_outer_layout_->setContentsMargins(48, 36, 48, 36);
        home_outer_layout_->addStretch(1);

        auto* center_row = new QHBoxLayout;
        center_row->addStretch(1);

        home_welcome_frame_ = new QWidget(page);
        home_welcome_frame_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        home_columns_layout_ = new QHBoxLayout(home_welcome_frame_);
        home_columns_layout_->setContentsMargins(0, 0, 0, 0);
        home_columns_layout_->setSpacing(44);

        // Left Column: Brand + Primary Actions
        home_left_col_ = new QWidget(home_welcome_frame_);
        auto* left_layout = new QVBoxLayout(home_left_col_);
        left_layout->setContentsMargins(0, 4, 0, 4);
        left_layout->setSpacing(10);

        auto* brand_row = new QHBoxLayout;
        brand_row->setSpacing(15);
        home_logo_label_ = new QLabel(home_left_col_);
        home_logo_label_->setPixmap(roundedLogoPixmap(48));
        home_logo_label_->setFixedSize(48, 48);

        auto* brand_text = new QVBoxLayout;
        brand_text->setSpacing(2);
        home_wordmark_label_ = new QLabel(QStringLiteral("Rythmo"), home_left_col_);
        const QString ver = QCoreApplication::applicationVersion();
        home_version_label_ = new QLabel(
            ver.isEmpty() ? QStringLiteral("Dubia Productions") : QStringLiteral("Version %1").arg(ver),
            home_left_col_);
        brand_text->addWidget(home_wordmark_label_);
        brand_text->addWidget(home_version_label_);
        brand_row->addWidget(home_logo_label_, 0, Qt::AlignVCenter);
        brand_row->addLayout(brand_text);
        brand_row->addStretch(1);
        left_layout->addLayout(brand_row);

        home_brand_spacer_ = new QSpacerItem(1, 26, QSizePolicy::Minimum, QSizePolicy::Fixed);
        left_layout->addSpacerItem(home_brand_spacer_);

        home_new_btn_ = new QPushButton(QStringLiteral("Nouvelle bande rythmo"), home_left_col_);
        home_new_btn_->setObjectName(QStringLiteral("primaryButton"));
        home_new_btn_->setCursor(Qt::PointingHandCursor);

        home_open_btn_ = new QPushButton(QStringLiteral("Ouvrir un projet…"), home_left_col_);
        home_open_btn_->setCursor(Qt::PointingHandCursor);

        home_voice_btn_ = new QPushButton(QStringLiteral("Séparation de voix…"), home_left_col_);
        home_voice_btn_->setCursor(Qt::PointingHandCursor);

        home_resume_button_ = new QPushButton(QStringLiteral("Reprendre le projet ouvert"), home_left_col_);
        home_resume_button_->setObjectName(QStringLiteral("ghostButton"));
        home_resume_button_->setCursor(Qt::PointingHandCursor);

        left_layout->addWidget(home_new_btn_);
        left_layout->addWidget(home_open_btn_);
        left_layout->addWidget(home_voice_btn_);
        left_layout->addSpacing(6);
        left_layout->addWidget(home_resume_button_);
        left_layout->addStretch(1);

        connect(home_new_btn_, &QPushButton::clicked, this, [this] { createNewDocument(true); });
        connect(home_open_btn_, &QPushButton::clicked, this, [this] { openDocument(); });
        connect(home_voice_btn_, &QPushButton::clicked, this, [this] { openVoiceSeparation(); });
        connect(home_resume_button_, &QPushButton::clicked, this, [this] { showEditor(); });

        // Vertical Hairline Divider
        auto* vline = new QFrame(home_welcome_frame_);
        vline->setObjectName(QStringLiteral("vDivider"));
        vline->setFixedWidth(1);

        // Right Column: Recent Projects
        auto* right_col = new QWidget(home_welcome_frame_);
        auto* right_layout = new QVBoxLayout(right_col);
        right_layout->setContentsMargins(0, 4, 0, 4);
        right_layout->setSpacing(14);

        auto* recent_header = new QHBoxLayout;
        home_recent_heading_ = new QLabel(QStringLiteral("Projets récents"), right_col);
        home_view_all_btn_ = new QPushButton(QStringLiteral("Tout afficher"), right_col);
        home_view_all_btn_->setObjectName(QStringLiteral("ghostButton"));
        home_view_all_btn_->setCursor(Qt::PointingHandCursor);
        connect(home_view_all_btn_, &QPushButton::clicked, this, [this] { showProjectPage(QStringLiteral("Bande Rythmo")); });
        recent_header->addWidget(home_recent_heading_);
        recent_header->addStretch(1);
        recent_header->addWidget(home_view_all_btn_);
        right_layout->addLayout(recent_header);

        home_recent_panel_ = new QWidget(right_col);
        home_recent_panel_->setObjectName(QStringLiteral("homeRecentPanel"));
        auto* recent_list_layout = new QVBoxLayout(home_recent_panel_);
        recent_list_layout->setContentsMargins(0, 0, 0, 0);
        recent_list_layout->setSpacing(5);
        right_layout->addWidget(home_recent_panel_, 1);

        home_columns_layout_->addWidget(home_left_col_);
        home_columns_layout_->addWidget(vline);
        home_columns_layout_->addWidget(right_col, 1);

        center_row->addWidget(home_welcome_frame_);
        center_row->addStretch(1);
        home_outer_layout_->addLayout(center_row);
        home_outer_layout_->addStretch(1);

        home_widget_ = page;
        page_stack_->addWidget(home_widget_);
    }

    if (home_resume_button_) {
        home_resume_button_->setVisible(document_ != nullptr);
    }

    page_stack_->setCurrentWidget(home_widget_);
    if (home_recent_panel_) {
        auto* list_layout = qobject_cast<QVBoxLayout*>(home_recent_panel_->layout());
        while (list_layout && list_layout->count() > 0) {
            QLayoutItem* item = list_layout->takeAt(0);
            if (QWidget* widget = item->widget()) widget->deleteLater();
            delete item;
        }
        QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
        const QStringList paths = settings.value(QStringLiteral("recentProjects")).toStringList();
        int shown = 0;
        for (const auto& path : paths) {
            const QFileInfo info(path);
            if (!info.exists() || !info.isFile()) continue;
            auto* recent = new QPushButton(home_recent_panel_);
            recent->setObjectName(QStringLiteral("recentRow"));
            recent->setToolTip(info.absoluteFilePath());
            recent->setCursor(Qt::PointingHandCursor);
            recent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

            auto* inner = new QHBoxLayout(recent);
            inner->setContentsMargins(14, 9, 14, 9);
            inner->setSpacing(16);

            auto* meta_col = new QVBoxLayout;
            meta_col->setSpacing(2);
            auto* name_lbl = new QLabel(info.fileName(), recent);
            name_lbl->setObjectName(QStringLiteral("recentTitleLabel"));
            name_lbl->setAttribute(Qt::WA_TransparentForMouseEvents);

            const QString raw_folder = QDir::toNativeSeparators(info.absolutePath());
            auto* path_lbl = new QLabel(raw_folder, recent);
            path_lbl->setObjectName(QStringLiteral("recentPathLabel"));
            path_lbl->setProperty("rawFolder", raw_folder);
            path_lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
            meta_col->addWidget(name_lbl);
            meta_col->addWidget(path_lbl);

            auto* date_lbl = new QLabel(info.lastModified().toString(QStringLiteral("dd/MM/yyyy")), recent);
            date_lbl->setObjectName(QStringLiteral("recentDateLabel"));
            date_lbl->setAttribute(Qt::WA_TransparentForMouseEvents);

            inner->addLayout(meta_col, 1);
            inner->addWidget(date_lbl, 0, Qt::AlignVCenter);

            list_layout->addWidget(recent);
            connect(recent, &QPushButton::clicked, this, [this, path = info.absoluteFilePath()] {
                openRecentProject(path);
            });
            if (++shown == 8) break;
        }
        if (shown == 0) {
            list_layout->addSpacing(24);
            auto* empty = new QLabel(QStringLiteral("Aucun projet récent"), home_recent_panel_);
            empty->setObjectName(QStringLiteral("recentEmptyLabel"));
            list_layout->addWidget(empty);
        }
        list_layout->addStretch(1);
    }
    last_home_scale_ = -1.0;
    rescaleHomePage();
    statusBar()->hide();
}

void MainWindow::rescaleHomePage() {
    if (!home_widget_ || !home_welcome_frame_ || !home_left_col_) return;
    const int avail_w = std::max(width(), home_widget_->width());
    const int avail_h = std::max(height(), home_widget_->height());

    // Smooth proportional scale factor: 1.0 at ~1120x720 up to 1.62 on large / fullscreen Retina displays
    const qreal sx = avail_w / 1160.0;
    const qreal sy = avail_h / 740.0;
    const qreal s = std::clamp(std::min(sx, sy), 0.92, 1.62);

    const int frame_w = std::clamp(qRound(avail_w * 0.72), 720, qRound(1480 * std::max<qreal>(1.0, s / 1.35)));
    const int frame_h = std::clamp(qRound(avail_h * 0.56), 380, 820);
    home_welcome_frame_->setFixedWidth(frame_w);
    home_welcome_frame_->setMinimumHeight(frame_h);

    const int col_gap = qRound(44 * s);
    const int left_w = qRound(282 * s);
    if (home_outer_layout_) {
        const int mx = qRound(48 * s);
        const int my = qRound(34 * s);
        home_outer_layout_->setContentsMargins(mx, my, mx, my);
    }
    if (home_columns_layout_) home_columns_layout_->setSpacing(col_gap);
    home_left_col_->setFixedWidth(left_w);

    if (std::abs(s - last_home_scale_) >= 0.015) {
        last_home_scale_ = s;
        const int logo_sz = qRound(46 * s);
        if (home_logo_label_) {
            home_logo_label_->setFixedSize(logo_sz, logo_sz);
            home_logo_label_->setPixmap(roundedLogoPixmap(logo_sz));
        }
        if (home_wordmark_label_) {
            home_wordmark_label_->setStyleSheet(
                QStringLiteral("font-size: %1pt; font-weight: 700; color: #F4F4F6;")
                    .arg(qRound(18.5 * s)));
        }
        if (home_version_label_) {
            home_version_label_->setStyleSheet(
                QStringLiteral("font-size: %1pt; color: #82828C;")
                    .arg(std::max(9, qRound(9.5 * s))));
        }
        if (home_brand_spacer_) {
            home_brand_spacer_->changeSize(1, qRound(24 * s), QSizePolicy::Minimum, QSizePolicy::Fixed);
            if (home_left_col_->layout()) home_left_col_->layout()->invalidate();
        }

        const int btn_h = qRound(40 * s);
        const int btn_pt = std::max(10, qRound(10.2 * s));
        const int btn_radius = std::max(6, qRound(6.0 * s));
        const QString primary_qss = QStringLiteral(
            "QPushButton { background: #ECECEF; border: 1px solid #ECECEF; border-radius: %1px; "
            "padding: 7px 16px; color: #111113; font-size: %2pt; font-weight: 600; }"
            "QPushButton:hover { background: #FFFFFF; border-color: #FFFFFF; color: #09090B; }"
            "QPushButton:pressed { background: #D4D4D8; border-color: #D4D4D8; }")
            .arg(btn_radius).arg(btn_pt);
        const QString secondary_qss = QStringLiteral(
            "QPushButton { background: #1E1E22; border: 1px solid #2C2C33; border-radius: %1px; "
            "padding: 7px 14px; color: #E4E4E7; font-size: %2pt; font-weight: 600; }"
            "QPushButton:hover { background: #26262C; border-color: #3A3A44; color: #FFFFFF; }"
            "QPushButton:pressed { background: #18181C; }")
            .arg(btn_radius).arg(btn_pt);
        const QString ghost_qss = QStringLiteral(
            "QPushButton { background: transparent; border: 1px solid transparent; border-radius: %1px; "
            "padding: 6px 12px; color: #A8A8B2; font-size: %2pt; font-weight: 500; }"
            "QPushButton:hover { background: #222227; color: #F4F4F6; }")
            .arg(btn_radius).arg(std::max(10, qRound(9.8 * s)));

        if (home_new_btn_) {
            home_new_btn_->setMinimumHeight(btn_h);
            home_new_btn_->setStyleSheet(primary_qss);
        }
        for (QPushButton* btn : {home_open_btn_, home_voice_btn_}) {
            if (!btn) continue;
            btn->setMinimumHeight(btn_h);
            btn->setStyleSheet(secondary_qss);
        }
        if (home_resume_button_) {
            home_resume_button_->setMinimumHeight(qRound(36 * s));
            home_resume_button_->setStyleSheet(ghost_qss);
        }
        if (home_recent_heading_) {
            home_recent_heading_->setStyleSheet(
                QStringLiteral("color: #B4B4BE; font-size: %1pt; font-weight: 600;")
                    .arg(std::max(10, qRound(10.8 * s))));
        }
        if (home_view_all_btn_) {
            home_view_all_btn_->setStyleSheet(
                QStringLiteral("QPushButton { background: transparent; border: 0; border-radius: 5px; "
                               "color: #82828C; font-size: %1pt; font-weight: 500; padding: 4px 9px; }"
                               "QPushButton:hover { background: #222227; color: #EDEDEF; }")
                    .arg(std::max(9, qRound(9.5 * s))));
        }
    }

    if (home_recent_panel_) {
        const int right_col_w = std::max(280, frame_w - left_w - col_gap - 4);
        const int path_avail_w = std::max(200, right_col_w - qRound(135 * s));
        const int row_h = qRound(54 * s);
        const int pad_x = qRound(13 * s);
        const int pad_y = qRound(8 * s);
        const int title_pt = std::max(10, qRound(10.6 * s));
        const int sub_pt = std::max(9, qRound(9.0 * s));

        const auto rows = home_recent_panel_->findChildren<QPushButton*>(
            QStringLiteral("recentRow"), Qt::FindDirectChildrenOnly);
        for (QPushButton* row_btn : rows) {
            row_btn->setMinimumHeight(row_h);
            if (auto* lay = row_btn->layout()) lay->setContentsMargins(pad_x, pad_y, pad_x, pad_y);
            if (auto* title_lbl = row_btn->findChild<QLabel*>(QStringLiteral("recentTitleLabel"))) {
                title_lbl->setStyleSheet(
                    QStringLiteral("color: #EDEDEF; font-size: %1pt; font-weight: 600;").arg(title_pt));
            }
            if (auto* date_lbl = row_btn->findChild<QLabel*>(QStringLiteral("recentDateLabel"))) {
                date_lbl->setStyleSheet(
                    QStringLiteral("color: #787882; font-size: %1pt;").arg(sub_pt));
            }
            if (auto* path_lbl = row_btn->findChild<QLabel*>(QStringLiteral("recentPathLabel"))) {
                path_lbl->setStyleSheet(
                    QStringLiteral("color: #82828C; font-size: %1pt;").arg(sub_pt));
                const QString raw = path_lbl->property("rawFolder").toString();
                if (!raw.isEmpty()) {
                    QFont pf = path_lbl->font();
                    pf.setPointSize(sub_pt);
                    path_lbl->setText(QFontMetrics(pf).elidedText(raw, Qt::ElideMiddle, path_avail_w));
                }
            }
        }
        if (auto* empty_lbl = home_recent_panel_->findChild<QLabel*>(QStringLiteral("recentEmptyLabel"))) {
            empty_lbl->setStyleSheet(
                QStringLiteral("color: #71717A; font-size: %1pt; padding-left: 2px;")
                    .arg(std::max(10, qRound(10.2 * s))));
        }
    }
}

void MainWindow::showProjectPage(const QString& module) {
    if (module == QStringLiteral("Séparation vocale")) {
        openVoiceSeparation();
        return;
    }
    if (!confirmDiscardChanges()) return;
    statusBar()->hide();
    active_project_module_ = module;
    if (!projects_widget_) {
        projects_widget_ = new QWidget(page_stack_);
        projects_widget_->setObjectName(QStringLiteral("projectsPage"));
        auto* outer = new QHBoxLayout(projects_widget_);
        outer->setContentsMargins(40, 28, 40, 28);
        outer->addStretch(1);

        auto* container = new QWidget(projects_widget_);
        projects_container_ = container;
        container->setMaximumWidth(std::clamp(qRound(width() * 0.72), 820, 1360));
        container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 4, 0, 4);
        layout->setSpacing(12);

        auto* nav_row = new QHBoxLayout;
        auto* back_top = new QPushButton(QStringLiteral("← Accueil"), container);
        back_top->setObjectName(QStringLiteral("ghostButton"));
        connect(back_top, &QPushButton::clicked, this, [this] { showHome(); });
        nav_row->addWidget(back_top);
        nav_row->addStretch(1);
        layout->addLayout(nav_row);

        projects_title_ = new QLabel(container);
        projects_title_->setStyleSheet(QStringLiteral("color: #F4F4F6; font-size: 16pt; font-weight: 700;"));
        layout->addWidget(projects_title_);

        auto* filter_row = new QHBoxLayout;
        auto* recent_heading = new QLabel(QStringLiteral("Projets récents"), container);
        recent_heading->setObjectName(QStringLiteral("sectionCaption"));
        projects_search_edit_ = new QLineEdit(container);
        projects_search_edit_->setPlaceholderText(QStringLiteral("Rechercher…"));
        projects_search_edit_->setClearButtonEnabled(true);
        projects_search_edit_->setMaximumWidth(260);
        filter_row->addWidget(recent_heading);
        filter_row->addStretch(1);
        filter_row->addWidget(projects_search_edit_);
        layout->addLayout(filter_row);

        recent_projects_list_ = new QListWidget(container);
        recent_projects_list_->setMinimumHeight(260);
        recent_projects_empty_ = new QLabel(QStringLiteral("Aucun projet récent."), container);
        recent_projects_empty_->setStyleSheet(QStringLiteral("color: #71717A; font-size: 9pt; padding: 4px;"));

        connect(projects_search_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (!recent_projects_list_) return;
            const QString q = text.trimmed();
            for (int i = 0; i < recent_projects_list_->count(); ++i) {
                auto* item = recent_projects_list_->item(i);
                item->setHidden(!q.isEmpty() && !item->text().contains(q, Qt::CaseInsensitive));
            }
        });
        connect(recent_projects_list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
            if (item) openRecentProject(item->data(Qt::UserRole).toString());
        });

        auto* actions = new QHBoxLayout;
        actions->setSpacing(8);
        auto* back = new QPushButton(QStringLiteral("Retour"), container);
        auto* open_recent = new QPushButton(QStringLiteral("Ouvrir la sélection"), container);
        auto* open = new QPushButton(QStringLiteral("Parcourir…"), container);
        project_continue_button_ = new QPushButton(container);
        project_continue_button_->setObjectName(QStringLiteral("primaryButton"));
        for (auto* button : {back, open_recent, open, project_continue_button_}) button->setMinimumHeight(34);
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
                createNewDocument(true);
            } else {
                openStudioModule(active_project_module_);
            }
        });

        layout->addWidget(recent_projects_list_, 1);
        layout->addWidget(recent_projects_empty_);
        layout->addLayout(actions);
        outer->addWidget(container, 1);
        outer->addStretch(1);
        page_stack_->addWidget(projects_widget_);
    }
    projects_title_->setText(module);
    project_continue_button_->setText(module == QStringLiteral("Bande Rythmo")
        ? QStringLiteral("Nouvelle bande rythmo")
        : QStringLiteral("Ouvrir"));
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
        const QString modified = info.lastModified().toString(QStringLiteral("dd/MM/yyyy HH:mm"));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   ·   %2\n%3").arg(info.fileName(), modified, QDir::toNativeSeparators(info.absolutePath())));
        item->setData(Qt::UserRole, absolute);
        item->setToolTip(absolute);
        recent_projects_list_->addItem(item);
    }
    if (existing != paths) {
        settings.setValue(QStringLiteral("recentProjects"), existing);
        settings.sync();
    }
    if (recent_projects_list_->count() > 0 && !recent_projects_list_->currentItem()) {
        recent_projects_list_->setCurrentRow(0);
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
    dialog.setWindowTitle(QStringLiteral("Police de la bande rythmo"));
    dialog.setMinimumWidth(460);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("Police de la bande rythmo"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 700; color: #F4F4F6;"));
    auto* description = new QLabel(QStringLiteral("Choisissez une police installée ou importez un fichier (.ttf, .otf)."), &dialog);
    description->setWordWrap(true);
    description->setStyleSheet(QStringLiteral("color: #8A8A93;"));
    auto* font_combo = new QFontComboBox(&dialog);
    font_combo->setCurrentFont(QFont(settings.value(QStringLiteral("timelineFontFamily"), QStringLiteral("Montserrat")).toString()));
    auto* preview = new QLabel(QStringLiteral("La voix prend sa place à la barre."), &dialog);
    preview->setMinimumHeight(68);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral(
        "background: #161619; border: 1px solid #26262C; border-radius: 7px; color: #F4F4F6; padding: 12px;"));
    auto* import = new QPushButton(QStringLiteral("Importer un fichier de police…"), &dialog);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addWidget(font_combo);
    layout->addWidget(preview);
    layout->addWidget(import, 0, Qt::AlignLeft);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Appliquer"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annuler"));
    layout->addSpacing(4);
    layout->addWidget(buttons);
    QString imported_path = imported_timeline_font_path_;
    const auto update_preview = [font_combo, preview] {
        QFont font = font_combo->currentFont();
        font.setBold(true);
        font.setPointSize(16);
        preview->setFont(font);
    };
    connect(font_combo, &QFontComboBox::currentFontChanged, &dialog, [update_preview](const QFont&) {
        update_preview();
    });
    update_preview();
    connect(import, &QPushButton::clicked, &dialog, [&dialog, font_combo, &imported_path] {
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
    status_label_->setText(QStringLiteral("Police de la bande rythmo définie sur « %1 ».").arg(chosen.family()));
}

void MainWindow::showEditor() {
    if (page_stack_ && editor_widget_) page_stack_->setCurrentWidget(editor_widget_);
    statusBar()->show();
    updateEditorHeader();
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
            if (answer == QMessageBox::Yes) createNewDocument(true);
        }
        return;
    }
    auto* page = new QDialog(this);
    page->setWindowTitle(module);
    page->setMinimumSize(480, 220);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 20);
    auto* title = new QLabel(module, page);
    title->setStyleSheet(QStringLiteral("font-size: 16pt; font-weight: 700; color: #F4F4F6;"));
    auto* description = new QLabel(QStringLiteral("Cet espace Rythmo n’est pas disponible."), page);
    description->setWordWrap(true);
    description->setStyleSheet(QStringLiteral("font-size: 10pt; color: #8A8A93;"));
    auto* close = new QPushButton(QStringLiteral("Retour à Rythmo"), page);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addStretch(1);
    layout->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, page, &QDialog::accept);
    page->exec();
    page->deleteLater();
}

void MainWindow::openVoiceSeparation() {
    const bool return_to_editor = page_stack_ && page_stack_->currentWidget() == editor_widget_ && document_ != nullptr;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Séparation de voix IA | Rythmo"));
    dialog.setMinimumSize(780, 580);
    dialog.resize(880, 640);
    dialog.setStyleSheet(studioStyleSheet());

    const QString app_data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString runtime_root = QDir(app_data_dir).filePath(QStringLiteral("cinematic_separation_runtime"));
    const QString model_dir = QDir(runtime_root).filePath(QStringLiteral("checkpoints"));
    QDir().mkpath(model_dir);
    QSettings model_settings;
    QString custom_model_id = model_settings.value(QStringLiteral("voice/customModelId")).toString();
    QString custom_model_directory = model_settings.value(QStringLiteral("voice/customModelDirectory")).toString();
    QString custom_checkpoint = model_settings.value(QStringLiteral("voice/customCheckpoint")).toString();
    QString custom_config = model_settings.value(QStringLiteral("voice/customConfig")).toString();

    struct ModelSpec {
        QString repo;
        QString checkpoint;
        QString index_file;
        qint64 minimum_bytes;
        int sample_rate;
        QString summary;
        QString download_url;
        QString sha256;
        QString config_url;
        QString config_sha256;
    };
    const auto model_spec = [&](const QString& id) {
        if (!custom_model_id.isEmpty() && id == custom_model_id)
            return ModelSpec{QStringLiteral("custom"), custom_checkpoint, custom_config, 1024, 44100,
                QStringLiteral("Modèle RoFormer personnel importé localement."), {}, {}, {}, {}};
        if (id == QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"))
            return ModelSpec{QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"), QStringLiteral("inst_v1e_plus.ckpt"), QStringLiteral("config_melbandroformer_inst.yaml"), 913090472, 44100,
                QStringLiteral("Profil Studio (Élevé) · Qualité d’isolation maximale · 44,1 kHz · ~913 Mo."),
                QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-Inst/resolve/f86cd9e99d63eb9499b00fca424bc4ed8a8aeaba/inst_v1e_plus.ckpt"),
                QStringLiteral("6a4ddba739f0352407fb6e18b29206b82318ec427fe37fcedb0f83241e4e15fb"),
                QStringLiteral("https://huggingface.co/pcunwa/Mel-Band-Roformer-Inst/resolve/b5eb552e1fd019392ad5f912aebe2e8bdd481421/config_melbandroformer_inst.yaml"), {}};
        if (id == QStringLiteral("BS-RoFormer-Viperx-1297"))
            return ModelSpec{QStringLiteral("BS-RoFormer-Viperx-1297"), QStringLiteral("model_bs_roformer_ep_317_sdr_12.9755.ckpt"), QStringLiteral("model_bs_roformer_ep_317_sdr_12.9755.yaml"), 639000000, 44100,
                QStringLiteral("Profil Équilibré (Moyen) · Excellent compromis vitesse / précision · 44,1 kHz · ~639 Mo."),
                QStringLiteral("https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/model_bs_roformer_ep_317_sdr_12.9755.ckpt"),
                QStringLiteral("5b84f37e8d444c8cb30c79d77f613a41c05868ff9c9ac6c7049c00aefae115aa"),
                QStringLiteral("https://raw.githubusercontent.com/TRvlvr/application_data/main/mdx_model_data/mdx_c_configs/model_bs_roformer_ep_317_sdr_12.9755.yaml"),
                QStringLiteral("2bfdd16c656bd9519aba757cc4f8834b7ede675eb1e00ec4772d74ae1c41af7f")};
        return ModelSpec{QStringLiteral("Mel-RoFormer-Small-Unwa"), QStringLiteral("melband_roformer_small_v1.ckpt"), QStringLiteral("config_melbandroformer_small.yaml"), 200000000, 44100,
            QStringLiteral("Profil Rapide (Faible) · Léger et adapté aux machines modestes · 44,1 kHz · ~203 Mo."),
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
    root->setContentsMargins(0, 0, 0, 0);
    auto* stack = new QStackedWidget(&dialog);
    root->addWidget(stack, 1);

    // Page 1: Profile Setup & Download
    auto* setup = new QWidget(stack);
    auto* setup_layout = new QVBoxLayout(setup);
    setup_layout->setContentsMargins(30, 26, 30, 22);
    setup_layout->setSpacing(12);

    auto* heading = new QLabel(QStringLiteral("Modèle de séparation vocale"), setup);
    heading->setStyleSheet(QStringLiteral("font-size: 16pt; font-weight: 700; color: #F4F4F6;"));
    auto* intro = new QLabel(
        QStringLiteral("Choisissez un profil RoFormer. Les modèles sont exécutés localement sur votre appareil."),
        setup);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9.5pt;"));

    auto* profile_card = new QFrame(setup);
    profile_card->setObjectName(QStringLiteral("studioCard"));
    auto* pc_layout = new QVBoxLayout(profile_card);
    pc_layout->setContentsMargins(18, 16, 18, 16);
    pc_layout->setSpacing(10);

    auto* profile_lbl = new QLabel(QStringLiteral("Profil"), profile_card);
    profile_lbl->setObjectName(QStringLiteral("sectionCaption"));
    auto* profile = new QComboBox(profile_card);
    profile->setMinimumHeight(36);
    profile->addItem(QStringLiteral("Faible (Rapide et léger)"), QStringLiteral("Mel-RoFormer-Small-Unwa"));
    profile->addItem(QStringLiteral("Moyen (Équilibré)"), QStringLiteral("BS-RoFormer-Viperx-1297"));
    profile->addItem(QStringLiteral("Élevé (Qualité studio)"), QStringLiteral("Mel-RoFormer-Unwa-v1e-plus"));
    if (!custom_model_id.isEmpty() && QFileInfo(custom_model_directory).isDir())
        profile->addItem(QStringLiteral("Modèle personnel"), custom_model_id);
    profile->addItem(QStringLiteral("Importer un modèle personnel…"), QStringLiteral("__import__"));
    const QString initial_profile = model_settings.value(QStringLiteral("voice/selectedProfile"),
        QStringLiteral("Mel-RoFormer-Small-Unwa")).toString();
    profile->setCurrentIndex(std::max(0, profile->findData(initial_profile)));

    auto* model_summary = new QLabel(profile_card);
    model_summary->setWordWrap(true);
    model_summary->setStyleSheet(QStringLiteral("color: #D4D4D8; font-size: 9.5pt;"));

    auto* status_row = new QHBoxLayout;
    auto* model_info = new QLabel(profile_card);
    model_info->setWordWrap(true);
    model_info->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9pt; font-weight: 500;"));
    auto* delete_model_button = new QPushButton(QStringLiteral("Supprimer le modèle"), profile_card);
    delete_model_button->setObjectName(QStringLiteral("dangerGhostButton"));
    delete_model_button->setEnabled(false);
    status_row->addWidget(model_info, 1);
    status_row->addWidget(delete_model_button);

    pc_layout->addWidget(profile_lbl);
    pc_layout->addWidget(profile);
    pc_layout->addWidget(model_summary);
    pc_layout->addLayout(status_row);

    const auto update_model_info = [model_info, model_summary, model_path, model_spec, model_is_installed, model_directory_exists, profile, delete_model_button, custom_model_id] {
        const QString id = profile->currentData().toString();
        if (id == QStringLiteral("__import__")) {
            model_summary->setText(QStringLiteral("Chargez un checkpoint RoFormer (.ckpt) accompagné de sa configuration YAML à 44,1 kHz."));
            model_info->setText(QStringLiteral("Import manuel"));
            model_info->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9pt;"));
            delete_model_button->setEnabled(false);
            return;
        }
        const auto spec = model_spec(id);
        model_summary->setText(spec.summary);
        const QFileInfo info(model_path(id));
        if (model_is_installed(id)) {
            model_info->setText(QStringLiteral("Installé · %1 Mo")
                .arg(info.size() / (1024.0 * 1024.0), 0, 'f', 0));
            model_info->setStyleSheet(QStringLiteral("color: #D4D4D8; font-size: 9pt; font-weight: 600;"));
        } else if (model_directory_exists(id)) {
            model_info->setText(QStringLiteral("Installation incomplète · supprimez le modèle puis réessayez"));
            model_info->setStyleSheet(QStringLiteral("color: #FBBF24; font-size: 9pt;"));
        } else {
            model_info->setText(QStringLiteral("Téléchargement requis lors de la première utilisation (~%1 Mo)")
                                .arg(spec.minimum_bytes / (1024.0 * 1024.0), 0, 'f', 0));
            model_info->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9pt;"));
        }
        delete_model_button->setEnabled(model_directory_exists(id) && id != custom_model_id);
    };
    update_model_info();

    auto* progress_text = new QLabel(setup);
    progress_text->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9pt;"));
    auto* progress = new QProgressBar(setup);
    progress->setRange(0, 1000);
    progress->hide();
    progress_text->hide();

    auto* actions = new QHBoxLayout;
    auto* back = new QPushButton(return_to_editor ? QStringLiteral("Fermer") : QStringLiteral("Retour"), setup);
    auto* continue_button = new QPushButton(setup);
    continue_button->setObjectName(QStringLiteral("primaryButton"));
    continue_button->setMinimumHeight(34);
    continue_button->setDefault(true);
    const auto update_continue_text = [continue_button, model_is_installed, profile] {
        const QString id = profile->currentData().toString();
        continue_button->setText(id == QStringLiteral("__import__")
            ? QStringLiteral("Importer un modèle…")
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
    setup_layout->addSpacing(4);
    setup_layout->addWidget(profile_card);
    setup_layout->addWidget(progress_text);
    setup_layout->addWidget(progress);
    setup_layout->addStretch(1);
    setup_layout->addLayout(actions);
    stack->addWidget(setup);

    // Page 2: Batch Separation Workspace
    auto* separation = new QWidget(stack);
    auto* separation_layout = new QVBoxLayout(separation);
    separation_layout->setContentsMargins(30, 24, 30, 20);
    separation_layout->setSpacing(10);

    auto* sep_header = new QHBoxLayout;
    auto* separation_title = new QLabel(QStringLiteral("Séparation de voix"), separation);
    separation_title->setStyleSheet(QStringLiteral("font-size: 16pt; font-weight: 700; color: #F4F4F6;"));
    auto* change_profile = new QPushButton(QStringLiteral("Modèle…"), separation);
    sep_header->addWidget(separation_title, 1);
    sep_header->addWidget(change_profile, 0, Qt::AlignVCenter);

    auto* separation_description = new QLabel(
        QStringLiteral("Génère une piste Voix et une piste Musique & Effets (M&E) pour chaque fichier vidéo ou audio."),
        separation);
    separation_description->setWordWrap(true);
    separation_description->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9.5pt;"));

    auto* drop_zone = new QLabel(
        QStringLiteral("Glisser-déposer des fichiers vidéo ou audio"),
        separation);
    drop_zone->setObjectName(QStringLiteral("voiceDropZone"));
    drop_zone->setAlignment(Qt::AlignCenter);
    drop_zone->setAcceptDrops(true);
    drop_zone->setMinimumHeight(74);
    drop_zone->setStyleSheet(QStringLiteral(
        "background: #161619; border: 1px dashed #32323A; border-radius: 8px; "
        "color: #A1A1AA; padding: 12px; font-size: 9.5pt;"));
    drop_zone->installEventFilter(this);

    auto* sources = new QListWidget(separation);
    sources->setMinimumHeight(112);
    sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
    drop_zone->setProperty("sourceListWidget", QVariant::fromValue(static_cast<QObject*>(sources)));

    auto* source_row = new QHBoxLayout;
    source_row->setSpacing(8);
    auto* choose_source = new QPushButton(QStringLiteral("Ajouter des fichiers…"), separation);
    auto* remove_sources = new QPushButton(QStringLiteral("Retirer"), separation);
    remove_sources->setObjectName(QStringLiteral("ghostButton"));
    source_row->addWidget(choose_source);
    source_row->addWidget(remove_sources);
    source_row->addStretch(1);

    auto* export_box = new QFrame(separation);
    export_box->setObjectName(QStringLiteral("studioCard"));
    auto* export_box_layout = new QGridLayout(export_box);
    export_box_layout->setContentsMargins(14, 12, 14, 12);
    export_box_layout->setHorizontalSpacing(10);
    export_box_layout->setVerticalSpacing(8);

    auto* out_lbl = new QLabel(QStringLiteral("Dossier de sortie"), export_box);
    out_lbl->setStyleSheet(QStringLiteral("color: #A1A1AA; font-weight: 500;"));
    auto* output_dir = new QLineEdit(export_box);
    output_dir->setPlaceholderText(QStringLiteral("Dossier de destination"));
    output_dir->setText(QDir(app_data_dir).filePath(QStringLiteral("output")));
    auto* choose_output = new QPushButton(QStringLiteral("Parcourir…"), export_box);

    auto* format_label = new QLabel(QStringLiteral("Format audio"), export_box);
    format_label->setStyleSheet(QStringLiteral("color: #A1A1AA; font-weight: 500;"));
    auto* output_format = new QComboBox(export_box);
    output_format->addItem(QStringLiteral("WAV (PCM sans perte)"), QStringLiteral("wav"));
    output_format->addItem(QStringLiteral("FLAC (compression sans perte)"), QStringLiteral("flac"));
    output_format->addItem(QStringLiteral("MP3"), QStringLiteral("mp3"));
    output_format->addItem(QStringLiteral("OGG Vorbis"), QStringLiteral("ogg"));
    output_format->addItem(QStringLiteral("M4A / AAC"), QStringLiteral("m4a"));

    export_box_layout->addWidget(out_lbl, 0, 0);
    export_box_layout->addWidget(output_dir, 0, 1);
    export_box_layout->addWidget(choose_output, 0, 2);
    export_box_layout->addWidget(format_label, 1, 0);
    export_box_layout->addWidget(output_format, 1, 1);
    export_box_layout->setColumnStretch(1, 1);

    auto* runtime_info = new QLabel(
        QStringLiteral("Traitement local sur cet appareil."),
        separation);
    runtime_info->setWordWrap(true);
    runtime_info->setStyleSheet(QStringLiteral(
        "background: #161619; border: 1px solid #25252B; border-radius: 6px; padding: 9px 12px; color: #8A8A93; font-size: 9pt;"));

    auto* work_progress = new QProgressBar(separation);
    work_progress->setRange(0, 0);
    work_progress->hide();

    auto* work_actions = new QHBoxLayout;
    work_actions->setSpacing(10);
    auto* close = new QPushButton(QStringLiteral("Fermer"), separation);
    auto* separate_button = new QPushButton(QStringLiteral("Séparer Voix + M&E"), separation);
    separate_button->setObjectName(QStringLiteral("primaryButton"));
    separate_button->setMinimumHeight(34);
    work_actions->addWidget(close);
    work_actions->addStretch(1);
    work_actions->addWidget(separate_button);

    separation_layout->addLayout(sep_header);
    separation_layout->addWidget(separation_description);
    separation_layout->addWidget(drop_zone);
    separation_layout->addWidget(sources, 1);
    separation_layout->addLayout(source_row);
    separation_layout->addWidget(export_box);
    separation_layout->addWidget(runtime_info);
    separation_layout->addWidget(work_progress);
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
        Q_UNUSED(python_dir);
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
        return true;
#endif
    };
    const auto start_torch_install = [&](bool use_mirror = false) {
        [[maybe_unused]] bool nvidia_cuda_available = false;
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
    connect(back, &QPushButton::clicked, &dialog, [&, return_to_editor] {
        if (downloader.state() != QProcess::NotRunning) downloader.kill();
        else {
            Q_UNUSED(return_to_editor);
            dialog.reject();
        }
    });
    connect(&downloader, &QProcess::readyReadStandardError, &dialog, [&downloader, &active_download_output, progress_text] {
        const QString output = QString::fromLocal8Bit(downloader.readAllStandardError()).trimmed();
        if (!output.isEmpty()) {
            active_download_output = output;
            progress_text->setText(QStringLiteral("Téléchargement sécurisé · %1").arg(output.right(100)));
        }
    });
    connect(&downloader, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog, [&, progress, progress_text, return_to_editor](int exit_code, QProcess::ExitStatus exit_status) {
        const auto spec = model_spec(active_model_id);
        downloading = false;
        profile->setEnabled(true);
        continue_button->setEnabled(true);
        back->setText(return_to_editor ? QStringLiteral("Retour à l’éditeur") : QStringLiteral("Retour"));
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
                QStringLiteral("Le client curl est introuvable. Vérifiez la connexion Internet et la présence de curl sur le système, puis réessayez."));
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
            item->setIcon(makeStudioIcon(StudioIcon::Film, QColor(QStringLiteral("#A1A1AA")), 15));
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
            runtime_info->setStyleSheet(QStringLiteral(
                "background: #1B231E; border: 1px solid #2A3F32; "
                "border-radius: 6px; padding: 9px 12px; color: #D4D4D8; font-weight: 500;"));
            runtime_info->setText(QStringLiteral("Séparation terminée : %1 fichier(s) enregistré(s) dans %2")
                .arg(job_final_outputs.size()).arg(job_output_dir));
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
        job_ffmpeg = findBundledOrSystemTool(QStringLiteral("ffmpeg"));
        if (!job_ffmpeg.isEmpty()) ffmpeg_path_ = job_ffmpeg;
        if (job_ffmpeg.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("FFmpeg requis"), QStringLiteral("FFmpeg est nécessaire pour décoder les vidéos, préparer les pistes et encoder les sorties. Placez ffmpeg avec Rythmo ou ajoutez-le au PATH."));
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
        runtime_info->setStyleSheet(QStringLiteral(
            "background: #161619; border: 1px solid #25252B; border-radius: 6px; padding: 9px 12px; color: #8A8A93; font-size: 9pt;"));
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
    if (return_to_editor && document_) showEditor();
    else showHome();
    show();
    raise();
    activateWindow();
}

void MainWindow::createMenus() {
    auto* file_menu = menuBar()->addMenu(QStringLiteral("Fichier"));
    auto* home_action = file_menu->addAction(QStringLiteral("Accueil Rythmo"));
    connect(home_action, &QAction::triggered, this, [this] { showHome(); });
    file_menu->addSeparator();

    auto* new_action = file_menu->addAction(QStringLiteral("Nouveau document"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, [this] { createNewDocument(true); });

    auto* open_action = file_menu->addAction(QStringLiteral("Ouvrir…"));
    open_action->setShortcut(QKeySequence::Open);
    connect(open_action, &QAction::triggered, this, [this] { openDocument(); });

    auto* save_action = file_menu->addAction(QStringLiteral("Enregistrer"));
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, [this] { saveDocument(); });

    auto* save_as_action = file_menu->addAction(QStringLiteral("Enregistrer sous…"));
    save_as_action->setShortcut(QKeySequence::SaveAs);
    connect(save_as_action, &QAction::triggered, this, [this] { saveDocumentAs(); });

    file_menu->addSeparator();
    auto* open_video_action = file_menu->addAction(QStringLiteral("Ouvrir la vidéo source…"));
    connect(open_video_action, &QAction::triggered, this, [this] { openVideo(); });

    auto* export_action = file_menu->addAction(QStringLiteral("Exporter la vidéo rythmo…"));
    export_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(export_action, &QAction::triggered, this, [this] { exportVideoWithRhythm(); });

    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(QStringLiteral("Quitter"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    auto* edit_menu = menuBar()->addMenu(QStringLiteral("Édition"));
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

    auto* add_line_action = edit_menu->addAction(QStringLiteral("Ajouter une réplique"));
    add_line_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(add_line_action, &QAction::triggered, this, [this] { addLine(); });

    auto* remove_line_action = edit_menu->addAction(QStringLiteral("Supprimer la réplique sélectionnée"));
    remove_line_action->setShortcut(QKeySequence::Delete);
    connect(remove_line_action, &QAction::triggered, this, [this] { removeSelectedLine(); });

    auto* split_action = edit_menu->addAction(QStringLiteral("Scinder la réplique…"));
    split_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(split_action, &QAction::triggered, this, [this] { splitSelectedLine(); });

    auto* shot_action = edit_menu->addAction(QStringLiteral("Ajouter un changement de plan"));
    shot_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    shot_action->setShortcutContext(Qt::ApplicationShortcut);
    connect(shot_action, &QAction::triggered, this, [this] { addShotMarker(); });

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("Outils"));
    auto* roles_action = tools_menu->addAction(QStringLiteral("Distribution et rôles…"));
    connect(roles_action, &QAction::triggered, this, [this] { manageRoles(); });
    auto* shift_tc_action = tools_menu->addAction(QStringLiteral("Décaler tous les timecodes…"));
    connect(shift_tc_action, &QAction::triggered, this, [this] { shiftDocumentTimecodes(); });
    auto* validate_action = tools_menu->addAction(QStringLiteral("Diagnostic du document DetX…"));
    connect(validate_action, &QAction::triggered, this, [this] { showDocumentDiagnostics(); });
    tools_menu->addSeparator();
    auto* separation_action = tools_menu->addAction(QStringLiteral("Séparation de voix IA…"));
    connect(separation_action, &QAction::triggered, this, [this] { openVoiceSeparation(); });

    auto* view_menu = menuBar()->addMenu(QStringLiteral("Affichage"));
    auto* fullscreen_action = view_menu->addAction(QStringLiteral("Vidéo + bande rythmo en plein écran"));
    fullscreen_action->setShortcut(QKeySequence(Qt::Key_F11));
    connect(fullscreen_action, &QAction::triggered, this, [this] { toggleRhythmFullscreen(); });
    view_menu->addSeparator();
    auto* zoom_in_action = view_menu->addAction(QStringLiteral("Zoomer sur la bande"));
    connect(zoom_in_action, &QAction::triggered, this, [this] { if (timeline_) timeline_->zoomIn(); });
    auto* zoom_out_action = view_menu->addAction(QStringLiteral("Dézoomer la bande"));
    connect(zoom_out_action, &QAction::triggered, this, [this] { if (timeline_) timeline_->zoomOut(); });
    auto* zoom_reset_action = view_menu->addAction(QStringLiteral("Réinitialiser le zoom (8,0 s)"));
    connect(zoom_reset_action, &QAction::triggered, this, [this] { if (timeline_) timeline_->resetZoom(); });

    auto* help_menu = menuBar()->addMenu(QStringLiteral("Aide"));
    auto* shortcuts_action = help_menu->addAction(QStringLiteral("Raccourcis clavier et gestes…"));
    connect(shortcuts_action, &QAction::triggered, this, [this] { showShortcutsDialog(); });
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

namespace {

bool bufferContainsFourCC(const QByteArray& buf, const char (&fourcc)[5]) noexcept {
    if (buf.size() < 4) return false;
    const char* data = buf.constData();
    const qsizetype limit = buf.size() - 4;
    for (qsizetype i = 0; i <= limit; ++i) {
        if (data[i] == fourcc[0] && data[i + 1] == fourcc[1] &&
            data[i + 2] == fourcc[2] && data[i + 3] == fourcc[3]) {
            return true;
        }
    }
    return false;
}

bool needsDecodeProxyFast(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QStringLiteral("webm") || ext == QStringLiteral("ivf") ||
        ext == QStringLiteral("av1") || ext == QStringLiteral("ogv")) {
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const qint64 total_size = file.size();
    const QByteArray head = file.read(65536);
    auto check_chunk = [](const QByteArray& chunk) noexcept {
        return bufferContainsFourCC(chunk, "av01") ||
               bufferContainsFourCC(chunk, "av1C") ||
               bufferContainsFourCC(chunk, "vp09") ||
               bufferContainsFourCC(chunk, "vpcC") ||
               bufferContainsFourCC(chunk, "vp08") ||
               bufferContainsFourCC(chunk, "dav1");
    };
    if (check_chunk(head)) return true;
    if (total_size > 65536) {
        if (file.seek(std::max<qint64>(65536, total_size - 65536))) {
            const QByteArray tail = file.read(65536);
            if (check_chunk(tail)) return true;
        }
    }
    return false;
}

QString persistentProxyCacheDir(const QString& fallback_dir) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = fallback_dir;
    if (base.isEmpty()) return {};
    const QString dir_path = QDir(base).filePath(QStringLiteral("video-proxies"));
    QDir().mkpath(dir_path);
    return dir_path;
}

void prunePersistentProxyCache(const QString& cache_dir, qint64 max_bytes = 1610612736LL) {
    if (cache_dir.isEmpty()) return;
    QDir dir(cache_dir);
    auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<QFileInfo> valid_files;
    for (const QFileInfo& info : entries) {
        if (info.fileName().contains(QStringLiteral(".part."))) {
            if (info.lastModified().toUTC().secsTo(now) > 600) {
                QFile::remove(info.absoluteFilePath());
            }
            continue;
        }
        total += info.size();
        valid_files.push_back(info);
    }
    for (const QFileInfo& oldest : valid_files) {
        if (total <= max_bytes) break;
        if (QFile::remove(oldest.absoluteFilePath())) {
            total -= oldest.size();
        }
    }
}

QString persistentProxyBaseForSource(const QString& source_path, const QString& fallback_dir) {
    const QString cache_dir = persistentProxyCacheDir(fallback_dir);
    if (cache_dir.isEmpty()) return {};
    const QFileInfo info(source_path);
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(canonical.toUtf8());
    hash.addData(QByteArrayView("|", 1));
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArrayView("|", 1));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    hash.addData(QByteArrayView("|rythmo-scrub-v2", 16));
    return QDir(cache_dir).filePath(
        QStringLiteral("proxy-%1").arg(QString::fromLatin1(hash.result().toHex().left(24))));
}

} // namespace

void MainWindow::loadDocument(const QString& path) {
    try {
        auto loaded = DetxDocument::load(fs_path(path));
        if (seek_coalesce_timer_) seek_coalesce_timer_->stop();
        if (video_decode_watchdog_timer_) video_decode_watchdog_timer_->stop();
        pending_media_seek_ms_ = -1;
        media_seek_in_flight_ = false;
        last_dispatched_seek_ms_ = -1;
        last_dispatched_seek_frame_ = -1;
        received_video_frames_ = 0;
        player_->stop();
        player_->setSource(QUrl{});
        document_ = std::make_unique<DetxDocument>(std::move(loaded));
        resetUndoHistory(true);
        document_path_ = QFileInfo(path).absoluteFilePath();
        rememberRecentProject(document_path_);
        timecode_fps_ = document_->fps();
        video_start_ = video_start_timecode(*document_).value_or(Timecode{});
        video_path_.clear();
        media_fps_ = 0.0;
        current_position_ms_ = 0;
        last_slider_permille_ = -1;
        last_displayed_timecode_ = Timecode{-1, -1, -1, -1};
        if (lines_search_edit_) lines_search_edit_->clear();

        QString resolved_video;
        const QString stored_video = video_path_from_document(*document_);
        if (!stored_video.isEmpty()) {
            const QFileInfo stored_info(stored_video);
            const QString candidate = stored_info.isAbsolute()
                ? stored_info.absoluteFilePath()
                : QFileInfo(QFileInfo(document_path_).absoluteDir(), stored_video).absoluteFilePath();
            if (QFileInfo::exists(candidate)) {
                resolved_video = candidate;
                video_path_ = QFileInfo(candidate).absoluteFilePath();
            }
        }

        refreshTable();
        refreshTimeline();
        updatePosition(0);
        setDirty(false);
        showEditor();

        if (!resolved_video.isEmpty()) {
            // Load video synchronously right as the DetX project opens (zero empty-screen flash!)
            loadVideo(resolved_video, false);
        } else if (!stored_video.isEmpty()) {
            status_label_->setText(
                QStringLiteral("Document chargé : %1 rôle(s), %2 réplique(s), %3 i/s · Vidéo référencée introuvable, associez-la manuellement.")
                    .arg(static_cast<qulonglong>(document_->roles().size()))
                    .arg(static_cast<qulonglong>(document_->lines().size()))
                    .arg(timecode_fps_));
        } else {
            status_label_->setText(QStringLiteral("Document chargé : %1 rôle(s), %2 réplique(s), %3 i/s%4")
                                   .arg(static_cast<qulonglong>(document_->roles().size()))
                                   .arg(static_cast<qulonglong>(document_->lines().size()))
                                   .arg(timecode_fps_)
                                   .arg(document_->fps_is_assumed() ? QStringLiteral(" (cadence à confirmer)") : QString{}));
        }
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Ouverture impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::saveDocument() {
    if (!document_) return;
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (document_path_.isEmpty()) { saveDocumentAs(); return; }
    try {
        document_->save(fs_path(document_path_));
        setDirty(false);
        rememberRecentProject(document_path_);
        status_label_->setText(QStringLiteral("Projet enregistré : %1").arg(QFileInfo(document_path_).fileName()));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Enregistrement impossible"), QString::fromUtf8(error.what()));
    }
}

bool MainWindow::saveDocumentAs() {
    if (!document_) return false;
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Enregistrer le document DetX"),
                                                   document_path_, QStringLiteral("Documents DetX (*.detx)"));
    if (path.isEmpty()) return false;
    document_path_ = path.endsWith(QStringLiteral(".detx"), Qt::CaseInsensitive) ? path : path + QStringLiteral(".detx");
    saveDocument();
    return !dirty_;
}

bool MainWindow::confirmDiscardChanges() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!dirty_) return true;
    const auto result = QMessageBox::warning(this, QStringLiteral("Modifications non enregistrées"),
        QStringLiteral("Le document a été modifié. Enregistrer avant de continuer ?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (result == QMessageBox::Cancel) return false;
    if (result == QMessageBox::Save) { saveDocument(); return !dirty_; }
    return true;
}

void MainWindow::openVideo() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Ouvrir la vidéo source"), {},
        QStringLiteral("Vidéos courantes (*.mp4 *.m4v *.mov *.mkv *.avi *.webm *.wmv *.mpg *.mpeg *.m2v *.ts *.mts *.m2ts *.mxf *.flv *.3gp *.ogv *.vob *.ivf *.av1);;Tous les fichiers (*.*)"));
    if (!path.isEmpty()) loadVideo(path);
}

void MainWindow::loadVideo(const QString& path, bool persist_reference) {
    if (poster_frame_process_ && poster_frame_process_->state() != QProcess::NotRunning) {
        disconnect(poster_frame_process_, nullptr, this, nullptr);
        poster_frame_process_->kill();
        poster_frame_process_->deleteLater();
        poster_frame_process_ = nullptr;
    }
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
    if (seek_coalesce_timer_) seek_coalesce_timer_->stop();
    if (video_decode_watchdog_timer_) video_decode_watchdog_timer_->stop();
    pending_media_seek_ms_ = -1;
    media_seek_in_flight_ = false;
    last_dispatched_seek_ms_ = -1;
    last_dispatched_seek_frame_ = -1;
    received_video_frames_ = 0;
    fallback_resume_pos_ms_ = 0;
    fallback_resume_playing_ = false;
    fallback_resume_pending_ = false;
    decode_proxy_candidates_.clear();
    player_->stop();
    player_->setSource(QUrl{});
    if (player_->videoOutput() != video_widget_) player_->setVideoOutput(video_widget_);
    if (!decode_proxy_path_.isEmpty() && !decode_proxy_is_persistent_) {
        QFile::remove(decode_proxy_path_);
    }
    decode_proxy_path_.clear();
    decode_proxy_final_path_.clear();
    decode_proxy_is_persistent_ = false;
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
    updateEditorHeader();
    refreshTimeline();

    // 1. Instant persistent scrub-optimized proxy cache hit (< 15ms load!)
    const QString proxy_base = persistentProxyBaseForSource(
        video_path_, decode_temp_dir_.isValid() ? decode_temp_dir_.path() : QString{});
    if (!proxy_base.isEmpty()) {
        for (const QString& ext : {QStringLiteral("mp4"), QStringLiteral("mkv")}) {
            const QString candidate_proxy = proxy_base + QLatin1Char('.') + ext;
            if (QFileInfo(candidate_proxy).size() > 0) {
                decode_proxy_path_ = candidate_proxy;
                decode_proxy_is_persistent_ = true;
                decode_proxy_active_ = true;
                decode_fallback_attempted_ = true;
                status_label_->setText(QStringLiteral("Vidéo prête : %1").arg(QFileInfo(video_path_).fileName()));
                player_->setSource(QUrl::fromLocalFile(decode_proxy_path_));
                return;
            }
        }
    }

    if (ffmpeg_path_.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg introuvable : placez FFmpeg à côté de l’application ou ajoutez-le au PATH."));
        player_->setSource(QUrl::fromLocalFile(video_path_));
        return;
    }

    // 2. Zero-wait AV1/VP9 container detection: display Frame 0 in ~50ms and build persistent scrub proxy immediately!
    if (needsDecodeProxyFast(video_path_)) {
        requestInstantPosterFrame(video_path_, current_position_ms_);
        startFfmpegDecodeFallback(QStringLiteral("Optimisation de l’aperçu fluide (AV1/VP9)"));
        return;
    }

    // 3. Native H.264 / HEVC / ProRes direct hardware playback
    status_label_->setText(QStringLiteral("Préchargement vidéo : %1").arg(QFileInfo(video_path_).fileName()));
    player_->setSource(QUrl::fromLocalFile(video_path_));
}

void MainWindow::requestInstantPosterFrame(const QString& path, qint64 position_ms) {
    if (path.isEmpty() || !video_widget_ || !video_widget_->videoSink()) return;
    const QString ffmpeg = ffmpeg_path_.isEmpty() ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty()) return;

    if (poster_frame_process_ && poster_frame_process_->state() != QProcess::NotRunning) {
        disconnect(poster_frame_process_, nullptr, this, nullptr);
        poster_frame_process_->kill();
        poster_frame_process_->deleteLater();
        poster_frame_process_ = nullptr;
    }

    auto* proc = new QProcess(this);
    poster_frame_process_ = proc;
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    auto buf = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, buf] {
        buf->append(proc->readAllStandardOutput());
    });
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, proc, buf](int code, QProcess::ExitStatus st) {
            buf->append(proc->readAllStandardOutput());
            proc->deleteLater();
            if (poster_frame_process_ == proc) poster_frame_process_ = nullptr;
            if (code != 0 || st != QProcess::NormalExit || buf->isEmpty() || received_video_frames_ > 0) return;
            QImage poster = QImage::fromData(*buf, "BMP");
            if (!poster.isNull() && video_widget_ && video_widget_->videoSink()) {
                video_widget_->videoSink()->setVideoFrame(QVideoFrame(poster));
            }
        });

    const QString ss_arg = QString::number(std::max<qint64>(0, position_ms) / 1000.0, 'f', 3);
    proc->start(ffmpeg, {
        QStringLiteral("-nostdin"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-ss"), ss_arg,
        QStringLiteral("-i"), path,
        QStringLiteral("-frames:v"), QStringLiteral("1"),
        QStringLiteral("-vf"), QStringLiteral("scale='min(1280,iw)':-2"),
        QStringLiteral("-f"), QStringLiteral("image2pipe"),
        QStringLiteral("-c:v"), QStringLiteral("bmp"),
        QStringLiteral("-")
    });
}

void MainWindow::startFfmpegDecodeFallback(const QString& qt_error) {
    if (decode_fallback_attempted_) return;
    decode_fallback_attempted_ = true;
    const QString ffmpeg = ffmpeg_path_.isEmpty() ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg est requis pour le décodage vidéo. Placez FFmpeg à côté de l’application ou ajoutez-le au PATH."));
        return;
    }

    // Reuse session- and QSettings-cached FFmpeg encoder list so we never pay process-spawn latency twice!
    const QFileInfo ff_info(ffmpeg);
    const QString ff_signature = QStringLiteral("%1|%2|%3")
        .arg(ff_info.canonicalFilePath().isEmpty() ? ff_info.absoluteFilePath() : ff_info.canonicalFilePath())
        .arg(ff_info.size())
        .arg(ff_info.lastModified().toMSecsSinceEpoch());
    static QString s_cached_ff_sig;
    static QString s_cached_encoders;
    if (s_cached_ff_sig == ff_signature && !s_cached_encoders.isEmpty()) {
        startFfmpegDecodeProxy(s_cached_encoders);
        return;
    }
    {
        QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
        if (settings.value(QStringLiteral("ffmpegEncoderSig")).toString() == ff_signature) {
            const QString persisted = settings.value(QStringLiteral("ffmpegEncoderList")).toString();
            if (!persisted.isEmpty()) {
                s_cached_ff_sig = ff_signature;
                s_cached_encoders = persisted;
                startFfmpegDecodeProxy(persisted);
                return;
            }
        }
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
        [this, probe, qt_error, ff_signature](int code, QProcess::ExitStatus state) {
            const QString encoders = probe->property("encoderOutput").toString()
                + QString::fromLocal8Bit(probe->readAll());
            probe->deleteLater();
            if (decode_encoder_probe_process_ == probe) decode_encoder_probe_process_ = nullptr;
            if (code != 0 || state != QProcess::NormalExit) {
                decode_fallback_attempted_ = false;
                status_label_->setText(QStringLiteral("FFmpeg ne répond pas : %1").arg(qt_error));
                return;
            }
            s_cached_ff_sig = ff_signature;
            s_cached_encoders = encoders;
            QSettings settings(QStringLiteral("Dubia Productions"), QStringLiteral("Rythmo by Dubia Productions"));
            settings.setValue(QStringLiteral("ffmpegEncoderSig"), ff_signature);
            settings.setValue(QStringLiteral("ffmpegEncoderList"), encoders);
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
    if (video_path_.isEmpty()) return;
    const QString ffmpeg = ffmpeg_path_.isEmpty()
        ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg introuvable pour décoder cette vidéo."));
        return;
    }
    const auto has_enc = [&](const char* token) {
        return encoders.contains(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QLatin1String(token))));
    };
    decode_proxy_candidates_.clear();
#ifdef Q_OS_MACOS
    if (has_enc("h264_videotoolbox")) decode_proxy_candidates_ << QStringLiteral("h264_videotoolbox");
    if (has_enc("libx264")) decode_proxy_candidates_ << QStringLiteral("libx264");
#else
    // Try GPU hardware encoders first across Windows & Linux, then guaranteed CPU libx264/mpeg4 fallback!
    if (has_enc("h264_nvenc")) decode_proxy_candidates_ << QStringLiteral("h264_nvenc");
    if (has_enc("h264_qsv")) decode_proxy_candidates_ << QStringLiteral("h264_qsv");
    if (has_enc("h264_amf")) decode_proxy_candidates_ << QStringLiteral("h264_amf");
    if (has_enc("h264_mf")) decode_proxy_candidates_ << QStringLiteral("h264_mf");
    if (has_enc("libx264")) decode_proxy_candidates_ << QStringLiteral("libx264");
    if (has_enc("mpeg4")) decode_proxy_candidates_ << QStringLiteral("mpeg4");
#endif

    if (has_enc("aac")) decode_proxy_audio_encoder_ = QStringLiteral("aac");
    else if (has_enc("libmp3lame")) decode_proxy_audio_encoder_ = QStringLiteral("libmp3lame");
    else decode_proxy_audio_encoder_ = QStringLiteral("pcm_s16le");

    if (decode_proxy_candidates_.isEmpty()) {
        status_label_->setText(QStringLiteral("FFmpeg doit fournir un encodeur H.264 pour créer l’aperçu vidéo."));
        return;
    }
    decode_log_tail_.clear();
    launchNextDecodeProxyCandidate();
}

void MainWindow::launchNextDecodeProxyCandidate() {
    if (video_path_.isEmpty() || decode_proxy_candidates_.isEmpty()) return;
    const QString ffmpeg = ffmpeg_path_.isEmpty()
        ? findBundledOrSystemTool(QStringLiteral("ffmpeg")) : ffmpeg_path_;
    if (ffmpeg.isEmpty()) return;

    const QString video_encoder = decode_proxy_candidates_.takeFirst();
    const QString audio_encoder = decode_proxy_audio_encoder_;
    const bool is_software_encoder = (video_encoder == QStringLiteral("libx264") ||
                                      video_encoder == QStringLiteral("mpeg4"));

#ifdef Q_OS_MACOS
    const QString preview_extension = QStringLiteral("mp4");
#else
    const QString preview_extension = (audio_encoder == QStringLiteral("aac") && video_encoder != QStringLiteral("mpeg4"))
        ? QStringLiteral("mp4") : QStringLiteral("mkv");
#endif
    const QString fallback_dir = decode_temp_dir_.isValid() ? decode_temp_dir_.path() : QDir::tempPath();
    const QString cache_dir = persistentProxyCacheDir(fallback_dir);
    prunePersistentProxyCache(cache_dir);
    const QString proxy_base = persistentProxyBaseForSource(video_path_, fallback_dir);
    if (!proxy_base.isEmpty()) {
        decode_proxy_final_path_ = proxy_base + QLatin1Char('.') + preview_extension;
        decode_proxy_path_ = proxy_base + QStringLiteral(".part.%1.%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8), preview_extension);
        decode_proxy_is_persistent_ = false;
    } else {
        decode_proxy_final_path_.clear();
        decode_proxy_path_ = QDir(fallback_dir).filePath(
            QStringLiteral("preview-%1.%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), preview_extension));
        decode_proxy_is_persistent_ = false;
    }

    QStringList args{
        QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-hide_banner")
    };
    if (!is_software_encoder) {
        args << QStringLiteral("-hwaccel") << QStringLiteral("auto");
    }
    // 720p + short 8-frame GOP + zero B-frames: encodes 3x faster AND makes trackpad scrubbing instant (< 3ms/seek)!
    args << QStringLiteral("-i") << video_path_
         << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-map") << QStringLiteral("0:a:0?")
         << QStringLiteral("-sn") << QStringLiteral("-dn")
         << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2")
         << QStringLiteral("-g") << QStringLiteral("8")
         << QStringLiteral("-c:v") << video_encoder;

    if (video_encoder == QStringLiteral("libx264")) {
        args << QStringLiteral("-preset") << QStringLiteral("ultrafast")
             << QStringLiteral("-tune") << QStringLiteral("fastdecode")
             << QStringLiteral("-bf") << QStringLiteral("0")
             << QStringLiteral("-crf") << QStringLiteral("23")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    } else if (video_encoder == QStringLiteral("h264_videotoolbox")) {
        args << QStringLiteral("-allow_sw") << QStringLiteral("1")
             << QStringLiteral("-bf") << QStringLiteral("0")
             << QStringLiteral("-b:v") << QStringLiteral("8M")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    } else if (video_encoder == QStringLiteral("h264_nvenc")) {
        args << QStringLiteral("-preset") << QStringLiteral("p1")
             << QStringLiteral("-bf") << QStringLiteral("0")
             << QStringLiteral("-cq") << QStringLiteral("24")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    } else if (video_encoder == QStringLiteral("h264_qsv") || video_encoder == QStringLiteral("h264_amf") ||
               video_encoder == QStringLiteral("h264_mf")) {
        args << QStringLiteral("-bf") << QStringLiteral("0")
             << QStringLiteral("-b:v") << QStringLiteral("8M")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    } else {
        args << QStringLiteral("-bf") << QStringLiteral("0")
             << QStringLiteral("-q:v") << QStringLiteral("4");
    }
    args << QStringLiteral("-c:a") << audio_encoder;
    if (audio_encoder == QStringLiteral("aac") || audio_encoder == QStringLiteral("libmp3lame")) {
        args << QStringLiteral("-b:a") << QStringLiteral("160k");
    }
    if (preview_extension == QStringLiteral("mp4")) {
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    args << QStringLiteral("-avoid_negative_ts") << QStringLiteral("make_zero") << decode_proxy_path_;

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
    decode_fallback_running_ = true;
    status_label_->setText(QStringLiteral("Optimisation rapide de la vidéo pour lecture fluide (%1)…")
        .arg(QFileInfo(video_path_).fileName()));
    decode_fallback_process_->start(ffmpeg, args);
}

void MainWindow::finishFfmpegDecodeFallback(int exit_code, QProcess::ExitStatus status) {
    if (!decode_fallback_running_) return;
    decode_fallback_running_ = false;
    if (decode_fallback_process_)
        decode_log_tail_ += QString::fromLocal8Bit(decode_fallback_process_->readAll());
    if (exit_code == 0 && status == QProcess::NormalExit && QFileInfo(decode_proxy_path_).size() > 0) {
        if (!decode_proxy_final_path_.isEmpty()) {
            QFile::remove(decode_proxy_final_path_);
            if (QFile::rename(decode_proxy_path_, decode_proxy_final_path_)) {
                decode_proxy_path_ = decode_proxy_final_path_;
                decode_proxy_is_persistent_ = true;
            }
            decode_proxy_final_path_.clear();
        }
        if (player_->playbackState() == QMediaPlayer::PlayingState) {
            fallback_resume_playing_ = true;
            fallback_resume_pos_ms_ = current_position_ms_;
        } else if (current_position_ms_ > 0) {
            fallback_resume_pos_ms_ = current_position_ms_;
        }
        fallback_resume_pending_ = true;
        decode_proxy_candidates_.clear();
        decode_proxy_active_ = true;
        received_video_frames_ = 0;
        if (video_decode_watchdog_timer_) video_decode_watchdog_timer_->stop();
        player_->stop();
        if (player_->videoOutput() != video_widget_) player_->setVideoOutput(video_widget_);
        player_->setSource(QUrl::fromLocalFile(decode_proxy_path_));
        status_label_->setText(QStringLiteral("Vidéo prête · %1").arg(QFileInfo(video_path_).fileName()));
        return;
    }
    QFile::remove(decode_proxy_path_);
    decode_proxy_path_.clear();
    if (!decode_proxy_candidates_.isEmpty()) {
        launchNextDecodeProxyCandidate();
        return;
    }
    QMessageBox::warning(this, QStringLiteral("Décodage vidéo impossible"),
        QStringLiteral("Qt Multimedia n’a pas pu lire cette vidéo et FFmpeg n’a pas réussi à créer l’aperçu. Vérifiez que FFmpeg inclut le décodeur du codec source et qu’il reste assez d’espace temporaire.\n\n%1")
            .arg(decode_log_tail_.right(2500)));
}

void MainWindow::updateTableRow(int row) {
    if (!document_ || !lines_table_ || row < 0 || row >= static_cast<int>(document_->lines().size()) ||
        row >= lines_table_->rowCount()) return;

    const QScopedValueRollback<bool> guard(populating_table_, true);
    const QSignalBlocker blocker(lines_table_);

    static const QFont tc_font = [] {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSize(10);
        f.setStyleHint(QFont::Monospace);
        return f;
    }();

    const auto& line = document_->lines()[static_cast<std::size_t>(row)];
    auto* in = lines_table_->item(row, 0);
    if (!in) { in = new QTableWidgetItem(); lines_table_->setItem(row, 0, in); }
    in->setText(first_timecode(line, true));
    in->setFont(tc_font);
    in->setForeground(QColor(QStringLiteral("#E4E4E7")));
    if (line.lipsyncs.empty()) in->setFlags(in->flags() & ~Qt::ItemIsEditable);
    else in->setFlags(in->flags() | Qt::ItemIsEditable);

    auto* out = lines_table_->item(row, 1);
    if (!out) { out = new QTableWidgetItem(); lines_table_->setItem(row, 1, out); }
    out->setText(first_timecode(line, false));
    out->setFont(tc_font);
    out->setForeground(QColor(QStringLiteral("#8A8A93")));
    if (line.lipsyncs.empty()) out->setFlags(out->flags() & ~Qt::ItemIsEditable);
    else out->setFlags(out->flags() | Qt::ItemIsEditable);

    QString role_label = QStringLiteral("Sans rôle");
    QColor swatch_color(QStringLiteral("#52525B"));
    if (!line.role_id.empty()) {
        role_label = qstr(line.role_id);
        swatch_color = QColor(QStringLiteral("#60A5FA"));
        for (const auto& r : document_->roles()) {
            if (r.id == line.role_id) {
                role_label = qstr(r.name);
                swatch_color = displayRoleColor(r.color);
                break;
            }
        }
    }
    auto* role_item = lines_table_->item(row, 2);
    if (!role_item) { role_item = new QTableWidgetItem(); lines_table_->setItem(row, 2, role_item); }
    role_item->setText(role_label);
    role_item->setIcon(makeRoleSwatchIcon(swatch_color, 11));
    role_item->setData(Qt::UserRole, qstr(line.role_id));
    role_item->setFlags(role_item->flags() | Qt::ItemIsEditable);

    const int clamped_track = std::clamp(line.track, 0, 3);
    auto* track_item = lines_table_->item(row, 3);
    if (!track_item) { track_item = new QTableWidgetItem(); lines_table_->setItem(row, 3, track_item); }
    track_item->setText(QStringLiteral("P%1").arg(clamped_track + 1));
    track_item->setData(Qt::UserRole, clamped_track);
    track_item->setForeground(QColor(QStringLiteral("#A8A8B2")));
    track_item->setToolTip(QStringLiteral("Piste %1").arg(clamped_track + 1));
    track_item->setFlags(track_item->flags() | Qt::ItemIsEditable);

    const QString display_text = qstr(line.text).replace(QLatin1Char('\n'), QStringLiteral("  ↵  "));
    auto* text = lines_table_->item(row, 4);
    if (!text) { text = new QTableWidgetItem(); lines_table_->setItem(row, 4, text); }
    text->setText(display_text);
    text->setToolTip(qstr(line.text));
    text->setFlags(text->flags() & ~Qt::ItemIsEditable);
}

void MainWindow::refreshTable() {
    populating_table_ = true;
    const QSignalBlocker blocker(lines_table_);
    lines_table_->setUpdatesEnabled(false);
    const int count = document_ ? static_cast<int>(document_->lines().size()) : 0;
    lines_table_->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        updateTableRow(row);
    }
    lines_table_->setUpdatesEnabled(true);
    populating_table_ = false;
    updateEditorHeader();
    applyLinesFilter();
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
        } else if (column == 2) {
            line.role_id = utf8(item->data(Qt::UserRole).toString());
            if (line.element) line.element->set_attribute("role", line.role_id);
        } else if (column == 3) {
            const QVariant user_val = item->data(Qt::UserRole);
            int value = 0;
            if (user_val.isValid()) {
                value = std::clamp(user_val.toInt(), 0, 3);
            } else {
                bool ok = false;
                value = item->text().toInt(&ok);
                if (!ok || value < 0 || value > 3) throw std::invalid_argument("La piste doit être comprise entre 0 et 3.");
            }
            line.track = value;
            if (line.element) line.element->set_attribute("track", std::to_string(value));
            selected_timeline_track_ = value;
            updateTrackPills();
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
        timeline_->updateLineCache(row);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Modification refusée"), QString::fromUtf8(error.what()));
        updateTableRow(row);
    }
}

void MainWindow::addLine() {
    addLineAt(selected_timeline_track_, current_position_ms_);
}

void MainWindow::addLineAt(int track, qint64 position_ms) {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_) {
        QMessageBox::information(this, QStringLiteral("Aucun document"), QStringLiteral("Créez ou ouvrez d’abord un document DetX."));
        return;
    }
    track = std::clamp(track, 0, 3);
    selected_timeline_track_ = track;
    updateTrackPills();
    if (document_->roles().empty()) {
        QMessageBox::information(this, QStringLiteral("Aucun personnage"),
            QStringLiteral("Ajoutez d’abord un personnage pour lui attribuer cette réplique."));
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
        } else if (document_->roles().size() == 1) {
            role = document_->roles().front().id;
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
        setDirty(true);
        if (lines_search_edit_) lines_search_edit_->clear();
        refreshTable();
        lines_table_->selectRow(inserted_row);
        lines_table_->scrollToItem(lines_table_->item(inserted_row, 4));
        timeline_->setSelection(inserted_row, selected_timeline_track_);
        status_label_->setText(QStringLiteral("Réplique créée sur la Piste %1 à %2.")
                               .arg(selected_timeline_track_ + 1).arg(qstr(in.format())));
        editTimelineLine(inserted_row);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Création impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::editTimelineLine(int row, int cue_index) {
    if (!document_ || row < 0 || row >= static_cast<int>(document_->lines().size())) return;
    auto& line = document_->lines()[static_cast<std::size_t>(row)];
    if (line.role_id.empty()) {
        if (document_->roles().empty()) {
            QMessageBox::information(this, QStringLiteral("Aucun rôle"),
                QStringLiteral("Ajoutez un personnage avant de modifier cette réplique."));
            manageRoles();
            if (!document_ || document_->roles().empty()) return;
        }
        QStringList names;
        for (const auto& role : document_->roles()) {
            const QString name = qstr(role.name);
            const auto duplicates = std::count_if(document_->roles().begin(), document_->roles().end(),
                [&](const Role& other) { return other.name == role.name; });
            names << (duplicates > 1 ? name + QStringLiteral(" (") + qstr(role.id) + QLatin1Char(')') : name);
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(this, QStringLiteral("Personnage de la réplique"),
            QStringLiteral("Qui parle ?"), names, 0, false, &accepted);
        if (!accepted) return;
        const int index = names.indexOf(chosen);
        if (index < 0 || index >= static_cast<int>(document_->roles().size())) return;
        line.role_id = document_->roles()[static_cast<std::size_t>(index)].id;
        if (line.element) line.element->set_attribute("role", line.role_id);
        setDirty(true);
        refreshTable();
    }
    timeline_->beginInlineEdit(row, cue_index);
}

void MainWindow::splitSelectedLine() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_) return;
    const int row = lines_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(document_->lines().size())) {
        status_label_->setText(QStringLiteral("Sélectionnez une réplique avant de la scinder."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Scinder la réplique"));
    dialog.setMinimumWidth(380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("Point de scission"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 700; color: #F4F4F6;"));
    auto* desc = new QLabel(QStringLiteral("Timecode du point de coupe (HH:MM:SS:FF) à l’intérieur de la réplique :"), &dialog);
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("color: #8A8A93;"));
    auto* cut_timecode = new QLineEdit(qstr(currentTimecode().format()), &dialog);
    layout->addWidget(title);
    layout->addWidget(desc);
    layout->addWidget(cut_timecode);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Scinder"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annuler"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addSpacing(4);
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
        status_label_->setText(QStringLiteral("Réplique scindée au timecode choisi · Chaque segment dispose de son propre intervalle."));
    } catch (const std::exception& error) {
        QMessageBox::information(this, QStringLiteral("Scission impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::addShotMarker() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_) {
        status_label_->setText(QStringLiteral("Ouvrez ou créez un projet DetX avant d’ajouter un changement de plan."));
        return;
    }
    const Timecode timecode = currentTimecode();
    try {
        if (!document_->add_shot_marker(timecode)) {
            status_label_->setText(QStringLiteral("Un changement de plan existe déjà à %1.")
                                   .arg(QString::fromStdString(timecode.format())));
            return;
        }
        setDirty(true);
        refreshTimeline();
        status_label_->setText(QStringLiteral("Changement de plan ajouté à %1.")
                               .arg(QString::fromStdString(timecode.format())));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Changement de plan impossible"),
                             QString::fromUtf8(error.what()));
    }
}

void MainWindow::shiftDocumentTimecodes() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_) {
        QMessageBox::information(this, QStringLiteral("Décalage de timecodes"),
            QStringLiteral("Ouvrez d’abord un document DetX pour décaler ses timecodes."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Décaler tous les timecodes"));
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Décalage temporel global"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 700; color: #F4F4F6;"));
    auto* desc = new QLabel(
        QStringLiteral("Décale l’ensemble des répliques, boucles et changements de plan d’un nombre d’images (cadence actuelle : %1 i/s).").arg(document_->fps()),
        &dialog);
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("color: #8A8A93;"));
    auto* frames_spin = new QSpinBox(&dialog);
    frames_spin->setRange(-360000, 360000);
    frames_spin->setValue(0);
    frames_spin->setSuffix(QStringLiteral(" image(s)"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Appliquer le décalage"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annuler"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(title);
    layout->addWidget(desc);
    layout->addWidget(frames_spin);
    layout->addSpacing(4);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || frames_spin->value() == 0) return;
    try {
        document_->shift_all_timecodes(static_cast<std::int64_t>(frames_spin->value()));
        setDirty(true);
        refreshTable();
        status_label_->setText(QStringLiteral("Tous les timecodes ont été décalés de %1 image(s).")
            .arg(frames_spin->value()));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Décalage impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::showDocumentDiagnostics() {
    if (!document_) {
        QMessageBox::information(this, QStringLiteral("Diagnostic DetX"),
            QStringLiteral("Aucun document DetX n’est ouvert."));
        return;
    }
    const auto issues = document_->validate();
    if (issues.empty()) {
        QMessageBox::information(this, QStringLiteral("Diagnostic DetX"),
            QStringLiteral("Aucune anomalie détectée · Le document DetX est parfaitement valide (%1 rôle(s), %2 réplique(s), %3 i/s).")
                .arg(static_cast<qulonglong>(document_->roles().size()))
                .arg(static_cast<qulonglong>(document_->lines().size()))
                .arg(document_->fps()));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Diagnostic du document DetX"));
    dialog.setMinimumSize(560, 360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    auto* heading = new QLabel(QStringLiteral("%1 point(s) d’attention détecté(s)").arg(static_cast<qulonglong>(issues.size())), &dialog);
    heading->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 700; color: #FBBF24;"));
    auto* list = new QListWidget(&dialog);
    for (const auto& issue : issues) {
        list->addItem(qstr(issue.to_string()));
    }
    auto* close = new QPushButton(QStringLiteral("Fermer"), &dialog);
    close->setObjectName(QStringLiteral("primaryButton"));
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(heading);
    layout->addWidget(list, 1);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog.exec();
}

void MainWindow::showShortcutsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Raccourcis clavier & gestes | Rythmo"));
    dialog.setMinimumSize(580, 440);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Raccourcis clavier et gestes"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 14pt; font-weight: 700; color: #F4F4F6;"));
    layout->addWidget(title);

    const std::vector<std::pair<QString, QString>> shortcuts{
        {QStringLiteral("Lecture / Pause"), QStringLiteral("Espace")},
        {QStringLiteral("Plein écran / Quitter le plein écran"), QStringLiteral("F11 / Échap")},
        {QStringLiteral("Insérer une réplique à la barre rouge"), QStringLiteral("Entrée (sur la bande)")},
        {QStringLiteral("Éditer le texte directement dans la bande"), QStringLiteral("Double-clic, clic sur sélection ou F2")},
        {QStringLiteral("Passer à la réplique suivante / précédente en édition"), QStringLiteral("Tab / Maj + Tab")},
        {QStringLiteral("Scinder une réplique"), QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText)},
        {QStringLiteral("Ajouter un changement de plan"), QKeySequence(Qt::CTRL | Qt::Key_M).toString(QKeySequence::NativeText)},
        {QStringLiteral("Supprimer la réplique sélectionnée"), QStringLiteral("Suppr / Retour arrière")},
        {QStringLiteral("Réplique ou plan précédent / suivant"), QStringLiteral("← / →")},
        {QStringLiteral("Avancer ou reculer d’une image"), QStringLiteral("Alt + ←/→  ou  Maj + ←/→")},
        {QStringLiteral("Zoomer / Dézoomer sur la bande"), QStringLiteral("Molette à crans  ou  + / −")},
        {QStringLiteral("Naviguer dans le temps"), QStringLiteral("Glissement à deux doigts (Trackpad) ou clic sur la règle")},
        {QStringLiteral("Annuler / Rétablir"), QStringLiteral("%1 / %2")
            .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText),
                 QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText))}
    };

    auto* table = new QTableWidget(static_cast<int>(shortcuts.size()), 2, &dialog);
    table->setHorizontalHeaderLabels({QStringLiteral("Action"), QStringLiteral("Raccourci / Geste")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (int i = 0; i < static_cast<int>(shortcuts.size()); ++i) {
        auto* act = new QTableWidgetItem(shortcuts[static_cast<std::size_t>(i)].first);
        auto* key = new QTableWidgetItem(shortcuts[static_cast<std::size_t>(i)].second);
        key->setForeground(QColor(QStringLiteral("#D4D4D8")));
        QFont f = key->font();
        f.setBold(true);
        key->setFont(f);
        table->setItem(i, 0, act);
        table->setItem(i, 1, key);
    }
    layout->addWidget(table, 1);

    auto* close = new QPushButton(QStringLiteral("Fermer"), &dialog);
    close->setObjectName(QStringLiteral("primaryButton"));
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog.exec();
}

void MainWindow::positionFullscreenHud() {
    if (!full_screen_host_ || !full_screen_exit_button_) return;
    full_screen_exit_button_->adjustSize();
    const QSize btn_size = full_screen_exit_button_->sizeHint().expandedTo(QSize(196, 36));
    full_screen_exit_button_->resize(btn_size);
    const int x = std::max(16, full_screen_host_->width() - btn_size.width() - 20);
    const int y = (timeline_ && timeline_->parentWidget() == full_screen_host_ && timeline_->y() > 0)
        ? (timeline_->y() + 10)
        : 20;
    full_screen_exit_button_->move(x, y);
    full_screen_exit_button_->raise();
}

void MainWindow::toggleRhythmFullscreen() {
    if (!full_screen_host_) {
        full_screen_host_ = new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
        full_screen_host_->setMouseTracking(true);
        full_screen_host_->setWindowTitle(QStringLiteral("Rythmo by Dubia Productions | Vidéo + bande rythmo"));
        full_screen_layout_ = new QVBoxLayout(full_screen_host_);
        full_screen_layout_->setContentsMargins(0, 0, 0, 0);
        full_screen_layout_->setSpacing(0);
        auto* video_canvas = new FullscreenVideoCanvas(full_screen_host_);
        full_screen_video_canvas_ = video_canvas;
        full_screen_layout_->addWidget(video_canvas, 3);

        full_screen_exit_button_ = new QPushButton(QStringLiteral("Quitter le plein écran"), full_screen_host_);
        full_screen_exit_button_->setIcon(makeStudioIcon(StudioIcon::Fullscreen, QColor(QStringLiteral("#F4F4F6")), 15));
        full_screen_exit_button_->setCursor(Qt::PointingHandCursor);
        full_screen_exit_button_->setFocusPolicy(Qt::NoFocus);
        full_screen_exit_button_->setMouseTracking(true);
        full_screen_exit_button_->setToolTip(QStringLiteral("Quitter le mode plein écran (Échap ou F11)"));
        full_screen_exit_button_->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: rgba(24, 24, 29, 236);"
            "  border: 1px solid rgba(255, 255, 255, 56);"
            "  border-radius: 8px;"
            "  padding: 7px 15px;"
            "  color: #F4F4F6;"
            "  font-size: 10pt;"
            "  font-weight: 600;"
            "}"
            "QPushButton:hover {"
            "  background: rgba(38, 38, 46, 250);"
            "  border-color: rgba(255, 255, 255, 96);"
            "  color: #FFFFFF;"
            "}"
            "QPushButton:pressed {"
            "  background: rgba(18, 18, 22, 250);"
            "}"));
        full_screen_exit_button_->hide();
        connect(full_screen_exit_button_, &QPushButton::clicked, this, [this] {
            if (full_screen_active_) toggleRhythmFullscreen();
        });

        full_screen_hud_timer_ = new QTimer(this);
        full_screen_hud_timer_->setSingleShot(true);
        full_screen_hud_timer_->setInterval(2200);
        connect(full_screen_hud_timer_, &QTimer::timeout, this, [this] {
            if (!full_screen_exit_button_ || !full_screen_active_) return;
            if (full_screen_exit_button_->underMouse()) {
                full_screen_hud_timer_->start(1200);
                return;
            }
            full_screen_exit_button_->hide();
        });

        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), full_screen_host_);
        connect(escape, &QShortcut::activated, this, [this] {
            if (timeline_ && timeline_->isEditingText()) {
                timeline_->commitOrCancelEdit(false);
                return;
            }
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
            if (timeline_ && timeline_->isEditingText()) return;
            togglePlayPause();
        });
    }

    if (!full_screen_active_) {
        if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
        timeline_host_layout_->removeWidget(timeline_);
        video_host_layout_->removeWidget(video_widget_);
        full_screen_layout_->addWidget(timeline_, 2);
        static_cast<FullscreenVideoCanvas*>(full_screen_video_canvas_)->setForegroundVideo(video_widget_);
        timeline_->setCleanPresentation(true);
        if (full_screen_hud_timer_) full_screen_hud_timer_->stop();
        if (full_screen_exit_button_) full_screen_exit_button_->hide();
        last_fullscreen_mouse_pos_ = QCursor::pos();
        full_screen_active_ = true;
        hide();
        full_screen_host_->showFullScreen();
        positionFullscreenHud();
        if (full_screen_exit_button_) full_screen_exit_button_->hide();
        QTimer::singleShot(180, this, [this] {
            if (!full_screen_active_) return;
            positionFullscreenHud();
            if (full_screen_exit_button_ && !full_screen_exit_button_->isVisible()) {
                last_fullscreen_mouse_pos_ = QCursor::pos();
            }
        });
        timeline_->setFocus(Qt::OtherFocusReason);
    } else {
        if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
        if (full_screen_hud_timer_) full_screen_hud_timer_->stop();
        if (full_screen_exit_button_) full_screen_exit_button_->hide();
        full_screen_layout_->removeWidget(timeline_);
        timeline_->setCleanPresentation(false);
        static_cast<FullscreenVideoCanvas*>(full_screen_video_canvas_)->setForegroundVideo(nullptr);
        video_widget_->setParent(video_host_);
        video_host_layout_->addWidget(video_widget_, 1);
        video_widget_->show();
        if (empty_video_page_) empty_video_page_->setVisible(video_path_.isEmpty());
        if (video_host_) video_host_->setVisible(!video_path_.isEmpty());
        timeline_->setParent(timeline_host_);
        timeline_host_layout_->addWidget(timeline_, 1);
        timeline_->show();
        full_screen_active_ = false;
        full_screen_host_->hide();
        updateEditorHeader();
        showNormal();
        raise();
        activateWindow();
    }
}

void MainWindow::exportVideoWithRhythm() {
    if (export_active_) return;
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_ || video_path_.isEmpty() || player_->duration() <= 0) {
        QMessageBox::information(this, QStringLiteral("Export vidéo"),
            QStringLiteral("Ouvrez un document DetX et sa vidéo source avant de lancer l’export."));
        return;
    }
    const QString ffmpeg = findBundledOrSystemTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("FFmpeg introuvable"),
            QStringLiteral("Installez une version complète de FFmpeg et ajoutez son dossier bin au PATH. "
                           "L’export utilise FFmpeg pour détecter les encodeurs et créer la vidéo."));
        return;
    }

    QProcess encoder_probe;
    encoder_probe.setProcessChannelMode(QProcess::MergedChannels);
    encoder_probe.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
    if (!encoder_probe.waitForStarted(3000) || !encoder_probe.waitForFinished(10000)) {
        QMessageBox::warning(this, QStringLiteral("FFmpeg indisponible"), QStringLiteral("Impossible d’interroger FFmpeg."));
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
        QMessageBox::warning(this, QStringLiteral("ffprobe introuvable"),
            QStringLiteral("L’export a besoin de ffprobe, fourni avec FFmpeg, pour lire la définition et la cadence de la vidéo."));
        return;
    }
    QProcess media_probe;
    media_probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=width,height"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=0"), video_path_});
    if (!media_probe.waitForStarted(3000) || !media_probe.waitForFinished(10000) || media_probe.exitCode() != 0) {
        QMessageBox::warning(this, QStringLiteral("Lecture vidéo impossible"), QStringLiteral("ffprobe n’a pas pu analyser la vidéo source."));
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
        QMessageBox::warning(this, QStringLiteral("Définition non prise en charge"), QStringLiteral("La définition vidéo est absente ou invalide."));
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
    QString output = QFileDialog::getSaveFileName(this, QStringLiteral("Exporter la vidéo rythmo"), suggested_path,
        QStringLiteral("MP4 (*.mp4);;MOV QuickTime (*.mov);;AVI (*.avi)"), &selected_filter);
    if (output.isEmpty()) return;
    output = QFileInfo(output).absoluteFilePath();
    QString extension = QFileInfo(output).suffix().toLower();
    if (extension.isEmpty()) {
        extension = selected_filter.contains(QStringLiteral("MOV"), Qt::CaseInsensitive) ? QStringLiteral("mov") :
                    (selected_filter.contains(QStringLiteral("AVI"), Qt::CaseInsensitive) ? QStringLiteral("avi") : QStringLiteral("mp4"));
        output += QLatin1Char('.') + extension;
    }
    if (extension != QStringLiteral("mp4") && extension != QStringLiteral("mov") && extension != QStringLiteral("avi")) {
        QMessageBox::warning(this, QStringLiteral("Format non pris en charge"), QStringLiteral("Choisissez un nom de fichier en .mp4, .mov ou .avi."));
        return;
    }
    if (QFileInfo(output).absoluteFilePath().compare(QFileInfo(video_path_).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        QMessageBox::warning(this, QStringLiteral("Destination invalide"), QStringLiteral("Le fichier vidéo source ne peut pas être remplacé par son export."));
        return;
    }
    const QFileInfo output_info(output);
    if (!output_info.dir().exists()) {
        QMessageBox::warning(this, QStringLiteral("Dossier de destination introuvable"),
            QStringLiteral("Le dossier choisi n’existe pas :\n%1").arg(output_info.absolutePath()));
        return;
    }
    // Never use QStorageInfo::isReadOnly() here: on macOS APFS, /Users is a firmlink into /System/Volumes/Data
    // while / is the Read-Only Signed System Volume, causing QStorageInfo::isReadOnly() to false-positive everywhere!
    bool can_write_destination = false;
    QString write_probe_error;
    {
        QTemporaryFile write_probe(output_info.dir().filePath(QStringLiteral(".rythmo-export-check-XXXXXX")));
        write_probe.setAutoRemove(true);
        if (write_probe.open() && write_probe.write("Rythmo", 6) == 6 && write_probe.flush()) {
            can_write_destination = true;
        } else {
            write_probe_error = write_probe.errorString();
        }
    }
    if (!can_write_destination) {
        // Fallback probe for macOS TCC / Powerbox / SMB where the user granted access to the chosen file path itself
        const bool existed_before = QFileInfo::exists(output);
        QFile direct_probe(output);
        const QIODevice::OpenMode mode = existed_before
            ? (QIODevice::ReadWrite | QIODevice::Append)
            : QIODevice::WriteOnly;
        if (direct_probe.open(mode)) {
            can_write_destination = true;
            direct_probe.close();
            if (!existed_before) QFile::remove(output);
        } else {
            write_probe_error = direct_probe.errorString();
        }
    }
    if (!can_write_destination) {
        QMessageBox::warning(this, QStringLiteral("Destination non accessible en écriture"),
            QStringLiteral("Rythmo ne peut pas créer de fichiers dans le dossier choisi :\n%1\n\nDétail système : %2\n\nSur macOS, vérifiez Réglages Système > Confidentialité et sécurité > Fichiers et dossiers si le refus persiste.")
                .arg(output_info.absolutePath(), write_probe_error));
        return;
    }
    export_settings.setValue(QStringLiteral("lastVideoExportDirectory"), output_info.absolutePath());

    struct EncoderOption { QString label; QString name; bool allowed; };
    const std::vector<EncoderOption> encoder_options{
        {QStringLiteral("H.264 (logiciel, libx264)"), QStringLiteral("libx264"), true},
        {QStringLiteral("H.264 (NVIDIA NVENC)"), QStringLiteral("h264_nvenc"), true},
        {QStringLiteral("H.264 (AMD AMF)"), QStringLiteral("h264_amf"), true},
        {QStringLiteral("H.264 (Intel Quick Sync)"), QStringLiteral("h264_qsv"), true},
        {QStringLiteral("H.264 (Apple VideoToolbox)"), QStringLiteral("h264_videotoolbox"), true},
        {QStringLiteral("H.265 / HEVC (logiciel, libx265)"), QStringLiteral("libx265"), extension != QStringLiteral("avi")},
        {QStringLiteral("H.265 / HEVC (NVIDIA NVENC)"), QStringLiteral("hevc_nvenc"), extension != QStringLiteral("avi")},
        {QStringLiteral("H.265 / HEVC (AMD AMF)"), QStringLiteral("hevc_amf"), extension != QStringLiteral("avi")},
        {QStringLiteral("H.265 / HEVC (Intel Quick Sync)"), QStringLiteral("hevc_qsv"), extension != QStringLiteral("avi")},
        {QStringLiteral("H.265 / HEVC (Apple VideoToolbox)"), QStringLiteral("hevc_videotoolbox"), extension != QStringLiteral("avi")},
        {QStringLiteral("AV1 (logiciel, SVT-AV1)"), QStringLiteral("libsvtav1"), extension != QStringLiteral("avi")},
        {QStringLiteral("AV1 (logiciel, libaom)"), QStringLiteral("libaom-av1"), extension != QStringLiteral("avi")},
        {QStringLiteral("AV1 (NVIDIA NVENC)"), QStringLiteral("av1_nvenc"), extension != QStringLiteral("avi")},
        {QStringLiteral("AV1 (AMD AMF)"), QStringLiteral("av1_amf"), extension != QStringLiteral("avi")},
        {QStringLiteral("AV1 (Intel Quick Sync)"), QStringLiteral("av1_qsv"), extension != QStringLiteral("avi")}
    };

    QDialog codec_dialog(this);
    codec_dialog.setWindowTitle(QStringLiteral("Paramètres d’export vidéo"));
    codec_dialog.setMinimumWidth(620);
    auto* codec_layout = new QVBoxLayout(&codec_dialog);
    codec_layout->setContentsMargins(24, 22, 24, 20);
    codec_layout->setSpacing(14);

    auto* exp_title = new QLabel(QStringLiteral("Exporter la bande rythmo"), &codec_dialog);
    exp_title->setStyleSheet(QStringLiteral("font-size: 14pt; font-weight: 700; color: #F4F4F6;"));
    auto* dest_summary = new QLabel(
        QStringLiteral("Source : %1 × %2   ·   Sortie : %3")
            .arg(source_width).arg(source_height).arg(output_info.fileName()),
        &codec_dialog);
    dest_summary->setStyleSheet(QStringLiteral("color: #8A8A93; font-size: 9pt;"));

    auto* form_card = new QFrame(&codec_dialog);
    form_card->setObjectName(QStringLiteral("studioCard"));
    auto* codec_form = new QFormLayout(form_card);
    codec_form->setContentsMargins(18, 16, 18, 16);
    codec_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    codec_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    codec_form->setVerticalSpacing(14);

    const auto prepare_export_combo = [](QComboBox* combo) {
        if (auto* style = QStyleFactory::create(QStringLiteral("Fusion"))) {
            style->setParent(combo);
            combo->setStyle(style);
        }
        combo->setStyleSheet(QStringLiteral(
            "QComboBox { min-height: 1.5em; padding: 6px 32px 6px 12px; color: #EDEDEF; background: #19191D; border: 1px solid #2C2C33; border-radius: 6px; }"
            "QComboBox:focus { border-color: #52525B; }"
            "QComboBox::drop-down { width: 26px; border: 0; }"
            "QComboBox QAbstractItemView { color: #EDEDEF; background: #1A1A1E; selection-background-color: #2C2C34; selection-color: #FFFFFF; }"));
        combo->ensurePolished();
        combo->setMinimumHeight(combo->fontMetrics().height() + 20);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    auto* resolution_combo = new QComboBox(form_card);
    prepare_export_combo(resolution_combo);
    resolution_combo->addItem(QStringLiteral("HD 720p (1280 × 720 · 60 i/s)"), QSize(1280, 720));
    resolution_combo->addItem(QStringLiteral("Full HD 1080p (1920 × 1080 · 60 i/s)"), QSize(1920, 1080));
    resolution_combo->addItem(QStringLiteral("QHD 1440p (2560 × 1440 · 60 i/s)"), QSize(2560, 1440));
    resolution_combo->addItem(QStringLiteral("Ultra HD 4K (3840 × 2160 · 60 i/s)"), QSize(3840, 2160));
    resolution_combo->setCurrentIndex(1);
    codec_form->addRow(QStringLiteral("Résolution :"), resolution_combo);

    auto* codec_combo = new QComboBox(form_card);
    prepare_export_combo(codec_combo);
    codec_form->addRow(QStringLiteral("Encodeur :"), codec_combo);

    auto* export_effects = new QCheckBox(QStringLiteral("Reproduire l’habillage du mode plein écran (fond flouté et vignettage)"), form_card);
    export_effects->setChecked(true);
    export_effects->setToolTip(QStringLiteral(
        "Ajoute le fond vidéo flouté, le vignettage et les ombres latérales. Désactivez cette option pour un export plus rapide sur fond noir."));
    codec_form->addRow(QStringLiteral("Habillage :"), export_effects);

    auto* hardware_info = new QLabel(QStringLiteral("Vérification des encodeurs matériels et logiciels sur cet appareil…"), &codec_dialog);
    hardware_info->setWordWrap(true);
    hardware_info->setStyleSheet(QStringLiteral(
        "background: #161619; border: 1px solid #25252B; border-radius: 6px; padding: 9px 12px; color: #8A8A93; font-size: 9pt;"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &codec_dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Lancer l’export"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annuler"));
    connect(buttons, &QDialogButtonBox::accepted, &codec_dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &codec_dialog, &QDialog::reject);

    codec_layout->addWidget(exp_title);
    codec_layout->addWidget(dest_summary);
    codec_layout->addWidget(form_card);
    codec_layout->addWidget(hardware_info);
    codec_layout->addWidget(buttons);

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
        QMessageBox::warning(this, QStringLiteral("Export indisponible"),
            QStringLiteral("Impossible de créer le dossier temporaire de validation des encodeurs."));
        return;
    }
    const auto refresh_encoders = [&] {
        const QString previous = codec_combo->currentData().toString();
        const QSize size = resolution_combo->currentData().toSize();
        codec_combo->clear();
        codec_combo->setEnabled(false);
        resolution_combo->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        QProgressDialog progress(QStringLiteral("Vérification des encodeurs sur cet appareil…"), QStringLiteral("Annuler"), 0,
                                 static_cast<int>(encoder_options.size()), &codec_dialog);
        progress.setWindowModality(Qt::WindowModal);
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
                connect(&test, &QProcess::readyRead, &test, [&test] { test.readAll(); });
                QEventLoop loop;
                QTimer timeout;
                timeout.setSingleShot(true);
                connect(&test, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
                connect(&test, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
                connect(&timeout, &QTimer::timeout, &test, &QProcess::kill);
                connect(&progress, &QProgressDialog::canceled, &test, &QProcess::kill);
                QStringList probe_args{QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                    QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=c=black:s=%1x%2:r=60").arg(size.width()).arg(size.height()),
                    QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
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
            ? QStringLiteral("Vérification annulée · Changez la résolution pour relancer la détection.")
            : codec_combo->count() == 0
                ? QStringLiteral("Aucun encodeur utilisable à cette résolution dans ce format. Essayez une autre résolution ou un autre conteneur.")
                : QStringLiteral("Seuls les encodeurs validés sur cet appareil à cette définition et à 60 i/s sont proposés. L’accélération matérielle est privilégiée."));
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

    // Use FFmpeg's SIMD 3-pass boxblur at 1/3 resolution across all platforms:
    // matches softVideoBackdrop 1:1, runs at >2000 fps, and avoids FFmpeg 9 coreimage extent errors!
    const bool use_coreimage = false;

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
    export_temp_path_ = QDir(export_temp_dir_.path()).filePath(
        QStringLiteral("%1.rythmo-partial.%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), extension));
    QFile::remove(export_temp_path_);
    export_frame_index_ = 0;
    export_frame_count_ = static_cast<qint64>(std::ceil(player_->duration() * fps / 1000.0));
    if (export_frame_count_ <= 0 || export_frame_count_ > 5'000'000) {
        QMessageBox::warning(this, QStringLiteral("Durée non prise en charge"), QStringLiteral("La durée vidéo dépasse la limite d’export."));
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
        args << QStringLiteral("-stream_loop") << QStringLiteral("-1")
            << QStringLiteral("-f") << QStringLiteral("rawvideo")
            << QStringLiteral("-pixel_format") << QStringLiteral("rgba")
            << QStringLiteral("-video_size") << QStringLiteral("%1x%2").arg(export_frame_width_).arg(export_video_height_)
            << QStringLiteral("-framerate") << fps_rational
            << QStringLiteral("-i") << export_backdrop_overlay_path_;
    }
    args
        << QStringLiteral("-f") << QStringLiteral("rawvideo")
        << QStringLiteral("-pixel_format") << QStringLiteral("rgba")
        << QStringLiteral("-video_size") << raw_size << QStringLiteral("-framerate") << fps_rational
        << QStringLiteral("-i") << QStringLiteral("-") << QStringLiteral("-filter_complex") << filter
        << QStringLiteral("-map") << QStringLiteral("[v]") << QStringLiteral("-map") << QStringLiteral("0:a?")
        << QStringLiteral("-shortest") << QStringLiteral("-r") << fps_rational
        << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    args += videoExportCodecArguments(encoder, output_width, output_height);
    if (extension == QStringLiteral("avi")) {
        args << QStringLiteral("-c:a") << (has_encoder(QStringLiteral("libmp3lame")) ? QStringLiteral("libmp3lame") : QStringLiteral("pcm_s16le"));
    } else {
        args << QStringLiteral("-c:a") << QStringLiteral("aac");
    }
    if (extension == QStringLiteral("mp4")) args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    args << export_temp_path_;

    export_frame_image_ = QImage(export_frame_width_, export_band_height_, QImage::Format_RGBA8888);
    if (export_frame_image_.isNull()) {
        export_frame_width_ = export_video_height_ = export_band_height_ = 0;
        QFile::remove(export_backdrop_overlay_path_);
        export_backdrop_overlay_path_.clear();
        QMessageBox::warning(this, QStringLiteral("Mémoire insuffisante"),
            QStringLiteral("Impossible d’allouer le tampon du rendu %1 × %2.").arg(output_width).arg(output_height));
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
    export_progress_ = new QProgressDialog(
        QStringLiteral("Préparation du rendu %1 × %2 · 60 i/s…").arg(output_width).arg(output_height),
        QStringLiteral("Annuler"), 0, 1000, this);
    export_progress_->setStyleSheet(studioStyleSheet());
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
        if (export_progress_) export_progress_->setLabelText(QStringLiteral("Finalisation de l’encodage…"));
        return;
    }
    if (export_process_->bytesToWrite() >= queue_limit) export_frame_timer_->stop();
    if (export_progress_ && export_progress_clock_.elapsed() >= 100) {
        export_progress_clock_.restart();
        const QString pipeline = export_effects_enabled_
            ? export_encoder_ + QStringLiteral(" + effets cinéma") : export_encoder_;
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
        destination.setDirectWriteFallback(true);
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
            status_label_->setText(QStringLiteral("Export terminé : ") + QFileInfo(export_output_path_).fileName());
            QMessageBox::information(this, QStringLiteral("Export terminé"),
                QStringLiteral("La vidéo avec sa bande rythmo a été créée :\n") + export_output_path_);
        } else {
            QMessageBox::warning(this, QStringLiteral("Enregistrement impossible"),
                QStringLiteral("FFmpeg a terminé l’encodage, mais Rythmo n’a pas pu enregistrer le fichier dans le dossier choisi :\n%1\n\nDétail système : %2\n\nLe fichier existant, s’il y en avait un, a été conservé.")
                    .arg(QFileInfo(export_output_path_).absolutePath(), transfer_error));
        }
    } else if (export_cancelled_) {
        QFile::remove(export_temp_path_);
        status_label_->setText(QStringLiteral("Export vidéo annulé."));
    } else if (exit_code != 0 || status == QProcess::CrashExit) {
        QFile::remove(export_temp_path_);
        QMessageBox::warning(this, QStringLiteral("Export échoué"),
            QStringLiteral("FFmpeg n’a pas terminé l’export. Vérifiez l’encodeur, les pilotes et l’espace disque.\n\n%1")
                .arg(export_log_tail_.right(3000)));
    }
    export_cancelled_ = false;
}

void MainWindow::removeSelectedLine() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
    if (!document_ || lines_table_->currentRow() < 0) return;
    const int row = lines_table_->currentRow();
    if (QMessageBox::question(this, QStringLiteral("Supprimer la réplique"),
                              QStringLiteral("Supprimer la réplique sélectionnée ?")) != QMessageBox::Yes) return;
    try {
        document_->remove_line(document_->lines()[static_cast<std::size_t>(row)]);
        setDirty(true);
        refreshTable();
        status_label_->setText(QStringLiteral("Réplique supprimée."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Suppression impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::addRole() {
    if (!document_) return;
    bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Nouveau personnage"),
                                            QStringLiteral("Nom du personnage :"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QString id = name.toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    id.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    if (id.isEmpty()) id = QStringLiteral("role");
    QString base = id;
    int suffix = 2;
    auto exists = [&](const QString& candidate) {
        return std::any_of(document_->roles().begin(), document_->roles().end(),
                           [&](const Role& role) { return qstr(role.id) == candidate; });
    };
    while (exists(id)) id = base + QStringLiteral("_") + QString::number(suffix++);

    const auto& palette = studioRolePalette();
    const QString assigned_color = palette[document_->roles().size() % palette.size()];

    try {
        document_->add_role(utf8(id), utf8(name), utf8(assigned_color));
        setDirty(true);
        refreshTable();
        status_label_->setText(QStringLiteral("Personnage ajouté : ") + name);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Ajout impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::manageRoles() {
    if (!document_) {
        QMessageBox::information(this, QStringLiteral("Distribution"),
            QStringLiteral("Créez ou ouvrez d’abord un document DetX pour gérer ses personnages."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Distribution & Rôles"));
    dialog.setMinimumSize(500, 400);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(12);

    auto* header_title = new QLabel(QStringLiteral("Personnages du projet"), &dialog);
    header_title->setStyleSheet(QStringLiteral("font-size: 14pt; font-weight: 700; color: #F4F4F6;"));
    auto* header_sub = new QLabel(
        QStringLiteral("Personnalisez les noms et couleurs des rôles affichés sur la bande rythmo."),
        &dialog);
    header_sub->setWordWrap(true);
    header_sub->setStyleSheet(QStringLiteral("color: #8A8A93;"));
    layout->addWidget(header_title);
    layout->addWidget(header_sub);

    auto* list = new QListWidget(&dialog);
    auto populate = [this, list] {
        const QString selected_id = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
        list->clear();
        if (!document_) return;
        for (const auto& role : document_->roles()) {
            const auto usage = std::count_if(document_->lines().begin(), document_->lines().end(),
                                             [&](const Line& line) { return line.role_id == role.id; });
            const QString label = QStringLiteral("%1   ·   %2 %3")
                .arg(qstr(role.name))
                .arg(static_cast<qulonglong>(usage))
                .arg(usage > 1 ? QStringLiteral("répliques") : QStringLiteral("réplique"));
            auto* item = new QListWidgetItem(makeRoleSwatchIcon(displayRoleColor(role.color), 14), label, list);
            item->setData(Qt::UserRole, qstr(role.id));
            item->setToolTip(QStringLiteral("ID : %1 · Couleur : %2").arg(qstr(role.id), qstr(role.color)));
            if (!selected_id.isEmpty() && qstr(role.id) == selected_id) {
                list->setCurrentItem(item);
            }
        }
        if (list->count() > 0 && !list->currentItem()) list->setCurrentRow(0);
    };
    populate();
    layout->addWidget(list, 1);

    auto* edit_row = new QHBoxLayout;
    edit_row->setSpacing(8);
    auto* add = new QPushButton(QStringLiteral("Ajouter"), &dialog);
    add->setObjectName(QStringLiteral("primaryButton"));
    auto* color_btn = new QPushButton(QStringLiteral("Couleur…"), &dialog);
    auto* rename_btn = new QPushButton(QStringLiteral("Renommer…"), &dialog);
    auto* import_btn = new QPushButton(QStringLiteral("Importer d’un DetX…"), &dialog);
    import_btn->setObjectName(QStringLiteral("ghostButton"));
    auto* remove = new QPushButton(QStringLiteral("Supprimer"), &dialog);
    remove->setObjectName(QStringLiteral("dangerGhostButton"));
    edit_row->addWidget(add);
    edit_row->addWidget(color_btn);
    edit_row->addWidget(rename_btn);
    edit_row->addWidget(import_btn);
    edit_row->addStretch(1);
    edit_row->addWidget(remove);
    layout->addLayout(edit_row);

    auto* bottom_row = new QHBoxLayout;
    bottom_row->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("Fermer"), &dialog);
    close->setMinimumWidth(96);
    bottom_row->addWidget(close);
    layout->addWidget(new QFrame(&dialog));
    layout->addLayout(bottom_row);

    connect(add, &QPushButton::clicked, &dialog, [this, populate] {
        addRole();
        populate();
    });
    const auto pick_role_color = [this, &dialog, list, populate] {
        auto* item = list->currentItem();
        if (!item || !document_) return;
        const std::string id = utf8(item->data(Qt::UserRole).toString());
        const auto found = std::find_if(document_->roles().begin(), document_->roles().end(),
                                        [&](const Role& r) { return r.id == id; });
        if (found == document_->roles().end()) return;
        const QColor current = displayRoleColor(found->color);
        const QColor chosen = QColorDialog::getColor(current, &dialog, QStringLiteral("Couleur de « %1 »").arg(qstr(found->name)));
        if (!chosen.isValid()) return;
        const QString hex = chosen.name(QColor::HexRgb).toUpper();
        try {
            document_->set_role_color(id, utf8(hex));
            setDirty(true);
            refreshTable();
            populate();
            status_label_->setText(QStringLiteral("Couleur mise à jour pour « %1 ».").arg(qstr(found->name)));
        } catch (const std::exception& error) {
            QMessageBox::warning(&dialog, QStringLiteral("Couleur invalide"), QString::fromUtf8(error.what()));
        }
    };
    connect(color_btn, &QPushButton::clicked, &dialog, pick_role_color);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [pick_role_color](QListWidgetItem*) { pick_role_color(); });

    connect(rename_btn, &QPushButton::clicked, &dialog, [this, &dialog, list, populate] {
        auto* item = list->currentItem();
        if (!item || !document_) return;
        const std::string id = utf8(item->data(Qt::UserRole).toString());
        const auto found = std::find_if(document_->roles().begin(), document_->roles().end(),
                                        [&](const Role& r) { return r.id == id; });
        if (found == document_->roles().end()) return;
        bool ok = false;
        const QString next_name = QInputDialog::getText(&dialog, QStringLiteral("Renommer le personnage"),
            QStringLiteral("Nouveau nom :"), QLineEdit::Normal, qstr(found->name), &ok).trimmed();
        if (!ok || next_name.isEmpty()) return;
        try {
            document_->set_role_name(id, utf8(next_name));
            setDirty(true);
            refreshTable();
            populate();
            status_label_->setText(QStringLiteral("Personnage renommé en « %1 ».").arg(next_name));
        } catch (const std::exception& error) {
            QMessageBox::warning(&dialog, QStringLiteral("Renommage impossible"), QString::fromUtf8(error.what()));
        }
    });

    connect(import_btn, &QPushButton::clicked, &dialog, [this, &dialog, populate] {
        const QString source = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Importer les rôles d’un DetX"),
            {}, QStringLiteral("Documents DetX (*.detx *.DetX);;Tous les fichiers (*)"));
        if (source.isEmpty() || !document_) return;
        try {
            const auto other = DetxDocument::load(fs_path(source));
            const auto imported = document_->import_roles(other);
            setDirty(true);
            refreshTable();
            populate();
            status_label_->setText(QStringLiteral("%1 rôle(s) vérifié(s) ou importé(s) depuis %2.")
                .arg(static_cast<qulonglong>(imported.size())).arg(QFileInfo(source).fileName()));
        } catch (const std::exception& error) {
            QMessageBox::warning(&dialog, QStringLiteral("Import impossible"), QString::fromUtf8(error.what()));
        }
    });

    connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, list, populate] {
        auto* item = list->currentItem();
        if (!item || !document_) return;
        const std::string id = utf8(item->data(Qt::UserRole).toString());
        const auto found = std::find_if(document_->roles().begin(), document_->roles().end(),
                                        [&](const Role& r) { return r.id == id; });
        const QString role_display = found != document_->roles().end() ? qstr(found->name) : item->text();
        const auto usage = std::count_if(document_->lines().begin(), document_->lines().end(),
                                         [&](const Line& line) { return line.role_id == id; });
        if (usage > 0) {
            QMessageBox::warning(&dialog, QStringLiteral("Rôle utilisé"),
                QStringLiteral("Impossible de supprimer « %1 » : ce personnage est attribué à %2 réplique(s) de la bande rythmo.")
                    .arg(role_display).arg(static_cast<qulonglong>(usage)));
            return;
        }
        try {
            document_->remove_role(id);
            setDirty(true);
            refreshTable();
            populate();
            status_label_->setText(QStringLiteral("Personnage supprimé : %1").arg(role_display));
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
                status_label_->setText(QStringLiteral("Cadence vidéo détectée : %1 i/s · Projet DetX réglé sur %2 i/s.")
                                       .arg(detected, 0, 'f', 3).arg(nominal));
            } catch (const std::exception& error) {
                status_label_->setText(QStringLiteral("FPS vidéo détecté, mais impossible de l’appliquer : ") + QString::fromUtf8(error.what()));
            }
        } else if (document_) {
            const auto difference = std::abs(detected - document_->fps());
            status_label_->setText(QStringLiteral("Vidéo : %1 i/s · DetX : %2 i/s%3")
                .arg(detected, 0, 'f', 3).arg(document_->fps())
                .arg(document_->fps_is_assumed() ? QStringLiteral(" (cadence implicite)") : (difference > 0.1 ? QStringLiteral(" (cadences distinctes)") : QString{})));
        } else {
            status_label_->setText(QStringLiteral("Vidéo chargée · %1 i/s").arg(detected, 0, 'f', 3));
        }
    }
    updateEditorHeader();
    refreshTimeline();
    if (player_->playbackState() == QMediaPlayer::PlayingState) {
        updatePosition(player_->position());
    }
}

void MainWindow::updatePosition(qint64 position_ms) {
    if (!player_) return;
    current_position_ms_ = std::max<qint64>(0, position_ms);
    timeline_->setPlayhead(current_position_ms_);
    const qint64 duration = player_->duration();
    if (duration > 0) {
        const int permille = qBound(0, qRound(static_cast<double>(position_ms) * 1000.0 / duration), 1000);
        if (permille != last_slider_permille_) {
            last_slider_permille_ = permille;
            const QSignalBlocker blocker(position_slider_);
            position_slider_->setValue(permille);
        }
    }
    const auto tc = currentTimecode();
    if (!(tc == last_displayed_timecode_)) {
        last_displayed_timecode_ = tc;
        timecode_label_->setText(qstr(tc.format()));
    }
}

void MainWindow::togglePlayPause() {
    if (!player_) return;
    if (seek_coalesce_timer_ && seek_coalesce_timer_->isActive()) {
        seek_coalesce_timer_->stop();
    }
    if (pending_media_seek_ms_ >= 0) {
        const qint64 target = pending_media_seek_ms_;
        pending_media_seek_ms_ = -1;
        media_seek_in_flight_ = false;
        if (!decode_fallback_running_ && !player_->source().isEmpty()) {
            playback_seek_pending_ = true;
            last_dispatched_seek_ms_ = target;
            player_->setPosition(target);
        }
    }
    if (decode_fallback_running_ || decode_encoder_probe_process_) {
        fallback_resume_playing_ = !fallback_resume_playing_;
        fallback_resume_pos_ms_ = current_position_ms_;
        play_button_->setIcon(makeStudioIcon(
            fallback_resume_playing_ ? StudioIcon::Pause : StudioIcon::Play,
            QColor(QStringLiteral("#111113")), 15));
        return;
    }
    if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    else player_->play();
}

void MainWindow::dispatchMediaSeek(qint64 target, bool force_exact) {
    if (!player_) return;
    if (decode_fallback_running_ || player_->source().isEmpty() ||
        player_->mediaStatus() == QMediaPlayer::NoMedia ||
        player_->mediaStatus() == QMediaPlayer::LoadingMedia) {
        fallback_resume_pos_ms_ = target;
        pending_media_seek_ms_ = target;
        return;
    }
    if (player_->playbackState() == QMediaPlayer::StoppedState) {
        player_->pause();
    }
    const double fps = media_fps_ > 1.0 ? media_fps_ : static_cast<double>(std::max(1, timecode_fps_));
    const qint64 frame_idx = qRound64(static_cast<double>(target) * fps / 1000.0);

    // Skip redundant intra-frame decoder flushes while trackpad scrubbing within the same video frame!
    if (!force_exact && frame_idx == last_dispatched_seek_frame_) {
        pending_media_seek_ms_ = target;
        if (seek_coalesce_timer_) seek_coalesce_timer_->start(90);
        return;
    }

    const bool can_dispatch_now = force_exact ||
                                  !media_seek_in_flight_ ||
                                  !media_seek_flight_clock_.isValid() ||
                                  media_seek_flight_clock_.elapsed() >= 85;
    if (can_dispatch_now) {
        pending_media_seek_ms_ = -1;
        media_seek_in_flight_ = true;
        media_seek_flight_clock_.start();
        last_dispatched_seek_ms_ = target;
        last_dispatched_seek_frame_ = frame_idx;
        playback_seek_pending_ = true;
        player_->setPosition(target);
        if (seek_coalesce_timer_) seek_coalesce_timer_->start(90);
    } else {
        // Do NOT cancel the in-flight seek! Let videoFrameChanged finish its frame and immediately chain this target!
        pending_media_seek_ms_ = target;
        if (seek_coalesce_timer_ && !seek_coalesce_timer_->isActive()) {
            seek_coalesce_timer_->start(90);
        }
    }
}

void MainWindow::flushPendingMediaSeek() {
    media_seek_in_flight_ = false;
    if (!player_ || pending_media_seek_ms_ < 0) return;
    if (decode_fallback_running_ || player_->source().isEmpty() ||
        player_->mediaStatus() == QMediaPlayer::NoMedia ||
        player_->mediaStatus() == QMediaPlayer::LoadingMedia) {
        fallback_resume_pos_ms_ = pending_media_seek_ms_;
        return;
    }
    const qint64 target = pending_media_seek_ms_;
    pending_media_seek_ms_ = -1;
    if (target == last_dispatched_seek_ms_) return;
    if (player_->playbackState() == QMediaPlayer::StoppedState) {
        player_->pause();
    }
    const double fps = media_fps_ > 1.0 ? media_fps_ : static_cast<double>(std::max(1, timecode_fps_));
    media_seek_in_flight_ = true;
    media_seek_flight_clock_.start();
    last_dispatched_seek_ms_ = target;
    last_dispatched_seek_frame_ = qRound64(static_cast<double>(target) * fps / 1000.0);
    playback_seek_pending_ = true;
    player_->setPosition(target);
}

void MainWindow::seekTimeline(qint64 position_ms) {
    if (!player_) return;
    const qint64 target = std::max<qint64>(0, player_->duration() > 0
        ? std::min(position_ms, player_->duration()) : position_ms);
    playback_anchor_position_ms_ = target;
    playback_seek_pending_ = true;
    const bool is_playing = (player_->playbackState() == QMediaPlayer::PlayingState);
    if (is_playing) playback_clock_.restart();
    updatePosition(target);
    dispatchMediaSeek(target, is_playing);
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
        if (resizing_timeline_edge_ && dirty) {
            if (!dirty_) {
                dirty_ = true;
                const auto name = document_path_.isEmpty() ? QStringLiteral("Sans titre") : QFileInfo(document_path_).fileName();
                setWindowTitle(QStringLiteral("* ") + name + QStringLiteral(" | Rythmo by Dubia Productions"));
                updateEditorHeader();
            }
            return;
        }
        const std::string snapshot = document_->serialize();
        if (!undo_snapshot_initialized_) {
            current_document_snapshot_ = snapshot;
            undo_snapshot_initialized_ = true;
        } else if (!restoring_history_ && dirty && snapshot != current_document_snapshot_) {
            undo_history_.push_back(current_document_snapshot_);
            if (undo_history_.size() > 100) undo_history_.erase(undo_history_.begin());
            redo_history_.clear();
            current_document_snapshot_ = snapshot;
        } else {
            current_document_snapshot_ = snapshot;
        }
        if (!dirty) saved_document_snapshot_ = snapshot;
        dirty_ = snapshot != saved_document_snapshot_;
    } else {
        dirty_ = dirty;
    }
    const auto name = document_path_.isEmpty() ? QStringLiteral("Sans titre") : QFileInfo(document_path_).fileName();
    setWindowTitle((dirty_ ? QStringLiteral("* ") : QString{}) + name + QStringLiteral(" | Rythmo by Dubia Productions"));
    updateEditorHeader();
}

void MainWindow::resetUndoHistory(bool saved_state) {
    undo_history_.clear();
    redo_history_.clear();
    undo_snapshot_initialized_ = document_ != nullptr;
    current_document_snapshot_ = document_ ? document_->serialize() : std::string{};
    saved_document_snapshot_ = saved_state ? current_document_snapshot_ : std::string{};
    updateEditorHeader();
}

void MainWindow::undoLastChange() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
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
        updateEditorHeader();
        status_label_->setText(QStringLiteral("Dernière modification annulée."));
    } catch (const std::exception& error) {
        restoring_history_ = false;
        QMessageBox::warning(this, QStringLiteral("Annulation impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::redoLastChange() {
    if (timeline_ && timeline_->isEditingText()) timeline_->commitOrCancelEdit(true);
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
        updateEditorHeader();
        status_label_->setText(QStringLiteral("Modification rétablie."));
    } catch (const std::exception& error) {
        restoring_history_ = false;
        QMessageBox::warning(this, QStringLiteral("Rétablissement impossible"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (confirmDiscardChanges()) event->accept();
    else event->ignore();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const auto& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
        if (suffix == QStringLiteral("detx") || suffix == QStringLiteral("xml") || isSupportedVideoSuffix(suffix)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    QString dropped_detx;
    QString dropped_video;
    for (const auto& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() || !info.isFile()) continue;
        const QString suffix = info.suffix().toLower();
        if (dropped_detx.isEmpty() && (suffix == QStringLiteral("detx") || suffix == QStringLiteral("xml"))) {
            dropped_detx = info.absoluteFilePath();
        } else if (dropped_video.isEmpty() && isSupportedVideoSuffix(suffix)) {
            dropped_video = info.absoluteFilePath();
        }
    }
    if (!dropped_detx.isEmpty()) {
        if (!confirmDiscardChanges()) return;
        loadDocument(dropped_detx);
        if (!dropped_video.isEmpty() && document_) loadVideo(dropped_video);
        event->acceptProposedAction();
    } else if (!dropped_video.isEmpty()) {
        if (!document_) {
            document_ = std::make_unique<DetxDocument>(DetxDocument::create());
            resetUndoHistory(false);
            document_path_.clear();
            video_start_ = {};
            media_fps_ = 0.0;
            timecode_fps_ = 25;
            refreshTable();
            setDirty(true);
        }
        showEditor();
        loadVideo(dropped_video);
        event->acceptProposedAction();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type ev_type = event->type();

    // Fast O(1) rejection for the 99%+ of Qt events (Paint, Timer, MetaCall, UpdateRequest, etc.)
    switch (ev_type) {
    case QEvent::Resize:
        if (watched == this || watched == home_widget_) {
            if (home_widget_ && page_stack_ && page_stack_->currentWidget() == home_widget_) {
                rescaleHomePage();
            }
            if (projects_container_ && projects_widget_ && page_stack_ &&
                page_stack_->currentWidget() == projects_widget_) {
                projects_container_->setMaximumWidth(std::clamp(qRound(width() * 0.72), 820, 1360));
            }
        }
        if (full_screen_active_ && full_screen_host_ && full_screen_exit_button_ &&
            (watched == full_screen_host_ || watched == timeline_)) {
            positionFullscreenHud();
        }
        return QMainWindow::eventFilter(watched, event);

    case QEvent::Move:
    case QEvent::Show:
        if (full_screen_active_ && full_screen_host_ && full_screen_exit_button_ &&
            (watched == full_screen_host_ || watched == timeline_)) {
            positionFullscreenHud();
        }
        return QMainWindow::eventFilter(watched, event);

    case QEvent::MouseMove:
        if (full_screen_active_ && full_screen_host_ && full_screen_exit_button_) {
            const QPoint global_pos = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
            if (last_fullscreen_mouse_pos_.x() < 0) {
                last_fullscreen_mouse_pos_ = global_pos;
            } else if ((global_pos - last_fullscreen_mouse_pos_).manhattanLength() >= 4) {
                last_fullscreen_mouse_pos_ = global_pos;
                positionFullscreenHud();
                if (!full_screen_exit_button_->isVisible()) {
                    full_screen_exit_button_->show();
                    full_screen_exit_button_->raise();
                }
                if (full_screen_hud_timer_) full_screen_hud_timer_->start(2200);
            }
        }
        return QMainWindow::eventFilter(watched, event);

    case QEvent::DragEnter:
    case QEvent::Drop:
    case QEvent::KeyPress:
    case QEvent::ShortcutOverride:
        break;

    default:
        return false;
    }

    if ((ev_type == QEvent::DragEnter || ev_type == QEvent::Drop) &&
        watched && watched->objectName() == QStringLiteral("voiceDropZone")) {
        if (ev_type == QEvent::DragEnter) {
            auto* drag = static_cast<QDragEnterEvent*>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else {
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
                    item->setIcon(makeStudioIcon(StudioIcon::Film, QColor(QStringLiteral("#A1A1AA")), 15));
                    item->setToolTip(info.absoluteFilePath());
                    item->setData(Qt::UserRole, info.absoluteFilePath());
                }
                drop->acceptProposedAction();
                return true;
            }
        }
    }
    if (ev_type == QEvent::KeyPress && !QApplication::activeModalWidget()) {
        QWidget* focus = QApplication::focusWidget();
        QWidget* active_window = QApplication::activeWindow();
        const bool in_this_window = focus &&
            (focus == this || isAncestorOf(focus) ||
             (full_screen_host_ && (focus == full_screen_host_ || full_screen_host_->isAncestorOf(focus))));
        const bool active_rythmo_window = active_window == this || active_window == full_screen_host_;
        const bool editing_text = (timeline_ && timeline_->isEditingText()) ||
                                  qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) ||
                                  qobject_cast<QPlainTextEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                                  qobject_cast<QComboBox*>(focus);
        auto* key = static_cast<QKeyEvent*>(event);
        const auto navigation_modifiers = key->modifiers() &
            (Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier | Qt::ShiftModifier);
        const bool horizontal_arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right;
        if (in_this_window && active_rythmo_window && !editing_text && timeline_ && horizontal_arrow) {
            const int direction = key->key() == Qt::Key_Right ? 1 : -1;
            if (navigation_modifiers == Qt::NoModifier) {
                timeline_->navigateToAdjacentCue(direction);
                key->accept();
                return true;
            }
            if (navigation_modifiers == Qt::AltModifier || navigation_modifiers == Qt::ShiftModifier) {
                timeline_->stepFrame(direction);
                key->accept();
                return true;
            }
        }
        if (in_this_window && active_rythmo_window && !editing_text) {
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
        if (timeline_ && timeline_->isEditingText()) {
            const int k = key->key();
            if (k == Qt::Key_Space || k == Qt::Key_Delete || k == Qt::Key_Backspace ||
                k == Qt::Key_Escape || k == Qt::Key_Return || k == Qt::Key_Enter ||
                k == Qt::Key_Tab || k == Qt::Key_Backtab ||
                k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down ||
                k == Qt::Key_Home || k == Qt::Key_End ||
                key->matches(QKeySequence::SelectAll) || key->matches(QKeySequence::Copy) ||
                key->matches(QKeySequence::Cut) || key->matches(QKeySequence::Paste)) {
                event->accept();
                return false;
            }
        }
        if (key->key() == Qt::Key_Space) {
            QWidget* focus = QApplication::focusWidget();
            const bool editing_text = qobject_cast<QLineEdit*>(focus) ||
                                      qobject_cast<QTextEdit*>(focus) ||
                                      qobject_cast<QPlainTextEdit*>(focus) ||
                                      qobject_cast<QAbstractSpinBox*>(focus) ||
                                      qobject_cast<QComboBox*>(focus);
            if (editing_text) {
                event->accept();
                return false;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace rythmo
