#include "render/rml_render_interface_skia.hpp"

#include "render/skia_device.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Log.h>

#include <core/SkBlendMode.h>
#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkColorFilter.h>
#include <core/SkData.h>
#include <core/SkImage.h>
#include <core/SkImageInfo.h>
#include <core/SkM44.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkRect.h>
#include <core/SkSamplingOptions.h>
#include <core/SkShader.h>
#include <core/SkSurface.h>
#include <core/SkVertices.h>
#include <effects/SkColorMatrix.h>
#include <effects/SkGradient.h>
#include <effects/SkImageFilters.h>
#include <pathops/SkPathOps.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace tokmon::desk {
namespace {

template <typename Color>
SkColor to_sk_color(const Color& color) {
  return SkColorSetARGB(color.alpha, color.red, color.green, color.blue);
}

template <typename Color>
SkColor4f to_sk_color4f(const Color& color) {
  constexpr float scale = 1.f / 255.f;
  return {color.red * scale, color.green * scale, color.blue * scale,
          color.alpha * scale};
}

SkColor to_sk_vertex_color(const Rml::ColourbPremultiplied& color) {
  // RmlUi deliberately emits premultiplied vertex colours, while SkVertices
  // accepts SkColor in non-premultiplied form and performs its own conversion.
  // Passing the Rml channels through directly double-premultiplies translucent
  // RCSS colours, making pills and overlays far too dark.
  return to_sk_color(color.ToNonPremultiplied());
}

SkColor4f to_sk_gradient_color(const Rml::ColourbPremultiplied& color) {
  return to_sk_color4f(color.ToNonPremultiplied());
}

SkM44 to_sk_matrix(const Rml::Matrix4f& source) {
  const auto r0 = source.GetRow(0);
  const auto r1 = source.GetRow(1);
  const auto r2 = source.GetRow(2);
  const auto r3 = source.GetRow(3);
  return SkM44(r0[0], r0[1], r0[2], r0[3],
               r1[0], r1[1], r1[2], r1[3],
               r2[0], r2[1], r2[2], r2[3],
               r3[0], r3[1], r3[2], r3[3]);
}

Rml::Vector2f map_point(const Rml::Matrix4f* transform,
                        Rml::Vector2f point) {
  if (!transform)
    return point;
  const auto mapped = *transform * Rml::Vector4f(point.x, point.y, 0.f, 1.f);
  const float inverse_w = std::abs(mapped.w) > 0.00001f ? 1.f / mapped.w : 1.f;
  return {mapped.x * inverse_w, mapped.y * inverse_w};
}

SkRect to_sk_rect(const Rml::Rectanglei& value) {
  return SkRect::MakeLTRB(static_cast<float>(value.Left()),
                          static_cast<float>(value.Top()),
                          static_cast<float>(value.Right()),
                          static_cast<float>(value.Bottom()));
}

} // namespace

struct RmlRenderInterfaceSkia::Geometry {
  std::vector<SkPoint> positions;
  std::vector<SkPoint> texcoords;
  std::vector<SkColor> colors;
  std::vector<std::uint16_t> indices;
};

struct RmlRenderInterfaceSkia::Texture {
  int width{0};
  int height{0};
  sk_sp<SkImage> image;
};

struct RmlRenderInterfaceSkia::Layer {
  sk_sp<SkSurface> surface;
  SkPath clip_mask;
  bool has_clip_mask{false};
};

enum class FilterType { Opacity, Blur, DropShadow, ColorMatrix, MaskImage };

struct RmlRenderInterfaceSkia::Filter {
  FilterType type{FilterType::Opacity};
  float value{1.f};
  float sigma{0.f};
  Rml::Vector2f offset{};
  SkColor color{SK_ColorTRANSPARENT};
  SkColorMatrix color_matrix;
  sk_sp<SkImage> mask;
};

enum class ShaderType { Linear, Radial, Conic, Creation };

struct RmlRenderInterfaceSkia::Shader {
  ShaderType type{ShaderType::Linear};
  bool repeating{false};
  Rml::Vector2f p{};
  Rml::Vector2f v{1.f, 1.f};
  float angle{0.f};
  std::vector<float> positions;
  std::vector<SkColor4f> colors;
};

