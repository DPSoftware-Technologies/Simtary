#pragma once
#include "stAudio.h"        // engine: wi::audio::DSPSource
#include <faust/gui/UI.h>   // Faust UI base (FAUSTFLOAT + control-collector base)

#include <string>
#include <vector>

// Project-side generic wrapper around any AOT Faust-generated `dsp` subclass.
// FaustProcessor<DSP> adapts it to the engine's wi::audio::DSPSource and exposes
// the DSP's Faust UI controls so the FaustManager panel can drive them without
// knowing the concrete type.

namespace st::audio
{
	// One Faust UI control, flattened for the ImGui panel. `zone` points into the
	// live DSP instance (a FAUSTFLOAT the compute() loop reads each block).
	struct FaustControl
	{
		std::string label;
		FAUSTFLOAT* zone = nullptr;
		float min = 0.0f, max = 1.0f;
	};

	// A real-time audio source (engine DSPSource) that also exposes its name and
	// controls for generic introspection by the manager / UI.
	class IFaustProcessor : public wi::audio::DSPSource
	{
	public:
		virtual const char* Name() const = 0;
		virtual std::vector<FaustControl>& Controls() = 0;
	};

	// Faust UI visitor: records active controls (sliders / nentry / button) into a
	// flat list; layout and passive widgets are ignored. Works for any .dsp.
	struct FaustControlCollector : public UI
	{
		std::vector<FaustControl>* out;
		explicit FaustControlCollector(std::vector<FaustControl>* o) : out(o) {}

		void openTabBox(const char*) override {}
		void openHorizontalBox(const char*) override {}
		void openVerticalBox(const char*) override {}
		void closeBox() override {}

		void addButton(const char* l, FAUSTFLOAT* z) override { out->push_back({ l, z, 0.0f, 1.0f }); }
		void addCheckButton(const char* l, FAUSTFLOAT* z) override { out->push_back({ l, z, 0.0f, 1.0f }); }
		void addVerticalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT mn, FAUSTFLOAT mx, FAUSTFLOAT) override { out->push_back({ l, z, float(mn), float(mx) }); }
		void addHorizontalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT mn, FAUSTFLOAT mx, FAUSTFLOAT) override { out->push_back({ l, z, float(mn), float(mx) }); }
		void addNumEntry(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT mn, FAUSTFLOAT mx, FAUSTFLOAT) override { out->push_back({ l, z, float(mn), float(mx) }); }

		void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
		void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
		void addSoundfile(const char*, const char*, Soundfile**) override {}
	};

	// Adapts a concrete AOT Faust dsp subclass (e.g. OrganDSP) to IFaustProcessor.
	template <class DSP>
	class FaustProcessor final : public IFaustProcessor
	{
	public:
		explicit FaustProcessor(const char* name) : name_(name) {}

		const char* Name() const override { return name_; }
		std::vector<FaustControl>& Controls() override { return controls_; }

		// getNumOutputs() is a compile-time constant in Faust output and is valid
		// before init(); the const_cast is only because Faust marks it non-const.
		int GetNumOutputs() const override { return const_cast<DSP&>(dsp_).getNumOutputs(); }

		void Prepare(int sampleRate) override
		{
			dsp_.init(sampleRate);
			controls_.clear();
			FaustControlCollector collector(&controls_);
			dsp_.buildUserInterface(&collector);
		}

		void Compute(int frames, float** outputs) override
		{
			// Faust dsp subclasses used here have 0 inputs; the pointer is unused.
			dsp_.compute(frames, nullptr, outputs);
		}

	private:
		const char* name_;
		DSP dsp_;
		std::vector<FaustControl> controls_;
	};
}
