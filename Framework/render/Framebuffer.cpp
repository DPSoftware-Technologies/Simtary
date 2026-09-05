#include "render/Framebuffer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace st::gfx {

using namespace wi::graphics;

Framebuffer::~Framebuffer() {
	Destroy();
}

bool Framebuffer::Create(uint32_t width, uint32_t height, Mode mode) {
	Desc desc;
	desc.width = width;
	desc.height = height;
	desc.mode = mode;
	return Create(desc);
}

bool Framebuffer::Create(const Desc& desc) {
	Destroy();

	if (desc.width == 0 || desc.height == 0) {
		wi::backlog::post("[Framebuffer] zero-sized framebuffer requested", wi::backlog::LogLevel::Error);
		return false;
	}

	desc_ = desc;

	GraphicsDevice* device = GetDevice();

	TextureDesc td;
	td.width = desc_.width;
	td.height = desc_.height;
	td.format = desc_.format;
	td.bind_flags = BindFlag::SHADER_RESOURCE;
	td.usage = Usage::DEFAULT;
	td.layout = ResourceState::SHADER_RESOURCE;

	if (desc_.mode == Mode::GPU) {
		td.bind_flags |= BindFlag::RENDER_TARGET;
		td.clear.color[0] = desc_.clearColor.x;
		td.clear.color[1] = desc_.clearColor.y;
		td.clear.color[2] = desc_.clearColor.z;
		td.clear.color[3] = desc_.clearColor.w;
	}

	if (!device->CreateTexture(&td, nullptr, &texture_)) {
		wi::backlog::post("[Framebuffer] CreateTexture failed", wi::backlog::LogLevel::Error);
		return false;
	}
	device->SetName(&texture_, "st::gfx::Framebuffer");

	if (desc_.mode == Mode::GPU) {
		drawCanvas_.init(desc_.width, desc_.height);
		return true;
	}

	// CPU mode: a second texture the CPU can write to, and the canvas that fills it.
	TextureDesc sd = td;
	sd.bind_flags = BindFlag::NONE;
	sd.usage = Usage::UPLOAD;
	sd.layout = ResourceState::COPY_SRC;
	if (!device->CreateTexture(&sd, nullptr, &staging_)) {
		wi::backlog::post("[Framebuffer] CreateTexture(staging) failed", wi::backlog::LogLevel::Error);
		texture_ = {};
		return false;
	}
	device->SetName(&staging_, "st::gfx::Framebuffer::staging");

	// allocate_buffer = false: the canvas draws into `pixels_` rather than into the
	// staging texture's mapped pointer. That pointer can carry row padding and may
	// need flipping on the way out, neither of which the canvas can express.
	pixels_.resize((size_t)desc_.width * desc_.height);
	std::memset(pixels_.data(), 0, pixels_.size() * sizeof(uint32_t));

	canvas_ = new GFXcanvas(
		(uint16_t)desc_.width, (uint16_t)desc_.height, GFXcanvas::Format::ARGB8888, /*allocate_buffer=*/false);
	canvas_->attachBuffer((uint8_t*)pixels_.data());

	return true;
}

void Framebuffer::Destroy() {
	delete canvas_;
	canvas_ = nullptr;
	pixels_.clear();
	staging_ = {};
	texture_ = {};
	desc_ = Desc();
}

int Framebuffer::GetDescriptorIndex() const {
	if (!texture_.IsValid()) return -1;
	return GetDevice()->GetDescriptorIndex(&texture_, SubresourceType::SRV);
}

// CPU drawing

void Framebuffer::Clear(uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->fillScreen(color);
}

void Framebuffer::SetPixel(int x, int y, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->drawPixel((int16_t)x, (int16_t)y, color);
}

void Framebuffer::DrawLine(int x0, int y0, int x1, int y1, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->drawLine((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, color);
}

void Framebuffer::DrawRect(int x, int y, int w, int h, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->drawRect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, color);
}

void Framebuffer::FillRect(int x, int y, int w, int h, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->fillRect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, color);
}

void Framebuffer::DrawCircle(int x, int y, int radius, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->drawCircle((int16_t)x, (int16_t)y, (int16_t)radius, color);
}

void Framebuffer::FillCircle(int x, int y, int radius, uint32_t color) {
	if (canvas_ == nullptr) return;
	canvas_->fillCircle((int16_t)x, (int16_t)y, (int16_t)radius, color);
}

void Framebuffer::DrawText(int x, int y, const char* text, uint32_t color, int size) {
	if (canvas_ == nullptr || text == nullptr) return;
	canvas_->setTextColor(color, GFX_TRANSPARENT);
	canvas_->setTextSize((uint8_t)std::max(1, size));
	canvas_->setCursor((int16_t)x, (int16_t)y);
	canvas_->writeText(text);
}