RmlRenderInterfaceSkia::RmlRenderInterfaceSkia(SkiaDevice& device)
    : device_(device) {}

RmlRenderInterfaceSkia::~RmlRenderInterfaceSkia() = default;

SkCanvas* RmlRenderInterfaceSkia::current_canvas() const {
  if (!layer_stack_.empty() && layer_stack_.back()->surface)
    return layer_stack_.back()->surface->getCanvas();
  return device_.surface() ? device_.surface()->getCanvas() : nullptr;
}

RmlRenderInterfaceSkia::Layer*
RmlRenderInterfaceSkia::find_layer(const Rml::LayerHandle handle) const {
  return handle ? reinterpret_cast<Layer*>(handle) : nullptr;
}

void RmlRenderInterfaceSkia::configure_canvas(
    SkCanvas& canvas, const bool include_transform,
    const Rml::Vector2f translation) const {
  if (scissor_enabled_)
    canvas.clipRect(to_sk_rect(scissor_), SkClipOp::kIntersect, false);
  const Layer* layer = layer_stack_.empty()
                           ? (!layers_.empty() && !layers_.front()->surface
                                  ? layers_.front().get()
                                  : nullptr)
                           : layer_stack_.back();
  if (clip_mask_enabled_ && layer && layer->has_clip_mask)
    canvas.clipPath(layer->clip_mask, SkClipOp::kIntersect, true);
  if (include_transform && transform_)
    canvas.concat(to_sk_matrix(*transform_));
  canvas.translate(translation.x, translation.y);
}

Rml::CompiledGeometryHandle RmlRenderInterfaceSkia::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
  auto geometry = std::make_unique<Geometry>();
  geometry->positions.reserve(vertices.size());
  geometry->texcoords.reserve(vertices.size());
  geometry->colors.reserve(vertices.size());
  for (const auto& vertex : vertices) {
    geometry->positions.push_back({vertex.position.x, vertex.position.y});
    geometry->texcoords.push_back({vertex.tex_coord.x, vertex.tex_coord.y});
    geometry->colors.push_back(to_sk_vertex_color(vertex.colour));
  }
  geometry->indices.reserve(indices.size());
  for (const int index : indices)
    geometry->indices.push_back(
        static_cast<std::uint16_t>(std::clamp(index, 0, 65535)));
  return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void RmlRenderInterfaceSkia::draw_geometry(
    Geometry& geometry, const Rml::Vector2f translation, Texture* texture,
    SkShader* override_shader) {
  auto* canvas = current_canvas();
  if (!canvas || geometry.positions.empty())
    return;

  canvas->save();
  configure_canvas(*canvas, true, translation);
  std::vector<SkPoint> texture_coordinates = geometry.texcoords;
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SK_ColorWHITE);
  if (override_shader) {
    paint.setShader(sk_ref_sp(override_shader));
  } else if (texture && texture->image) {
    for (auto& point : texture_coordinates) {
      point.fX *= static_cast<float>(texture->width);
      point.fY *= static_cast<float>(texture->height);
    }
    paint.setShader(texture->image->makeShader(
        SkTileMode::kClamp, SkTileMode::kClamp,
        SkSamplingOptions(SkFilterMode::kLinear)));
  }
  const auto vertices = SkVertices::MakeCopy(
      SkVertices::kTriangles_VertexMode,
      static_cast<int>(geometry.positions.size()), geometry.positions.data(),
      texture ? texture_coordinates.data() : nullptr, geometry.colors.data(),
      static_cast<int>(geometry.indices.size()), geometry.indices.data());
  if (vertices)
    canvas->drawVertices(vertices, SkBlendMode::kModulate, paint);
  canvas->restore();
}

void RmlRenderInterfaceSkia::RenderGeometry(
    const Rml::CompiledGeometryHandle handle, const Rml::Vector2f translation,
    const Rml::TextureHandle texture_handle) {
  auto* geometry = reinterpret_cast<Geometry*>(handle);
  if (geometry)
    draw_geometry(*geometry, translation,
                  reinterpret_cast<Texture*>(texture_handle), nullptr);
}

