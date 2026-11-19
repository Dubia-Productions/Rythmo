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
#include <cstdint>

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

// Three sliding box passes approximate a Gaussian. Uses two-stage decimation +
// cache-friendly transpose + fixed-point reciprocal so low-end x86/ARM CPUs avoid
// full-4K SmoothTransformation and per-pixel integer division.
inline QImage softVideoBackdrop(const QImage& source, QSize target) {
    if (source.isNull() || target.isEmpty()) return {};
    QImage image;
    if (source.size() == target && source.format() == QImage::Format_RGB32) {
        image = source.copy();
    } else {
        QImage pre_scaled = source;
        if (source.width() > target.width() * 3 || source.height() > target.height() * 3) {
            pre_scaled = source.scaled(target * 2, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
        }
        QImage scaled = pre_scaled.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        image = scaled.copy((scaled.width() - target.width()) / 2,
                            (scaled.height() - target.height()) / 2,
                            target.width(), target.height()).convertToFormat(QImage::Format_RGB32);
    }
    if (image.isNull()) return {};

    const int w = image.width();
    const int h = image.height();
    QImage transposed(h, w, QImage::Format_RGB32);
    if (transposed.isNull()) return {};

    const int radius = fullscreenBackdropBlurRadius(w);
    const std::uint32_t divisor = static_cast<std::uint32_t>(radius * 2 + 1);
    const std::uint32_t inv_mul = ((1u << 22) + divisor - 1u) / divisor;
    const std::uint32_t round_bias = divisor / 2u;

    // Blur horizontally AND transpose into output so BOTH axes read sequentially in L1 cache!
    auto blur_and_transpose = [radius, inv_mul, round_bias](const QImage& src, QImage& dst) {
        const int length = src.width();
        const int lines = src.height();
        const int dst_stride = dst.bytesPerLine() / 4;
        auto* dst_bits = reinterpret_cast<QRgb*>(dst.bits());
        for (int y = 0; y < lines; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
            std::uint32_t r = 0, g = 0, b = 0;
            for (int k = -radius; k <= radius; ++k) {
                const QRgb px = row[std::clamp(k, 0, length - 1)];
                r += static_cast<std::uint32_t>(qRed(px));
                g += static_cast<std::uint32_t>(qGreen(px));
                b += static_cast<std::uint32_t>(qBlue(px));
            }
            QRgb* out_col = dst_bits + y;
            for (int x = 0; x < length; ++x) {
                const int rr = std::min(255, static_cast<int>(((r + round_bias) * inv_mul) >> 22));
                const int gg = std::min(255, static_cast<int>(((g + round_bias) * inv_mul) >> 22));
                const int bb = std::min(255, static_cast<int>(((b + round_bias) * inv_mul) >> 22));
                out_col[x * dst_stride] = qRgb(rr, gg, bb);
                const QRgb leave = row[std::clamp(x - radius, 0, length - 1)];
                const QRgb enter = row[std::clamp(x + radius + 1, 0, length - 1)];
                r = r + static_cast<std::uint32_t>(qRed(enter)) - static_cast<std::uint32_t>(qRed(leave));
                g = g + static_cast<std::uint32_t>(qGreen(enter)) - static_cast<std::uint32_t>(qGreen(leave));
                b = b + static_cast<std::uint32_t>(qBlue(enter)) - static_cast<std::uint32_t>(qBlue(leave));
            }
        }
    };

    for (int pass = 0; pass < 3; ++pass) {
        blur_and_transpose(image, transposed);
        blur_and_transpose(transposed, image);
    }
    return image;
}
} // namespace rythmo
