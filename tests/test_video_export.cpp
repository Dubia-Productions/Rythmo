#include "rythmo/video_export_filter.hpp"
#include "rythmo/video_backdrop.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QDebug>

static bool run(const QString& tool, const QStringList& args, QByteArray& output, int timeout = 60000) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(tool, args);
    if (!process.waitForStarted(3000) || !process.waitForFinished(timeout)) {
        process.kill(); process.waitForFinished(3000);
        output = process.readAll();
        return false;
    }
    output = process.readAll();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // A flat backdrop must keep its color; an edge must be softened rather
    // than merely resized, including tiny sources and portrait buffers.
    QImage solid(64, 32, QImage::Format_RGB32);
    solid.fill(qRgb(70,120,180));
    const auto flat = rythmo::softVideoBackdrop(solid, {160,90});
    if (flat.isNull() || flat.pixel(0,0) != qRgb(70,120,180) || flat.pixel(159,89) != qRgb(70,120,180)) {
        qCritical() << "Backdrop flat-color check failed" << flat.isNull()
                    << (flat.isNull() ? 0 : flat.pixel(0,0))
                    << (flat.isNull() ? 0 : flat.pixel(159,89));
        return 1;
    }
    QImage edge(160,90,QImage::Format_RGB32);
    edge.fill(Qt::black);
    for (int y=0;y<90;++y) for(int x=80;x<160;++x) edge.setPixel(x,y,qRgb(255,255,255));
    const auto blurred = rythmo::softVideoBackdrop(edge,edge.size());
    if (qRed(blurred.pixel(78,45)) <= 0 || qRed(blurred.pixel(81,45)) >= 255 ||
        blurred.pixel(0,45) != qRgb(0,0,0) || blurred.pixel(159,45) != qRgb(255,255,255)) {
        qCritical() << "Backdrop blur check failed" << qRed(blurred.pixel(78,45))
                    << qRed(blurred.pixel(81,45)) << blurred.pixel(0,45) << blurred.pixel(159,45);
        return 1;
    }
    QImage tiny(1,1,QImage::Format_RGB32); tiny.fill(Qt::red);
    if (rythmo::softVideoBackdrop(tiny,{1,1}).pixel(0,0) != qRgb(255,0,0)) return 1;
    qInfo() << "PASS backdrop: color preservation, true blur, clamped boundaries";

    // The same Qt-rendered effect layer is used by the fullscreen canvas and
    // export. Check that only the fitted frame's two narrow side strips add
    // shadow; the base dim/vignette layer remains identical.
    const auto shadow_overlay = rythmo::fullscreenVideoOverlay({1280,432},{16,9},true);
    const auto plain_overlay = rythmo::fullscreenVideoOverlay({1280,432},{16,9},false);
    if (shadow_overlay.isNull() || plain_overlay.isNull() || shadow_overlay.size() != QSize(1280,432)) return 1;
    QImage shadow_on_white(1280,432,QImage::Format_RGB32), plain_on_white(1280,432,QImage::Format_RGB32);
    shadow_on_white.fill(Qt::white); plain_on_white.fill(Qt::white);
    { QPainter p(&shadow_on_white); p.drawImage(0,0,shadow_overlay); }
    { QPainter p(&plain_on_white); p.drawImage(0,0,plain_overlay); }
    const auto shadow_delta = [&](int x) { return qRed(plain_on_white.pixel(x,216))-qRed(shadow_on_white.pixel(x,216)); };
    if (shadow_delta(255) < 8 || shadow_delta(1024) < 8 || shadow_delta(100) != 0 || shadow_delta(1180) != 0) {
        qCritical() << "Fullscreen shadow geometry regression" << shadow_delta(255) << shadow_delta(1024)
                    << shadow_delta(100) << shadow_delta(1180);
        return 1;
    }
    qInfo() << "PASS fullscreen/export share the narrow shadow and vignette overlay";

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    const QString ffprobe = QStandardPaths::findExecutable("ffprobe");
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) { qInfo() << "SKIP: FFmpeg/ffprobe unavailable"; return 77; }
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    QByteArray result;
    bool coreimage_available = false;
#ifdef Q_OS_MACOS
    const QSize probe_canvas(320,180);
    const QSize probe_backdrop = rythmo::fullscreenBackdropSize(probe_canvas);
    const QString probe_size = QStringLiteral("%1:%2").arg(probe_backdrop.width()).arg(probe_backdrop.height());
    const QString probe_chain = QStringLiteral("scale=%1:force_original_aspect_ratio=increase:flags=bilinear,crop=%1,"
        "format=rgba,coreimage=filter=CIGaussianBlur@inputRadius=%2,scale=320:180:flags=bilinear")
        .arg(probe_size).arg(rythmo::fullscreenBackdropBlurRadius(probe_backdrop.width()));
    coreimage_available = run(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i",
        "color=c=black:s=160x90:d=0.1","-vf",probe_chain,"-frames:v","1","-f","null","-"},result,10000);