void RmlRenderInterfaceSkia::ReleaseGeometry(
    const Rml::CompiledGeometryHandle handle) {
  delete reinterpret_cast<Geometry*>(handle);
}

Rml::TextureHandle RmlRenderInterfaceSkia::LoadTexture(
    Rml::Vector2i& dimensions, const Rml::String& source) {
  dimensions = {0, 0};
  if (source.empty())
    return {};
  const auto data = SkData::MakeFromFileName(source.c_str());
  if (!data)
    return {};
  auto image = SkImages::DeferredFromEncodedData(data);
  if (!image)
    return {};
  auto texture = std::make_unique<Texture>();
  texture->width = image->width();
  texture->height = image->height();
  dimensions = {texture->width, texture->height};
  texture->image = std::move(image);
  return reinterpret_cast<Rml::TextureHandle>(texture.release());
}

Rml::TextureHandle RmlRenderInterfaceSkia::GenerateTexture(
    const Rml::Span<const Rml::byte> source, const Rml::Vector2i dimensions) {
  if (dimensions.x <= 0 || dimensions.y <= 0 || source.empty())
    return {};
  auto texture = std::make_unique<Texture>();
  texture->width = dimensions.x;
  texture->height = dimensions.y;
  const auto data = SkData::MakeWithCopy(source.data(), source.size());
  const auto info = SkImageInfo::Make(dimensions.x, dimensions.y,
                                      kRGBA_8888_SkColorType,
                                      kPremul_SkAlphaType);
  texture->image = SkImages::RasterFromData(
      info, data, static_cast<std::size_t>(dimensions.x) * 4u);
  if (!texture->image)
    return {};
  return reinterpret_cast<Rml::TextureHandle>(texture.release());
}

void RmlRenderInterfaceSkia::ReleaseTexture(const Rml::TextureHandle handle) {
  delete reinterpret_cast<Texture*>(handle);
}

void RmlRenderInterfaceSkia::EnableScissorRegion(const bool enable) {
  scissor_enabled_ = enable;
}

void RmlRenderInterfaceSkia::SetScissorRegion(const Rml::Rectanglei region) {
  scissor_ = region;
}

void RmlRenderInterfaceSkia::EnableClipMask(const bool enable) {
  clip_mask_enabled_ = enable;
}

void RmlRenderInterfaceSkia::RenderToClipMask(
    const Rml::ClipMaskOperation operation,
    const Rml::CompiledGeometryHandle geometry_handle,
    const Rml::Vector2f translation) {
  auto* geometry = reinterpret_cast<Geometry*>(geometry_handle);
  Layer* layer = layer_stack_.empty() ? nullptr : layer_stack_.back();
  if (!geometry)
    return;
  // Keep base-target clip state in a record with no render surface.
  if (!layer) {
    if (layers_.empty() || layers_.front()->surface) {
      auto base_state = std::make_unique<Layer>();
      layer = base_state.get();
      layers_.insert(layers_.begin(), std::move(base_state));
    } else {
      layer = layers_.front().get();
    }
  }

  SkPathBuilder path_builder;
  for (std::size_t index = 0; index + 2 < geometry->indices.size(); index += 3) {
    const auto a_index = geometry->indices[index];
    const auto b_index = geometry->indices[index + 1];
    const auto c_index = geometry->indices[index + 2];
    if (a_index >= geometry->positions.size() ||
        b_index >= geometry->positions.size() ||
        c_index >= geometry->positions.size())
      continue;
    auto map = [&](const SkPoint point) {
      auto result = map_point(transform_.get(),
                              {point.x() + translation.x,
                               point.y() + translation.y});
      return SkPoint::Make(result.x, result.y);
    };
    const auto a = map(geometry->positions[a_index]);
    const auto b = map(geometry->positions[b_index]);
    const auto c = map(geometry->positions[c_index]);
    path_builder.moveTo(a);
    path_builder.lineTo(b);
    path_builder.lineTo(c);
    path_builder.close();
  }
  SkPath path = path_builder.detach();
  if (operation == Rml::ClipMaskOperation::SetInverse)
    path.toggleInverseFillType();
  if (operation == Rml::ClipMaskOperation::Intersect && layer->has_clip_mask) {
    SkPath intersection;
    if (Op(layer->clip_mask, path, SkPathOp::kIntersect_SkPathOp,
           &intersection))
      layer->clip_mask = std::move(intersection);
  } else {
    layer->clip_mask = std::move(path);
  }
  layer->has_clip_mask = true;
}

