#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <memory>
#include <vector>

class SkCanvas;
class SkShader;

namespace tokmon::desk {

class SkiaDevice;

class RmlRenderInterfaceSkia final : public Rml::RenderInterface {
public:
  explicit RmlRenderInterfaceSkia(SkiaDevice& device);
  ~RmlRenderInterfaceSkia() override;

  Rml::CompiledGeometryHandle CompileGeometry(
      Rml::Span<const Rml::Vertex> vertices,
      Rml::Span<const int> indices) override;
  void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

  Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions,
                                 const Rml::String& source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                     Rml::Vector2i dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture) override;

  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;

  void EnableClipMask(bool enable) override;
  void RenderToClipMask(Rml::ClipMaskOperation operation,
                        Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation) override;
  void SetTransform(const Rml::Matrix4f* transform) override;

  Rml::LayerHandle PushLayer() override;
  void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                       Rml::BlendMode blend_mode,
                       Rml::Span<const Rml::CompiledFilterHandle> filters) override;
  void PopLayer() override;
  Rml::TextureHandle SaveLayerAsTexture() override;
  Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

  Rml::CompiledFilterHandle CompileFilter(
      const Rml::String& name, const Rml::Dictionary& parameters) override;
  void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

  Rml::CompiledShaderHandle CompileShader(
      const Rml::String& name, const Rml::Dictionary& parameters) override;
  void RenderShader(Rml::CompiledShaderHandle shader,
                    Rml::CompiledGeometryHandle geometry,
                    Rml::Vector2f translation,
                    Rml::TextureHandle texture) override;
  void ReleaseShader(Rml::CompiledShaderHandle shader) override;

  // Drop only swapchain-sized render targets after the platform device was
  // recreated. Geometry and decoded textures are CPU-owned and remain valid.
  void reset_after_device_recovery();

private:
  struct Geometry;
  struct Texture;
  struct Layer;
  struct Filter;
  struct Shader;

  [[nodiscard]] SkCanvas* current_canvas() const;
  [[nodiscard]] Layer* find_layer(Rml::LayerHandle handle) const;
  void configure_canvas(SkCanvas& canvas, bool include_transform,
                        Rml::Vector2f translation) const;
  void draw_geometry(Geometry& geometry, Rml::Vector2f translation,
                     Texture* texture, SkShader* shader);

  SkiaDevice& device_;
  bool scissor_enabled_{false};
  Rml::Rectanglei scissor_{};
  bool clip_mask_enabled_{false};
  std::unique_ptr<Rml::Matrix4f> transform_;
  std::unique_ptr<Layer> base_layer_;
  std::vector<std::unique_ptr<Layer>> layers_;
  std::vector<Layer*> layer_stack_;
};

} // namespace tokmon::desk
