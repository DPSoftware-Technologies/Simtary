#pragma once
#include "Simtary.h"
#include "GFX.h"

#include <cstdint>

namespace st::gfx {

// 0xAARRGGBB, the packing GFXcanvas draws in. GFX_RED / GFX_WHITE / ... from GFX.h
// work anywhere one of these does.
constexpr uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// An off-screen surface you draw into and then hand to something that samples a
// texture: a material, a light mask, a st::Projector.
//
// The point of this class is that none of the plumbing is yours any more. Creating a
// texture, creating a second UPLOAD texture to write from, honouring the driver's row
// pitch (which is NOT width * 4), flipping for the mesh's UV convention and
// submitting the copy is about sixty lines of graphics API per surface, and getting
// the format wrong swaps red and blue in a way that looks like an art bug. That work
// now happens once, here.
//
// Two ways to draw, picked with Mode at creation:
//
//   Mode::CPU - a software canvas (libgfx). Pixels, lines, text, bitmaps, from
//               ordinary code, on whatever thread you like. Call Present() when the
//               frame's drawing is done.
//
//     st::gfx::Framebuffer fb;
//     fb.Create(256, 256);
//     ...
//     fb.Clear(GFX_BLACK);
//     fb.DrawTextF(4, 4, GFX_YELLOW, 1, "SPEED %d", speed);
//     fb.Present();
//
//   Mode::GPU - a render target. Anything wi::image or wi::font can draw, at full
//               GPU speed, between Begin() and End().
//
//     wi::graphics::CommandList cmd = fb.Begin();
//     wi::image::Draw(&logo, params, cmd);
//     fb.End(cmd);
//
// Either way the result is one texture:
//
//     fb.BindToMaterial(scene, screenEntity);   // a TV, a monitor, a billboard
//     fb.BindToLightMask(scene, lightEntity);   // the engine's spot/point light mask
//     projector.Attach(fb);                     // st::Projector
class Framebuffer {
public:
	enum class Mode {
		CPU, // software canvas + upload
		GPU, // render target
	};

	struct Desc {
		uint32_t width = 256;
		uint32_t height = 256;
		Mode mode = Mode::CPU;

		// Which way the V axis runs is a property of the MESH being drawn on, not of
		// the framebuffer, so it is set per surface. Canvases draw top-down (row 0 =
		// top); meshes that put V=0 at the bottom need flipY.
		bool flipY = false;
		bool flipX = false;

		// B8G8R8A8 matches GFXcanvas's native 0xAARRGGBB word on a little-endian
		// target. R8G8B8A8 here is the classic red/blue swap.
		wi::graphics::Format format = wi::graphics::Format::B8G8R8A8_UNORM;

		// GPU mode only: what Begin() clears to.
		XMFLOAT4 clearColor = XMFLOAT4(0, 0, 0, 1);
	};

	Framebuffer() = default;
	~Framebuffer();
	Framebuffer(const Framebuffer&) = delete;
	Framebuffer& operator=(const Framebuffer&) = delete;

	bool Create(const Desc& desc);
	bool Create(uint32_t width, uint32_t height, Mode mode = Mode::CPU);
	void Destroy();

	bool IsValid() const { return texture_.IsValid(); }
	uint32_t GetWidth() const { return desc_.width; }
	uint32_t GetHeight() const { return desc_.height; }
	Mode GetMode() const { return desc_.mode; }

	// ── CPU drawing ──────────────────────────────────────────────────────────────
	// All of these are no-ops on a GPU-mode framebuffer.

	// The full libgfx surface, for anything the shorthand below does not cover
	// (fonts, rotation, anti-aliased strokes, triangles, bitmap blits).
	GFXcanvas* GetCanvas() { return canvas_; }

	// Tightly packed width * height ARGB words. Stable for the lifetime of the
	// framebuffer, so it is safe to hand to a decoder or a memcpy.
	uint32_t* GetPixels() { return pixels_.empty() ? nullptr : pixels_.data(); }

	void Clear(uint32_t color = 0xFF000000u);
	void SetPixel(int x, int y, uint32_t color);
	void DrawLine(int x0, int y0, int x1, int y1, uint32_t color);
	void DrawRect(int x, int y, int w, int h, uint32_t color);
	void FillRect(int x, int y, int w, int h, uint32_t color);
	void DrawCircle(int x, int y, int radius, uint32_t color);
	void FillCircle(int x, int y, int radius, uint32_t color);
	void DrawText(int x, int y, const char* text, uint32_t color, int size = 1);
	void DrawTextF(int x, int y, uint32_t color, int size, const char* format, ...);
	// Copy a block of ARGB pixels in - a decoded image, another framebuffer, a frame
	// out of a capture buffer.
	void DrawImage(int x, int y, const uint32_t* argb, int width, int height);

	// Orientation of the upload. Cheap to change at any time - it is applied while
	// the pixels are copied, so nothing is recreated and the texture stays valid.
	void SetFlip(bool flipX, bool flipY) { desc_.flipX = flipX; desc_.flipY = flipY; }

	// Push what has been drawn to the GPU. Once per frame, after the drawing.
	void Present();
	void Present(wi::graphics::CommandList cmd);

	// ── GPU drawing ──────────────────────────────────────────────────────────────
	// No-ops on a CPU-mode framebuffer.

	// Opens a render pass on the framebuffer and returns the command list to draw
	// into. wi::image / wi::font are pointed at this framebuffer's canvas, so their
	// coordinates are framebuffer pixels, not window pixels.
	wi::graphics::CommandList Begin();
	void Begin(wi::graphics::CommandList cmd);
	void End(wi::graphics::CommandList cmd);

	// ── consuming the result ─────────────────────────────────────────────────────
	const wi::graphics::Texture& GetTexture() const { return texture_; }
	int GetDescriptorIndex() const;

	// Assign as a material texture. Defaults to base colour, which is also the slot
	// the engine reads a light's mask from.
	bool BindToMaterial(
		wi::scene::Scene& scene,
		wi::ecs::Entity entity,
		wi::scene::MaterialComponent::TEXTURESLOT slot = wi::scene::MaterialComponent::BASECOLORMAP);

	// Make a spot/point/rect light project this framebuffer. The engine takes a
	// light's mask from a MaterialComponent on the light's own entity, so this
	// creates one if the entity has none.
	//
	// Note what a spot light does with it: light_spot() in the engine core clips to a
	// circular cone, so the image lands cropped into a circle. For a square image use
	// st::Projector instead - that is what it exists for.
	bool BindToLightMask(wi::scene::Scene& scene, wi::ecs::Entity lightEntity);

private:
	Desc desc_;

	wi::graphics::Texture texture_;  // what everything else samples
	wi::graphics::Texture staging_;  // CPU mode: permanently mapped upload copy
	GFXcanvas* canvas_ = nullptr;    // CPU mode
	wi::vector<uint32_t> pixels_;    // CPU mode: what the canvas draws into
	wi::Canvas drawCanvas_;          // GPU mode: sizes wi::image / wi::font
};

} // namespace st::gfx