void RmlRenderInterfaceSkia::SetTransform(const Rml::Matrix4f* transform) {
  if (transform)
    transform_ = std::make_unique<Rml::Matrix4f>(*transform);
  else
    transform_.reset();
}

Rml::LayerHandle RmlRenderInterfaceSkia::PushLayer() {
  auto* base = device_.surface();
  if (!base)
    return {};
  auto layer = std::make_unique<Layer>();
  layer->surface = base->makeSurface(device_.logical_width(),
                                     device_.logical_height());
  if (!layer->surface)
    return {};
  layer->surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  auto* result = layer.get();
  layers_.push_back(std::move(layer));
  layer_stack_.push_back(result);
  return reinterpret_cast<Rml::LayerHandle>(result);
}

void RmlRenderInterfaceSkia::CompositeLayers(
    const Rml::LayerHandle source_handle,
    const Rml::LayerHandle destination_handle, const Rml::BlendMode blend_mode,
    const Rml::Span<const Rml::CompiledFilterHandle> filters) {
  Layer* source = find_layer(source_handle);
  Layer* destination = find_layer(destination_handle);
  if (!source || !source->surface)
    return;
  auto image = source->surface->makeImageSnapshot();
  if (!image)
    return;

  for (const auto filter_handle : filters) {
    auto* filter = reinterpret_cast<Filter*>(filter_handle);
    if (!filter)
      continue;
    auto filtered_surface = source->surface->makeSurface(
        device_.logical_width(), device_.logical_height());
    if (!filtered_surface)
      continue;
    auto* canvas = filtered_surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    paint.setAntiAlias(true);
    switch (filter->type) {
    case FilterType::Opacity:
      paint.setAlphaf(std::clamp(filter->value, 0.f, 1.f));
      canvas->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
      break;
    case FilterType::Blur:
      paint.setImageFilter(SkImageFilters::Blur(filter->sigma, filter->sigma,
                                                nullptr));
      canvas->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
      break;
    case FilterType::DropShadow:
      paint.setImageFilter(SkImageFilters::DropShadow(
          filter->offset.x, filter->offset.y, filter->sigma, filter->sigma,
          filter->color, nullptr));
      canvas->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
      break;
    case FilterType::ColorMatrix:
      paint.setColorFilter(SkColorFilters::Matrix(filter->color_matrix));
      canvas->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
      break;
    case FilterType::MaskImage:
      canvas->drawImage(image, 0, 0);
      if (filter->mask) {
        paint.setBlendMode(SkBlendMode::kDstIn);
        canvas->drawImage(filter->mask, 0, 0, SkSamplingOptions(), &paint);
      }
      break;
    }
    image = filtered_surface->makeImageSnapshot();
  }

  auto* destination_canvas = destination && destination->surface
                                 ? destination->surface->getCanvas()
                                 : (device_.surface()
                                        ? device_.surface()->getCanvas()
                                        : nullptr);
  if (!destination_canvas)
    return;
  destination_canvas->save();
  if (scissor_enabled_)
    destination_canvas->clipRect(to_sk_rect(scissor_), SkClipOp::kIntersect,
                                 false);
  SkPaint paint;
  paint.setBlendMode(blend_mode == Rml::BlendMode::Replace
                         ? SkBlendMode::kSrc
                         : SkBlendMode::kSrcOver);
  destination_canvas->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
  destination_canvas->restore();
}

void RmlRenderInterfaceSkia::PopLayer() {
  if (!layer_stack_.empty())
    layer_stack_.pop_back();
}

