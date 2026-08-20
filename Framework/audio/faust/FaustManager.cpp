#include "audio/faust/FaustManager.h"

#include "imgui.h"

#include <utility>

namespace st::audio
{
	// No built-in processors: the AOT-generated Faust headers are project content.
	// A project registers its own from st::App::OnInitialize(), e.g.
	//     Audio().Register("organ", [] {
	//         return std::make_unique<st::audio::FaustProcessor<OrganDSP>>("organ");
	//     });
	FaustManager::FaustManager() = default;

	FaustManager::~FaustManager() { Unload(); }

	void FaustManager::Register(std::string name, Factory factory)
	{
		if (IndexOf(name) >= 0)
			return; // name already registered
		names_.push_back(std::move(name));
		factories_.push_back(std::move(factory));
	}

	int FaustManager::IndexOf(const std::string& name) const
	{
		for (size_t i = 0; i < names_.size(); ++i)
			if (names_[i] == name)
				return int(i);
		return -1;
	}

	bool FaustManager::Load(const std::string& name, int sampleRate)
	{
		const int idx = IndexOf(name);
		if (idx < 0)
			return false;

		Unload(); // stop + free whatever is currently playing

		current_ = factories_[idx]();
		if (!current_)
			return false;

		if (!stream_.Start(current_.get(), sampleRate))
		{
			current_.reset(); // device open failed — don't leave a dangling processor
			return false;
		}

		loadedName_ = name;
		return true;
	}

	void FaustManager::Unload()
	{
		stream_.Stop();     // joins the audio thread before the processor is freed
		current_.reset();
		loadedName_.clear();
	}

	void FaustManager::DrawPanel(const char* title, bool* open)
	{
		ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin(title, open))
		{
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("AOT Faust processors -> OpenAL (engine wi::audio::DSPStream)");
		ImGui::Separator();

		// Processor picker: selecting an entry loads (and starts) it immediately.
		const char* preview = IsLoaded() ? loadedName_.c_str() : "(none)";
		if (ImGui::BeginCombo("processor", preview))
		{
			for (const std::string& n : names_)
			{
				const bool selected = IsLoaded() && n == loadedName_;
				if (ImGui::Selectable(n.c_str(), selected) && !selected)
					Load(n);
			}
			ImGui::EndCombo();
		}

		if (IsLoaded())
		{
			if (ImGui::Button("Unload")) Unload();
			ImGui::SameLine();
			ImGui::Text("playing: %s", loadedName_.c_str());
		}
		else
		{
			if (ImGui::Button("Load") && !names_.empty()) Load(names_.front());
			ImGui::SameLine();
			ImGui::TextUnformatted("stopped");
		}

		float gain = stream_.GetGain();
		if (ImGui::SliderFloat("master gain", &gain, 0.0f, 1.0f))
			stream_.SetGain(gain);

		if (IsLoaded())
		{
			ImGui::Separator();
			ImGui::TextUnformatted("Faust controls");
			// The audio thread reads these zones in compute() while we write them;
			// they are plain floats, so the worst case is a one-block-stale value.
			for (FaustControl& c : current_->Controls())
			{
				float v = float(*c.zone);
				if (ImGui::SliderFloat(c.label.c_str(), &v, c.min, c.max))
					*c.zone = FAUSTFLOAT(v);
			}
		}

		ImGui::End();
	}
}
