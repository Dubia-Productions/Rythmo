#pragma once

#include "rythmo/video_backdrop.hpp"

#include <QString>
#include <QStringList>
#include <algorithm>

namespace rythmo {

inline QStringList videoExportCodecArguments(const QString& encoder, int width = 1920, int height = 1080) {
    QStringList args{QStringLiteral("-c:v"), encoder};
    if (encoder == "libx264") {
#ifdef Q_OS_WIN
        args << "-preset" << "fast" << "-crf" << "20";
#else
        args << "-preset" << "medium" << "-crf" << "20";
#endif
    } else if (encoder == "libx265") {
#ifdef Q_OS_WIN
        args << "-preset" << "fast" << "-crf" << "24";
#else
        args << "-preset" << "medium" << "-crf" << "24";
#endif
    } else if (encoder == "libsvtav1") {
#ifdef Q_OS_WIN
        args << "-preset" << "8" << "-crf" << "30";
#else
        args << "-preset" << "6" << "-crf" << "30";
#endif
    } else if (encoder == "libaom-av1") {
        args << "-crf" << "30" << "-b:v" << "0" << "-row-mt" << "1" << "-cpu-used" << "6";
    } else if (encoder.contains("nvenc")) {
#ifdef Q_OS_WIN
        args << "-preset" << "p4" << "-cq:v" << "23" << "-b:v" << "0";
#else
        args << "-preset" << "p5" << "-cq:v" << "23" << "-b:v" << "0";
#endif
    } else if (encoder.contains("_qsv")) {
#ifdef Q_OS_WIN
        args << "-preset" << "veryfast" << "-look_ahead" << "0" << "-async_depth" << "4"
             << "-global_quality" << "23";
#else
        args << "-global_quality" << "23";
#endif
    } else if (encoder.contains("_amf")) {
#ifdef Q_OS_WIN
        args << "-quality" << "speed";
#endif
        args << "-rc" << "cqp" << "-qp_i" << "23" << "-qp_p" << "23";
    }
    else if (encoder.contains("videotoolbox")) {
        // Size the bitrate with the output pixel count so 4K60 stays clean.
        // VideoToolbox is explicitly required; it must not silently fall back
        // to a software encoder when hardware encoding is unavailable.
        const qint64 reference_pixels = 1920LL * 1080LL;
        const qint64 output_pixels = static_cast<qint64>(width) * height;
        const qint64 reference_kbps = encoder.startsWith(QStringLiteral("hevc_")) ? 12000 : 22000;
        const qint64 bitrate_kbps = std::max<qint64>(6000,
            (reference_kbps * output_pixels + reference_pixels / 2) / reference_pixels);
        args << "-allow_sw" << "0" << "-realtime" << "1"
             << "-b:v" << QStringLiteral("%1k").arg(bitrate_kbps);
    }
    return args;
}

// Shared by the export pipeline and its real FFmpeg integration tests.
inline QString videoExportFilter(int width, int video_height,
                                 const QString& fps_rational, bool use_coreimage = false,
                                 bool include_effects = true) {
    const QString video_size = QStringLiteral("%1:%2").arg(width).arg(video_height);
    if (!include_effects) {
        return QStringLiteral(
            "[0:v]scale=%1:force_original_aspect_ratio=decrease,format=rgba,"
            "pad=%1:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format=yuv420p,fps=%2[v0];"
            "[1:v]format=yuv420p[v1];[v0][v1]vstack=inputs=2:shortest=1[v]")
            .arg(video_size, fps_rational);
    }
    const QSize background_size = fullscreenBackdropSize(QSize(width, video_height));
    const QString background_video_size = QStringLiteral("%1:%2")
        .arg(background_size.width()).arg(background_size.height());
    const int blur_radius = fullscreenBackdropBlurRadius(background_size.width());
    // Match FullscreenVideoCanvas: same one-third-resolution center crop,
    // shared blur radius, then bilinear enlargement. CoreImage keeps the Mac
    // GPU fast path; the portable box blur uses the same three-pass radius.
    // Both share the exact Qt-rendered dim/vignette/shadow overlay.
    const QString background_fit = QStringLiteral("scale=%1:force_original_aspect_ratio=increase:flags=bilinear,crop=%1")
        .arg(background_video_size);
    const QString background_effects = use_coreimage
        ? background_fit + QStringLiteral(",format=rgba,coreimage=filter=CIGaussianBlur@inputRadius=")
            + QString::number(blur_radius) + QStringLiteral(",scale=") + video_size + QStringLiteral(":flags=bilinear")
        : background_fit + QStringLiteral(",format=gbrp,boxblur=luma_radius=") + QString::number(blur_radius)
            + QStringLiteral(":luma_power=3:chroma_radius=") + QString::number(blur_radius)
            + QStringLiteral(":chroma_power=3,scale=") + video_size + QStringLiteral(":flags=bilinear");
    return QStringLiteral(
        // Keep the expensive video filters at the source cadence; upsample only
        // the completed picture to 60 fps before stacking the 60 fps rhythm band.
        "[0:v]split=2[bgsrc][fgsrc];"
        "[bgsrc]%3[bgblur];"
        "[bgblur][1:v]overlay=x=0:y=0:format=auto:shortest=1[bg];"
        "[fgsrc]scale=%2:force_original_aspect_ratio=decrease,format=rgba,"
        "pad=%2:(ow-iw)/2:(oh-ih)/2:color=black@0[fg];"
        "[bg][fg]overlay=x=0:y=0:format=auto,setsar=1,format=yuv420p,fps=%1[v0];"
        "[2:v]format=yuv420p[v1];[v0][v1]vstack=inputs=2:shortest=1[v]")
        .arg(fps_rational, video_size, background_effects);
}

} // namespace rythmo