Rml::TextureHandle RmlRenderInterfaceSkia::SaveLayerAsTexture() {
  auto* canvas = current_canvas();
  if (!canvas)
    return {};
  auto image = canvas->getSurface()->makeImageSnapshot();
  if (!image)
    return {};
  const auto bounds = scissor_enabled_
                          ? SkIRect::MakeLTRB(scissor_.Left(), scissor_.Top(),
                                             scissor_.Right(), scissor_.Bottom())
                          : SkIRect::MakeWH(device_.logical_width(),
                                           device_.logical_height());
  image = image->makeSubset(nullptr, bounds, {});
  if (!image)
    return {};
  auto texture = std::make_unique<Texture>();
  texture->width = bounds.width();
  texture->height = bounds.height();
  texture->image = std::move(image);
  return reinterpret_cast<Rml::TextureHandle>(texture.release());
}

Rml::CompiledFilterHandle RmlRenderInterfaceSkia::SaveLayerAsMaskImage() {
  auto* canvas = current_canvas();
  if (!canvas)
    return {};
  auto filter = std::make_unique<Filter>();
  filter->type = FilterType::MaskImage;
  filter->mask = canvas->getSurface()->makeImageSnapshot();
  return filter->mask
             ? reinterpret_cast<Rml::CompiledFilterHandle>(filter.release())
             : Rml::CompiledFilterHandle{};
}

Rml::CompiledFilterHandle RmlRenderInterfaceSkia::CompileFilter(
    const Rml::String& name, const Rml::Dictionary& parameters) {
  auto filter = std::make_unique<Filter>();
  if (name == "opacity") {
    filter->type = FilterType::Opacity;
    filter->value = Rml::Get(parameters, "value", 1.f);
  } else if (name == "blur") {
    filter->type = FilterType::Blur;
    filter->sigma = Rml::Get(parameters, "sigma", 1.f);
  } else if (name == "drop-shadow") {
    filter->type = FilterType::DropShadow;
    filter->sigma = Rml::Get(parameters, "sigma", 0.f);
    filter->offset = Rml::Get(parameters, "offset", Rml::Vector2f{});
    filter->color = to_sk_color(
        Rml::Get(parameters, "color", Rml::Colourb{}));
  } else {
    filter->type = FilterType::ColorMatrix;
    const float value = Rml::Get(parameters, "value", 1.f);
    if (name == "brightness") {
      filter->color_matrix.setScale(value, value, value, 1.f);
    } else if (name == "contrast") {
      filter->color_matrix.setScale(value, value, value, 1.f);
      const float shift = 0.5f - 0.5f * value;
      filter->color_matrix.postTranslate(shift, shift, shift, 0.f);
    } else if (name == "saturate") {
      filter->color_matrix.setSaturation(value);
    } else if (name == "grayscale") {
      filter->color_matrix.setSaturation(1.f - value);
    } else if (name == "invert") {
      const float v = std::clamp(value, 0.f, 1.f);
      const float scale = 1.f - 2.f * v;
      filter->color_matrix.setScale(scale, scale, scale, 1.f);
      filter->color_matrix.postTranslate(v, v, v, 0.f);
    } else if (name == "sepia") {
      const float v = value, r = 1.f - v;
      const float values[20] = {
          r + .393f*v, .769f*v, .189f*v, 0, 0,
          .349f*v, r + .686f*v, .168f*v, 0, 0,
          .272f*v, .534f*v, r + .131f*v, 0, 0,
          0, 0, 0, 1, 0};
      filter->color_matrix.setRowMajor(values);
    } else if (name == "hue-rotate") {
      const float c = std::cos(value), s = std::sin(value);
      const float values[20] = {
          .213f+.787f*c-.213f*s, .715f-.715f*c-.715f*s, .072f-.072f*c+.928f*s, 0, 0,
          .213f-.213f*c+.143f*s, .715f+.285f*c+.140f*s, .072f-.072f*c-.283f*s, 0, 0,
          .213f-.213f*c-.787f*s, .715f-.715f*c+.715f*s, .072f+.928f*c+.072f*s, 0, 0,
          0, 0, 0, 1, 0};
      filter->color_matrix.setRowMajor(values);
    } else {
      Rml::Log::Message(Rml::Log::LT_WARNING,
                        "Unsupported Skia filter '%s'.", name.c_str());
      return {};
    }
  }
  return reinterpret_cast<Rml::CompiledFilterHandle>(filter.release());
}

