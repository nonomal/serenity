/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Painter.h>
#include <LibWeb/Layout/InitialContainingBlock.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/GradientPainting.h>
#include <LibWeb/Painting/PaintContext.h>

namespace Web::Painting {

// https://www.w3.org/TR/css-backgrounds-3/#backgrounds
void paint_background(PaintContext& context, Layout::NodeWithStyleAndBoxModelMetrics const& layout_node, Gfx::FloatRect const& border_rect, Color background_color, Vector<CSS::BackgroundLayerData> const* background_layers, BorderRadiiData const& border_radii)
{
    auto& painter = context.painter();

    struct BackgroundBox {
        Gfx::FloatRect rect;
        BorderRadiiData radii;

        inline void shrink(float top, float right, float bottom, float left)
        {
            rect.shrink(top, right, bottom, left);
            radii.shrink(top, right, bottom, left);
        }
    };

    BackgroundBox border_box {
        border_rect,
        border_radii
    };

    auto get_box = [&](CSS::BackgroundBox box_clip) {
        auto box = border_box;
        switch (box_clip) {
        case CSS::BackgroundBox::ContentBox: {
            auto& padding = layout_node.box_model().padding;
            box.shrink(padding.top, padding.right, padding.bottom, padding.left);
            [[fallthrough]];
        }
        case CSS::BackgroundBox::PaddingBox: {
            auto& border = layout_node.box_model().border;
            box.shrink(border.top, border.right, border.bottom, border.left);
            [[fallthrough]];
        }
        case CSS::BackgroundBox::BorderBox:
        default:
            return box;
        }
    };

    auto color_box = border_box;
    if (background_layers && !background_layers->is_empty())
        color_box = get_box(background_layers->last().clip);

    auto layer_is_paintable = [&](auto& layer) {
        return (layer.background_image
            && ((layer.background_image->is_image() && layer.background_image->as_image().bitmap())
                || layer.background_image->is_linear_gradient()));
    };

    bool has_paintable_layers = false;
    if (background_layers) {
        for (auto& layer : *background_layers) {
            if (layer_is_paintable(layer)) {
                has_paintable_layers = true;
                break;
            }
        }
    }

    Gfx::AntiAliasingPainter aa_painter { painter };
    aa_painter.fill_rect_with_rounded_corners(color_box.rect.to_rounded<int>(),
        background_color, color_box.radii.top_left.as_corner(), color_box.radii.top_right.as_corner(), color_box.radii.bottom_right.as_corner(), color_box.radii.bottom_left.as_corner());

    if (!has_paintable_layers)
        return;

    // Note: Background layers are ordered front-to-back, so we paint them in reverse
    for (auto& layer : background_layers->in_reverse()) {
        // TODO: Gradients!
        if (!layer_is_paintable(layer))
            continue;
        Gfx::PainterStateSaver state { painter };

        // Clip
        auto clip_box = get_box(layer.clip);
        auto clip_rect = clip_box.rect.to_rounded<int>();
        painter.add_clip_rect(clip_rect);
        ScopedCornerRadiusClip corner_clip { painter, clip_rect, clip_box.radii };

        if (layer.background_image->is_linear_gradient()) {
            // FIXME: Support sizing and positioning rules with gradients.
            auto& linear_gradient = layer.background_image->as_linear_gradient();
            auto data = resolve_linear_gradient_data(layout_node, border_box.rect, linear_gradient);
            paint_linear_gradient(context, border_box.rect.to_rounded<int>(), data);
            continue;
        }

        auto& image = *layer.background_image->as_image().bitmap();
        Gfx::FloatRect background_positioning_area;

        // Attachment and Origin
        switch (layer.attachment) {
        case CSS::BackgroundAttachment::Fixed:
            background_positioning_area = layout_node.root().browsing_context().viewport_rect().to_type<float>();
            break;
        case CSS::BackgroundAttachment::Local:
        case CSS::BackgroundAttachment::Scroll:
            background_positioning_area = get_box(layer.origin).rect;
            break;
        }

        // Size
        Gfx::FloatRect image_rect;
        switch (layer.size_type) {
        case CSS::BackgroundSize::Contain: {
            float max_width_ratio = background_positioning_area.width() / image.width();
            float max_height_ratio = background_positioning_area.height() / image.height();
            float ratio = min(max_width_ratio, max_height_ratio);
            image_rect.set_size(image.width() * ratio, image.height() * ratio);
            break;
        }
        case CSS::BackgroundSize::Cover: {
            float max_width_ratio = background_positioning_area.width() / image.width();
            float max_height_ratio = background_positioning_area.height() / image.height();
            float ratio = max(max_width_ratio, max_height_ratio);
            image_rect.set_size(image.width() * ratio, image.height() * ratio);
            break;
        }
        case CSS::BackgroundSize::LengthPercentage: {
            float width;
            float height;
            bool x_is_auto = layer.size_x.is_length() && layer.size_x.length().is_auto();
            bool y_is_auto = layer.size_y.is_length() && layer.size_y.length().is_auto();
            if (x_is_auto && y_is_auto) {
                width = image.width();
                height = image.height();
            } else if (x_is_auto) {
                height = layer.size_y.resolved(layout_node, CSS::Length::make_px(background_positioning_area.height())).to_px(layout_node);
                width = image.width() * (height / image.height());
            } else if (y_is_auto) {
                width = layer.size_x.resolved(layout_node, CSS::Length::make_px(background_positioning_area.width())).to_px(layout_node);
                height = image.height() * (width / image.width());
            } else {
                width = layer.size_x.resolved(layout_node, CSS::Length::make_px(background_positioning_area.width())).to_px(layout_node);
                height = layer.size_y.resolved(layout_node, CSS::Length::make_px(background_positioning_area.height())).to_px(layout_node);
            }

            image_rect.set_size(width, height);
            break;
        }
        }

        // If background-repeat is round for one (or both) dimensions, there is a second step.
        // The UA must scale the image in that dimension (or both dimensions) so that it fits a
        // whole number of times in the background positioning area.
        if (layer.repeat_x == CSS::Repeat::Round || layer.repeat_y == CSS::Repeat::Round) {
            // If X ≠ 0 is the width of the image after step one and W is the width of the
            // background positioning area, then the rounded width X' = W / round(W / X)
            // where round() is a function that returns the nearest natural number
            // (integer greater than zero).
            if (layer.repeat_x == CSS::Repeat::Round) {
                image_rect.set_width(background_positioning_area.width() / roundf(background_positioning_area.width() / image_rect.width()));
            }
            if (layer.repeat_y == CSS::Repeat::Round) {
                image_rect.set_height(background_positioning_area.height() / roundf(background_positioning_area.height() / image_rect.height()));
            }

            // If background-repeat is round for one dimension only and if background-size is auto
            // for the other dimension, then there is a third step: that other dimension is scaled
            // so that the original aspect ratio is restored.
            if (layer.repeat_x != layer.repeat_y) {
                if (layer.size_x.is_length() && layer.size_x.length().is_auto()) {
                    image_rect.set_width(image.width() * (image_rect.height() / image.height()));
                }
                if (layer.size_y.is_length() && layer.size_y.length().is_auto()) {
                    image_rect.set_height(image.height() * (image_rect.width() / image.width()));
                }
            }
        }

        float space_x = background_positioning_area.width() - image_rect.width();
        float space_y = background_positioning_area.height() - image_rect.height();

        // Position
        float offset_x = layer.position_offset_x.resolved(layout_node, CSS::Length::make_px(space_x)).to_px(layout_node);
        if (layer.position_edge_x == CSS::PositionEdge::Right) {
            image_rect.set_right_without_resize(background_positioning_area.right() - offset_x);
        } else {
            image_rect.set_left(background_positioning_area.left() + offset_x);
        }

        float offset_y = layer.position_offset_y.resolved(layout_node, CSS::Length::make_px(space_y)).to_px(layout_node);
        if (layer.position_edge_y == CSS::PositionEdge::Bottom) {
            image_rect.set_bottom_without_resize(background_positioning_area.bottom() - offset_y);
        } else {
            image_rect.set_top(background_positioning_area.top() + offset_y);
        }

        // Repetition
        bool repeat_x = false;
        bool repeat_y = false;
        float x_step = 0;
        float y_step = 0;

        switch (layer.repeat_x) {
        case CSS::Repeat::Round:
            x_step = image_rect.width();
            repeat_x = true;
            break;
        case CSS::Repeat::Space: {
            int whole_images = background_positioning_area.width() / image_rect.width();
            if (whole_images <= 1) {
                x_step = image_rect.width();
                repeat_x = false;
            } else {
                float space = fmodf(background_positioning_area.width(), image_rect.width());
                x_step = image_rect.width() + ((float)space / (float)(whole_images - 1));
                repeat_x = true;
            }
            break;
        }
        case CSS::Repeat::Repeat:
            x_step = image_rect.width();
            repeat_x = true;
            break;
        case CSS::Repeat::NoRepeat:
            repeat_x = false;
            break;
        }
        // Move image_rect to the left-most tile position that is still visible
        if (repeat_x && image_rect.x() > clip_rect.x()) {
            auto x_delta = floorf(x_step * ceilf((image_rect.x() - clip_rect.x()) / x_step));
            image_rect.set_x(image_rect.x() - x_delta);
        }

        switch (layer.repeat_y) {
        case CSS::Repeat::Round:
            y_step = image_rect.height();
            repeat_y = true;
            break;
        case CSS::Repeat::Space: {
            int whole_images = background_positioning_area.height() / image_rect.height();
            if (whole_images <= 1) {
                y_step = image_rect.height();
                repeat_y = false;
            } else {
                float space = fmodf(background_positioning_area.height(), image_rect.height());
                y_step = image_rect.height() + ((float)space / (float)(whole_images - 1));
                repeat_y = true;
            }
            break;
        }
        case CSS::Repeat::Repeat:
            y_step = image_rect.height();
            repeat_y = true;
            break;
        case CSS::Repeat::NoRepeat:
            repeat_y = false;
            break;
        }
        // Move image_rect to the top-most tile position that is still visible
        if (repeat_y && image_rect.y() > clip_rect.y()) {
            auto y_delta = floorf(y_step * ceilf((image_rect.y() - clip_rect.y()) / y_step));
            image_rect.set_y(image_rect.y() - y_delta);
        }

        float initial_image_x = image_rect.x();
        float image_y = image_rect.y();
        Optional<Gfx::IntRect> last_int_image_rect;

        while (image_y < clip_rect.bottom()) {
            image_rect.set_y(image_y);

            float image_x = initial_image_x;
            while (image_x < clip_rect.right()) {
                image_rect.set_x(image_x);
                auto int_image_rect = image_rect.to_rounded<int>();
                if (int_image_rect != last_int_image_rect)
                    painter.draw_scaled_bitmap(int_image_rect, image, image.rect(), 1.0f, Gfx::Painter::ScalingMode::BilinearBlend);
                last_int_image_rect = int_image_rect;
                if (!repeat_x)
                    break;
                image_x += x_step;
            }

            if (!repeat_y)
                break;
            image_y += y_step;
        }
    }
}

}