#endif
    const bool ci_smoke = qEnvironmentVariableIsSet("CI");
    const bool filter_modes[] = {false, coreimage_available};
    const int filter_mode_count = coreimage_available ? 2 : 1;
    const auto verify = [&](const QString& output, QSize size, int frames) {
        if (!run(ffprobe, {"-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
            "stream=width,height,nb_read_frames,avg_frame_rate", "-of", "json", output}, result)) return false;
        const auto stream = QJsonDocument::fromJson(result).object()["streams"].toArray().at(0).toObject();
        return stream["width"].toInt() == size.width() && stream["height"].toInt() == size.height() &&
               stream["nb_read_frames"].toString().toInt() == frames && stream["avg_frame_rate"].toString() == "60/1";
    };
    const auto write_overlay = [&](QSize size) {
        const QSize canvas(size.width(), size.height()*3/5);
        const QString path = dir.filePath(QStringLiteral("overlay_%1x%2.rgba").arg(size.width()).arg(canvas.height()));
        const QImage pixels = rythmo::fullscreenVideoOverlay(canvas,{16,9}).convertToFormat(QImage::Format_RGBA8888);
        QFile file(path);
        if (pixels.isNull() || !file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char*>(pixels.constBits()), static_cast<qint64>(pixels.sizeInBytes())) !=
                static_cast<qint64>(pixels.sizeInBytes())) return QString();
        return path;
    };
    const auto inputs = [&](QSize size, const QString& source, const QString& overlay,
                            bool use_coreimage, bool include_effects = true) {
        const QSize canvas(size.width(), size.height()*3/5);
        QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", source};
        if (include_effects) {
            args << "-stream_loop" << "-1" << "-f" << "rawvideo" << "-pixel_format" << "rgba" << "-video_size"
                 << QStringLiteral("%1x%2").arg(canvas.width()).arg(canvas.height())
                 << "-framerate" << "60/1" << "-i" << overlay;
        }
        args << "-f" << "lavfi" << "-i"
             << QStringLiteral("testsrc2=s=%1x%2:r=60:d=0.2,format=rgba").arg(size.width()).arg(size.height()-size.height()*3/5)
             << "-filter_complex" << rythmo::videoExportFilter(size.width(), size.height()*3/5,
                                                                  "60/1", use_coreimage, include_effects)
             << "-map" << "[v]" << "-pix_fmt" << "yuv420p";
        return args;
    };
    // Also confirm the FFmpeg graph accepts and preserves the shared overlay.
    const auto red = [](const QByteArray& image,int x) { return static_cast<unsigned char>(image[(216*1280+x)*3]); };
    for (int mode=0; mode<filter_mode_count; ++mode) {
        QByteArray encoded_shadow, encoded_plain;
        for (bool enabled : {false, true}) {
            const auto overlay = rythmo::fullscreenVideoOverlay({1280,432},{16,9},enabled);
            const QString overlay_path = dir.filePath(QStringLiteral("%1-%2.rgba")
                .arg(filter_modes[mode] ? "coreimage" : "cpu", enabled ? "shadow" : "plain"));
            const QImage pixels = overlay.convertToFormat(QImage::Format_RGBA8888);
            QFile overlay_file(overlay_path);
            if (pixels.isNull() || !overlay_file.open(QIODevice::WriteOnly) ||
                overlay_file.write(reinterpret_cast<const char*>(pixels.constBits()),
                                   static_cast<qint64>(pixels.sizeInBytes())) !=
                    static_cast<qint64>(pixels.sizeInBytes()) || !overlay_file.flush()) return 1;
            overlay_file.close();
            QStringList args{"-nostdin","-v","error","-f","lavfi","-i","color=white:s=640x360:r=30:d=0.1",
                "-f","rawvideo","-pixel_format","rgba","-video_size","1280x432","-framerate","60/1","-i",overlay_path,
                "-f","lavfi","-i","color=black:s=1280x288:r=60:d=0.1",
                "-filter_complex",rythmo::videoExportFilter(1280,432,"60/1",filter_modes[mode]),"-map","[v]","-frames:v","1",
                "-pix_fmt","rgb24","-f","rawvideo","-"};
            if (!run(ffmpeg,args,result) || result.size()!=1280*720*3) {
                qCritical()<<"Fullscreen overlay export test failed"<<result.left(1000);
                return 1;
            }
            (enabled ? encoded_shadow : encoded_plain) = result;
        }
        const int left_shadow = red(encoded_plain,255) - red(encoded_shadow,255);
        const int right_shadow = red(encoded_plain,1024) - red(encoded_shadow,1024);
        if (left_shadow < 8 || right_shadow < 8 || std::abs(red(encoded_plain,100)-red(encoded_shadow,100)) > 1 ||
            std::abs(red(encoded_plain,1180)-red(encoded_shadow,1180)) > 1) {
            qCritical() << "Export shadow differs from fullscreen geometry" << filter_modes[mode] << left_shadow << right_shadow;
            return 1;
        }
    }
    qInfo() << "PASS FFmpeg composites the exact fullscreen effect overlay";
    // No frame/duration limit: also catches frozen shadow streams preventing EOF.
    const QSize sizes[]{{1280,720},{1920,1080},{2560,1440},{3840,2160}};
    int index = 0;
    for (const auto& size : sizes) {
        const QString source = index++ % 2 ? "testsrc2=s=180x320:r=30:d=0.067" : "testsrc2=s=640x360:r=30:d=0.067";
        const QString overlay = write_overlay(size);
        if (overlay.isEmpty()) return 1;
        for (int mode=0; mode<filter_mode_count; ++mode) {
            const int effect_mode_count = ci_smoke && size != sizes[0] ? 1 : 2;
            for (int effect_index = 0; effect_index < effect_mode_count; ++effect_index) {
                const bool include_effects = effect_mode_count == 1 || effect_index == 1;
                auto args = inputs(size, source, overlay, filter_modes[mode], include_effects);
                const QString file = dir.filePath(QStringLiteral("filter-%1-%2-%3.mkv")
                    .arg(size.width()).arg(mode).arg(include_effects ? "effects" : "plain"));
                if (ci_smoke) {
                    args << "-frames:v" << "1" << "-f" << "null" << "-";
                    if (!run(ffmpeg, args, result)) { qCritical() << size << include_effects << result; return 1; }
                } else {
                    args << "-frames:v" << "2" << "-c:v" << "ffv1" << file;
                    if (!run(ffmpeg, args, result) || !verify(file, size, 2)) {
                        qCritical() << size << include_effects << result;
                        return 1;
                    }
                }
            }
        }
        qInfo() << "PASS filter, portrait/landscape, EOF, 60fps:" << size;
    }
    const QString eof_overlay = write_overlay({1280,720});
    auto eof_args = inputs({1280,720}, "testsrc2=s=640x360:r=30:d=0.067", eof_overlay, false, true);
    const QString eof_file = dir.filePath("eof-loop.mkv");
    eof_args << "-c:v" << "ffv1" << eof_file;
    if (!run(ffmpeg, eof_args, result) || !verify(eof_file, {1280,720}, 4)) { qCritical() << "EOF/loop export test failed" << result; return 1; }
    qInfo() << "PASS overlay loop stops at source EOF with 60fps output";

    // Exercise real installed engines. Unavailable GPU drivers must fail the
    // same real-encode probe as the menu, even if FFmpeg lists that encoder.
    QStringList encoders{"libx264", "libx265", "libsvtav1", "libaom-av1", "h264_nvenc", "hevc_nvenc", "av1_nvenc",
        "h264_qsv", "hevc_qsv", "av1_qsv", "h264_amf", "hevc_amf", "av1_amf", "h264_videotoolbox", "hevc_videotoolbox"};
    if (ci_smoke) encoders = {"libx264", "libx265", "libsvtav1", "libaom-av1"};
    const QSize codec_size = ci_smoke ? QSize(320,180) : QSize(1280,720);
    const QString codec_source = QStringLiteral("testsrc2=s=%1x%2:r=30:d=0.067")
        .arg(codec_size.width()/2).arg(codec_size.height()/2);
    const int codec_frames = ci_smoke ? 1 : 2;
    int passed = 0;
    for (const auto& encoder : encoders) {
        const QString probe_source = QStringLiteral("color=s=%1x%2:r=60")
            .arg(codec_size.width()).arg(codec_size.height());
        QStringList probe{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", probe_source,
            "-frames:v", QString::number(codec_frames), "-pix_fmt", "yuv420p"};
        probe += rythmo::videoExportCodecArguments(encoder,codec_size.width(),codec_size.height());
        probe << dir.filePath("probe.mp4");
        if (!run(ffmpeg, probe, result, 30000)) { qInfo() << "UNAVAILABLE (hidden):" << encoder; continue; }
        const QString overlay = write_overlay(codec_size);
        if (overlay.isEmpty()) return 1;
        auto args = inputs(codec_size, codec_source, overlay, coreimage_available);
        args += rythmo::videoExportCodecArguments(encoder,codec_size.width(),codec_size.height());
        const QString file = dir.filePath("codec.mp4");
        args << "-frames:v" << QString::number(codec_frames) << file;
        if (!run(ffmpeg,args,result,180000) || !verify(file,codec_size,codec_frames)) { qCritical() << encoder << result; return 1; }
        ++passed; qInfo() << "PASS encoded output:" << encoder;
        if (ci_smoke) break;
    }
    return passed ? 0 : 1;
}