void RmlRenderInterfaceSkia::ReleaseFilter(
    const Rml::CompiledFilterHandle handle) {
  delete reinterpret_cast<Filter*>(handle);
}

Rml::CompiledShaderHandle RmlRenderInterfaceSkia::CompileShader(
    const Rml::String& name, const Rml::Dictionary& parameters) {
  auto shader = std::make_unique<Shader>();
  if (name == "linear-gradient") {
    shader->type = ShaderType::Linear;
    shader->p = Rml::Get(parameters, "p0", Rml::Vector2f{});
    shader->v = Rml::Get(parameters, "p1", Rml::Vector2f{}) - shader->p;
  } else if (name == "radial-gradient") {
    shader->type = ShaderType::Radial;
    shader->p = Rml::Get(parameters, "center", Rml::Vector2f{});
    shader->v = Rml::Get(parameters, "radius", Rml::Vector2f{1.f});
  } else if (name == "conic-gradient") {
    shader->type = ShaderType::Conic;
    shader->p = Rml::Get(parameters, "center", Rml::Vector2f{});
    shader->angle = Rml::Get(parameters, "angle", 0.f);
  } else if (name == "shader" &&
             Rml::Get(parameters, "value", Rml::String{}) == "creation") {
    shader->type = ShaderType::Creation;
    return reinterpret_cast<Rml::CompiledShaderHandle>(shader.release());
  } else {
    return {};
  }
  shader->repeating = Rml::Get(parameters, "repeating", false);
  const auto iterator = parameters.find("color_stop_list");
  if (iterator == parameters.end() ||
      iterator->second.GetType() != Rml::Variant::COLORSTOPLIST)
    return {};
  const auto& stops = iterator->second.GetReference<Rml::ColorStopList>();
  shader->positions.reserve(stops.size());
  shader->colors.reserve(stops.size());
  for (const auto& stop : stops) {
    shader->positions.push_back(stop.position.number);
    shader->colors.push_back(to_sk_gradient_color(stop.color));
  }
  return reinterpret_cast<Rml::CompiledShaderHandle>(shader.release());
}

void RmlRenderInterfaceSkia::RenderShader(
    const Rml::CompiledShaderHandle shader_handle,
    const Rml::CompiledGeometryHandle geometry_handle,
    const Rml::Vector2f translation, const Rml::TextureHandle texture_handle) {
  auto* shader = reinterpret_cast<Shader*>(shader_handle);
  auto* geometry = reinterpret_cast<Geometry*>(geometry_handle);
  if (!shader || !geometry)
    return;
  if (shader->type == ShaderType::Creation) {
    draw_geometry(*geometry, translation,
                  reinterpret_cast<Texture*>(texture_handle), nullptr);
    return;
  }
  const auto mode = shader->repeating ? SkTileMode::kRepeat : SkTileMode::kClamp;
  SkGradient gradient(SkGradient::Colors(
      SkSpan(shader->colors.data(), shader->colors.size()),
      SkSpan(shader->positions.data(), shader->positions.size()), mode), {});
  sk_sp<SkShader> sk_shader;
  if (shader->type == ShaderType::Linear) {
    const SkPoint points[] = {{shader->p.x, shader->p.y},
                              {shader->p.x + shader->v.x,
                               shader->p.y + shader->v.y}};
    sk_shader = SkShaders::LinearGradient(points, gradient);
  } else if (shader->type == ShaderType::Radial) {
    const float radius = std::max(std::abs(shader->v.x),
                                  std::abs(shader->v.y));
    sk_shader = SkShaders::RadialGradient(
        {shader->p.x, shader->p.y}, std::max(radius, 0.001f), gradient);
  } else {
    const float angle_degrees =
        shader->angle * 180.f / 3.14159265358979323846f;
    sk_shader = SkShaders::SweepGradient(
        {shader->p.x, shader->p.y}, angle_degrees, angle_degrees + 360.f,
        gradient);
  }
  draw_geometry(*geometry, translation, nullptr, sk_shader.get());
}

void RmlRenderInterfaceSkia::ReleaseShader(
    const Rml::CompiledShaderHandle handle) {
  delete reinterpret_cast<Shader*>(handle);
}

} // namespace tokmon::desk