void Framebuffer::DrawTextF(int x, int y, uint32_t color, int size, const char* format, ...) {
	if (canvas_ == nullptr || format == nullptr) return;

	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	DrawText(x, y, buffer, color, size);
}

void Framebuffer::DrawImage(int x, int y, const uint32_t* argb, int width, int height) {
	if (canvas_ == nullptr || argb == nullptr || width <= 0 || height <= 0) return;
	canvas_->drawRGBBitmap((int16_t)x, (int16_t)y, const_cast<uint32_t*>(argb), (int16_t)width, (int16_t)height);
}

void Framebuffer::Present() {
	if (desc_.mode != Mode::CPU || !texture_.IsValid()) return;
	Present(GetDevice()->BeginCommandList());
}

void Framebuffer::Present(CommandList cmd) {
	if (desc_.mode != Mode::CPU || !texture_.IsValid() || !staging_.IsValid()) return;
	if (staging_.mapped_subresources == nullptr || staging_.mapped_subresource_count == 0) return;

	const SubresourceData& sub = staging_.mapped_subresources[0];
	uint8_t* dstBase = (uint8_t*)sub.data_ptr;
	if (dstBase == nullptr) return;

	const uint32_t w = desc_.width;
	const uint32_t h = desc_.height;

	// row_pitch is the driver's, not ours - it can exceed width * 4 - so rows are
	// placed one at a time rather than as one contiguous memcpy.
	for (uint32_t y = 0; y < h; ++y) {
		const uint32_t* srcRow = pixels_.data() + (size_t)(desc_.flipY ? (h - 1 - y) : y) * w;
		uint32_t* dstRow = (uint32_t*)(dstBase + (size_t)y * sub.row_pitch);
		if (desc_.flipX) {
			for (uint32_t x = 0; x < w; ++x) {
				dstRow[x] = srcRow[w - 1 - x];
			}
		} else {
			std::memcpy(dstRow, srcRow, (size_t)w * sizeof(uint32_t));
		}
	}

	GetDevice()->CopyTexture(&texture_, 0, 0, 0, 0, 0, &staging_, 0, 0, cmd);
}

// GPU drawing

CommandList Framebuffer::Begin() {
	CommandList cmd = GetDevice()->BeginCommandList();
	Begin(cmd);
	return cmd;
}

void Framebuffer::Begin(CommandList cmd) {
	if (desc_.mode != Mode::GPU || !texture_.IsValid()) return;

	GraphicsDevice* device = GetDevice();

	device->RenderPassBegin(&texture_, cmd, /*clear=*/true);

	Viewport viewport;
	viewport.width = (float)desc_.width;
	viewport.height = (float)desc_.height;
	device->BindViewports(1, &viewport, cmd);

	Rect scissor;
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = (int32_t)desc_.width;
	scissor.bottom = (int32_t)desc_.height;
	device->BindScissorRects(1, &scissor, cmd);

	// So wi::image / wi::font coordinates are this framebuffer's pixels rather than
	// the window's. Both are per-thread state; the render path sets them again at the
	// top of its own compose, so there is nothing to put back here.
	wi::image::SetCanvas(drawCanvas_);
	wi::font::SetCanvas(drawCanvas_);
}

void Framebuffer::End(CommandList cmd) {
	if (desc_.mode != Mode::GPU || !texture_.IsValid()) return;
	GetDevice()->RenderPassEnd(cmd);
}

// consuming the result

bool Framebuffer::BindToMaterial(
	wi::scene::Scene& scene, wi::ecs::Entity entity, wi::scene::MaterialComponent::TEXTURESLOT slot) {
	if (!texture_.IsValid()) return false;

	wi::scene::MaterialComponent* material = scene.materials.GetComponent(entity);

	if (material == nullptr) {
		// The entity may be an object whose material sits on the mesh's subsets
		// instead - the usual case for a model loaded from a .wiscene.
		if (const wi::scene::ObjectComponent* object = scene.objects.GetComponent(entity)) {
			if (const wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(object->meshID)) {
				for (const auto& subset : mesh->subsets) {
					if (wi::scene::MaterialComponent* subsetMaterial = scene.materials.GetComponent(subset.materialID)) {
						material = subsetMaterial;
						break;
					}
				}
			}
		}
	}

	if (material == nullptr) return false;

	material->textures[slot].resource.SetTexture(texture_);
	material->SetDirty();
	return true;
}

bool Framebuffer::BindToLightMask(wi::scene::Scene& scene, wi::ecs::Entity lightEntity) {
	if (!texture_.IsValid()) return false;
	if (scene.lights.GetComponent(lightEntity) == nullptr) return false;

	wi::scene::MaterialComponent* material = scene.materials.GetComponent(lightEntity);
	if (material == nullptr) {
		material = &scene.materials.Create(lightEntity);
	}

	material->textures[wi::scene::MaterialComponent::BASECOLORMAP].resource.SetTexture(texture_);
	material->SetDirty();
	return true;
}

} // namespace st::gfx
