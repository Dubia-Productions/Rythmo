#pragma once
#include <QColor>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPointF>
#include <QRadialGradient>
#include <QRect>
#include <QSize>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace rythmo {
inline QSize fullscreenBackdropSize(QSize canvas, qreal device_pixel_ratio = 1.0) {
    if (canvas.isEmpty()) return {};
    const int target_width = std::clamp(qRound(canvas.width() * device_pixel_ratio / 3.0), 160, 1280);
    return {target_width, std::max(1, qRound(target_width * canvas.height() / static_cast<double>(canvas.width())))};
}

inline int fullscreenBackdropBlurRadius(int target_width) {
    return std::clamp(target_width / 110, 2, 16);
}

// Shared by the fullscreen canvas and video export so dimming, vignette,
// fitted-frame geometry, and the narrow side shadows cannot drift apart.
inline QImage fullscreenVideoOverlay(QSize canvas, QSize source, bool include_shadow = true) {
    if (canvas.isEmpty()) return {};
    QImage overlay(canvas, QImage::Format_ARGB32_Premultiplied);
    if (overlay.isNull()) return {};
    overlay.fill(Qt::transparent);
    QPainter painter(&overlay);
    painter.fillRect(QRect(QPoint(0, 0), canvas), QColor(0, 0, 0, 78));
    QRadialGradient vignette(QPointF(canvas.width() / 2.0, canvas.height() / 2.0),
                             std::max(canvas.width(), canvas.height()) * 0.76);
    vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
    vignette.setColorAt(0.72, QColor(0, 0, 0, 20));
    vignette.setColorAt(1.0, QColor(0, 0, 0, 112));
    painter.fillRect(QRect(QPoint(0, 0), canvas), vignette);

    if (include_shadow && !source.isEmpty()) {
        const qreal scale = std::min(canvas.width() / static_cast<qreal>(source.width()),
                                     canvas.height() / static_cast<qreal>(source.height()));
        const QSize fitted(qRound(source.width() * scale), qRound(source.height() * scale));
        const QRect frame_rect((canvas.width() - fitted.width()) / 2,
                               (canvas.height() - fitted.height()) / 2,
                               fitted.width(), fitted.height());
        const int shadow_width = std::max(12, canvas.width() / 100);
        QLinearGradient left_shadow(frame_rect.left() - shadow_width, 0,
                                    frame_rect.left(), 0);
        left_shadow.setColorAt(0.0, QColor(0, 0, 0, 0));
        left_shadow.setColorAt(1.0, QColor(0, 0, 0, 105));
        painter.fillRect(QRect(frame_rect.left() - shadow_width, frame_rect.top(),
                               shadow_width, frame_rect.height()), left_shadow);
        QLinearGradient right_shadow(frame_rect.right(), 0,
                                     frame_rect.right() + shadow_width, 0);
        right_shadow.setColorAt(0.0, QColor(0, 0, 0, 105));
        right_shadow.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.fillRect(QRect(frame_rect.right(), frame_rect.top(),
                               shadow_width, frame_rect.height()), right_shadow);
    }
    painter.end();
    return overlay;
}

// Three sliding box passes approximate a Gaussian. Cost is independent of
// radius; RGB32 avoids alpha/premultiplication ambiguity for the opaque backdrop.
inline QImage softVideoBackdrop(const QImage& source, QSize target) {
    if (source.isNull() || target.isEmpty()) return {};
    QImage scaled = source.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QImage image = scaled.copy((scaled.width()-target.width())/2,
                              (scaled.height()-target.height())/2,
                              target.width(), target.height()).convertToFormat(QImage::Format_RGB32);
    QImage scratch(target, QImage::Format_RGB32);
    if (image.isNull() || scratch.isNull()) return {};
    const int radius = fullscreenBackdropBlurRadius(target.width());
    const int divisor = radius*2+1;
    for (int pass=0; pass<3; ++pass) {
        for (int axis=0; axis<2; ++axis) {
            const int length = axis == 0 ? image.width() : image.height();
            const int lines = axis == 0 ? image.height() : image.width();
            const auto* input = reinterpret_cast<const QRgb*>(image.constBits());
            auto* output = reinterpret_cast<QRgb*>(scratch.bits());
            const int stride = image.bytesPerLine()/4;
            const int out_stride = scratch.bytesPerLine()/4;
            for (int line=0; line<lines; ++line) {
                const int base = axis == 0 ? line*stride : line;
                const int step = axis == 0 ? 1 : stride;
                const int out_base = axis == 0 ? line*out_stride : line;
                const int out_step = axis == 0 ? 1 : out_stride;
                int red=0, green=0, blue=0;
                for (int k=-radius; k<=radius; ++k) {
                    const QRgb pixel = input[base + std::clamp(k,0,length-1)*step];
                    red+=qRed(pixel); green+=qGreen(pixel); blue+=qBlue(pixel);
                }
                for (int i=0; i<length; ++i) {
                    output[out_base+i*out_step] = qRgb((red+divisor/2)/divisor,
                        (green+divisor/2)/divisor, (blue+divisor/2)/divisor);
                    const QRgb leave = input[base+std::clamp(i-radius,0,length-1)*step];
                    const QRgb enter = input[base+std::clamp(i+radius+1,0,length-1)*step];
                    red+=qRed(enter)-qRed(leave); green+=qGreen(enter)-qGreen(leave); blue+=qBlue(enter)-qBlue(leave);
                }
            }
            image.swap(scratch);
        }
    }
    return image;
}
} // namespace rythmo
