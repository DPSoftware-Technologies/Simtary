#pragma once
#include "stAudio.h"                    // engine: wi::audio::DSPStream
#include "audio/faust/FaustProcessor.h" // IFaustProcessor

#include <functional>
#include <memory>
#include <string>
#include <vector>

// FaustManager - loads/unloads AOT Faust processors and plays the active one
// through the engine's OpenAL DSPStream. Processors are registered by name with
// a factory; Load(name) stops whatever is playing and starts the new one. Only
// one processor plays at a time (single output stream).

namespace st::audio
{
	class FaustManager
	{
	public:
		using Factory = std::function<std::unique_ptr<IFaustProcessor>()>;

		FaustManager();   // starts empty; the project registers its processors
		~FaustManager();

		FaustManager(const FaustManager&) = delete;
		FaustManager& operator=(const FaustManager&) = delete;

		// Add a processor the manager can load. `name` must be unique.
		void Register(std::string name, Factory factory);
		const std::vector<std::string>& Available() const { return names_; }

		// Load + start `name` (unloads the current one first). Returns false if the
		// name is unknown or the audio device could not be opened.
		bool Load(const std::string& name, int sampleRate = 48000);
		// Stop + destroy the active processor. Idempotent.
		void Unload();

		bool IsLoaded() const { return current_ != nullptr; }
		const std::string& LoadedName() const { return loadedName_; }
		IFaustProcessor* Current() { return current_.get(); }

		void SetGain(float gain01) { stream_.SetGain(gain01); }
		float GetGain() const { return stream_.GetGain(); }

		// ImGui window: processor picker, Load/Unload, master gain, live controls.
		void DrawPanel(const char* title, bool* open);

	private:
		int IndexOf(const std::string& name) const;

		std::vector<std::string> names_;
		std::vector<Factory>     factories_;

		wi::audio::DSPStream              stream_;
		std::unique_ptr<IFaustProcessor>  current_;
		std::string                       loadedName_;
	};
}
