#pragma once
#include "CommonInclude.h"
#include "wiGraphicsDevice.h"
#include "wiVector.h"
#include "wiPlatform.h"

#include <string>
#include <functional>

#if WI_VECTOR_TYPE
namespace std
{
	template < typename, typename > class vector;
}
#endif // WI_VECTOR_TYPE

namespace wi::helper
{
	template <class T>
	constexpr void hash_combine(std::size_t& seed, const T& v)
	{
		std::hash<T> hasher;
		seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}

	constexpr size_t string_hash(const char* input) 
	{
		// https://stackoverflow.com/questions/2111667/compile-time-string-hashing
		size_t hash = sizeof(size_t) == 8 ? 0xcbf29ce484222325 : 0x811c9dc5;
		const size_t prime = sizeof(size_t) == 8 ? 0x00000100000001b3 : 0x01000193;

		while (*input) 
		{
			hash ^= static_cast<size_t>(*input);
			hash *= prime;
			++input;
		}

		return hash;
	}

	std::string toUpper(const std::string& s);

	std::string toLower(const std::string& s);

	std::wstring toUpper(const std::wstring& s);

	std::wstring toLower(const std::wstring& s);

	void messageBox(const std::string& msg, const std::string& caption = "Warning!");

	enum class MessageBoxResult
	{
		OK,
		Cancel,
		Yes,
		No,
		Abort,
		Retry,
		Ignore
	};

	// Shows a message box with custom buttons and returns the user's choice
	//	buttons can be combinations like: "OK", "OKCancel", "YesNo", "YesNoCancel", etc.
	MessageBoxResult messageBoxCustom(const std::string& msg, const std::string& caption = "Warning!", const std::string& buttons = "OK");

	// Returns file path if successful, empty string otherwise
	std::string screenshot(const wi::graphics::SwapChain& swapchain, const std::string& name = "");

	// Returns file path if successful, empty string otherwise
	std::string screenshot(const wi::graphics::Texture& texture, const std::string& name = "");

	// Save raw pixel data from the texture to memory
	bool saveTextureToMemory(const wi::graphics::Texture& texture, wi::vector<uint8_t>& texturedata);

	// Save texture to memory as a file format (file format is determined by extension, supported extensions: .png, .jpg, .jpeg, .tga, .bmp, .dds, .ico, .h, .raw)
	bool saveTextureToMemoryFile(const wi::graphics::Texture& texture, const std::string& fileExtension, wi::vector<uint8_t>& filedata);

	// Save raw texture data to memory as file format (file format is determined by extension, supported extensions: .png, .jpg, .jpeg, .tga, .bmp, .dds, .ico, .h, .raw)
	bool saveTextureToMemoryFile(const wi::vector<uint8_t>& textureData, const wi::graphics::TextureDesc& desc, const std::string& fileExtension, wi::vector<uint8_t>& filedata);

	// Save texture to file format (file format is determined by extension, supported extensions: .png, .jpg, .jpeg, .tga, .bmp, .dds, .ico, .h, .raw)
	bool saveTextureToFile(const wi::graphics::Texture& texture, const std::string& fileName);

	// Save raw texture data to file format (file format is determined by extension, supported extensions: .png, .jpg, .jpeg, .tga, .bmp, .dds, .ico, .h, .raw)
	bool saveTextureToFile(const wi::vector<uint8_t>& texturedata, const wi::graphics::TextureDesc& desc, const std::string& fileName);

	// Download buffer from GPU into CPU memory
	bool saveBufferToMemory(const wi::graphics::GPUBuffer& buffer, wi::vector<uint8_t>& data);

	// Creates cursor data from texture. If successful, the file data is returned in the data argument
	bool CreateCursorFromTexture(const wi::graphics::Texture& texture, int hotspotX, int hotspotY, wi::vector<uint8_t>& data);

	std::string getCurrentDateTimeAsString();

	void SplitPath(const std::string& fullPath, std::string& dir, std::string& fileName);

	std::string GetFileNameFromPath(const std::string& path);

	std::string GetDirectoryFromPath(const std::string& path);

	std::string GetExtensionFromFileName(const std::string& filename);

	std::string ReplaceExtension(const std::string& filename, const std::string& extension);

	// If it already has required extension, then it does nothing, otherwise appends extension
	std::string ForceExtension(const std::string& filename, const std::string& extension);

	std::string RemoveExtension(const std::string& filename);

	std::string GetPathRelative(const std::string& rootdir, const std::string& path);

	void MakePathRelative(const std::string& rootdir, std::string& path);

	void MakePathAbsolute(std::string& path);

	std::string BackslashToForwardSlash(const std::string& str);

	void DirectoryCreate(const std::string& path);

	// --- SIMTARY EXTENSION: read-only asset source override ---
	//
	// Everything the engine loads at runtime goes through FileRead and FileExists, which
	// read the local filesystem. That holds on desktop and stops holding the moment the
	// assets are not files: on Android they live compressed inside the APK and are only
	// reachable through AAssetManager, and a console package is its own container again.
	//
	// Installing an override redirects both functions. Return false from either callback
	// to decline a path and let the normal filesystem handling take it, which is what
	// makes a mixed setup work - read-only assets from the package, save data and shader
	// cache from real files on disk.
	//
	// file_read hands back a buffer the engine immediately copies out of and then releases
	// through file_free, so the callback can point straight at a memory-mapped asset
	// instead of allocating. Install it before wi::initializer runs, and expect it to be
	// called from many loading threads at once.
	// file_stat answers FileSize and FileTimestamp for a path the source owns. It is
	// optional but strongly recommended: without it those two fall back to the real
	// filesystem for a path that has no file behind it. FileTimestamp in particular
	// used to call std::filesystem::last_write_time on such a path right after
	// file_exists said yes, and with exceptions disabled the resulting filesystem_error
	// is a __fastfail, not a return value - the process dies with
	// STATUS_STACK_BUFFER_OVERRUN and no message. Report a stable timestamp (0 is fine
	// for an immutable package: it means "never newer", so the resource cache keeps its
	// entry instead of reloading every frame).
	struct AssetSourceOverride
	{
		bool (*file_read)(const std::string& fileName, const uint8_t** out_data, size_t* out_size, void* userdata) = nullptr;
		void (*file_free)(const uint8_t* data, void* userdata) = nullptr;
		bool (*file_exists)(const std::string& fileName, void* userdata) = nullptr;
		bool (*file_stat)(const std::string& fileName, uint64_t* out_size, uint64_t* out_timestamp, void* userdata) = nullptr;
		void* userdata = nullptr;
	};

	void SetAssetSourceOverride(const AssetSourceOverride& source);
	const AssetSourceOverride& GetAssetSourceOverride();
	// ----------------------------------------------------------

	// Returns the file size if the file exists, otherwise 0
	size_t FileSize(const std::string& fileName);

	bool FileRead(const std::string& fileName, wi::vector<uint8_t>& data, size_t max_read = ~0ull, size_t offset = 0);

#if WI_VECTOR_TYPE
	// This version is provided if std::vector != wi::vector
	bool FileRead(const std::string& fileName, std::vector<uint8_t>& data, size_t max_read = ~0ull, size_t offset = 0);
#endif // WI_VECTOR_TYPE

	bool FileWrite(const std::string& fileName, const uint8_t* data, size_t size);

	bool FileExists(const std::string& fileName);

	bool DirectoryExists(const std::string& fileName);

	uint64_t FileTimestamp(const std::string& fileName);

	bool FileCopy(const std::string& filename_src, const std::string& filename_dst);

	std::string GetTempDirectoryPath();
	std::string GetCacheDirectoryPath();
	std::string GetCurrentPath();
	std::string GetExecutablePath();

	struct FileDialogParams
	{
		enum TYPE
		{
			OPEN,
			SAVE,
		} type = OPEN;
		std::string description;
		wi::vector<std::string> extensions;
		bool multiselect = true; // only for TYPE::OPEN
	};
	void FileDialog(const FileDialogParams& params, const std::function<void(std::string fileName)>& onSuccess, const std::function<void()>& onFailure = nullptr);

	std::string FolderDialog(const std::string& description = "Select folder");

	void GetFileNamesInDirectory(const std::string& directory, const std::function<void(std::string fileName)>& onSuccess, const std::string& filter_extension = "");

	void GetFolderNamesInDirectory(const std::string& directory, const std::function<void(std::string folderName)>& onSuccess);

	// Converts a file into a C++ header file that contains the file contents as byte array.
	//	dataName : the byte array's name
	bool Bin2H(const uint8_t* data, size_t size, const std::string& dst_filename, const char* dataName);

	// Converts a file into a C++ source file that contains the file contents as byte array and using extern.
	//	dataName : the byte array's name
	//	Note: size is exported as name_size where name is the dataName that you give to it
	bool Bin2CPP(const uint8_t* data, size_t size, const std::string& dst_filename, const char* dataName);

	void StringConvert(const std::string& from, std::wstring& to);

	void StringConvert(const std::wstring& from, std::string& to);

	// Parameter - to - must be pre-allocated!
	// dest_size_in_characters : number of characters in the pre-allocated string memory
	// returns result string length
	int StringConvert(const char* from, wchar_t* to, int dest_size_in_characters);

	// Parameter - to - must be pre-allocated!
	// dest_size_in_characters : number of characters in the pre-allocated string memory
	// returns result string length
	int StringConvert(const wchar_t* from, char* to, int dest_size_in_characters);

	std::string StringRemoveTrailingWhitespaces(const std::string& str);

	// Prints debug info to the console output
	enum class DebugLevel
	{
		Normal,
		Warning,
		Error
	};
	void DebugOut(const std::string& str, DebugLevel level = DebugLevel::Normal);

	// Puts the current thread to sleeping state for a given time (OS can overtake)
	void Sleep(float milliseconds);

	// Spins for the given time and does nothing (OS can not overtake)
	void Spin(float milliseconds);

	// Sleeps if duration is long enough to wake up in time, Spins otherwise
	//	This lets OS overtake thread if duration is long enough to remain accurate
	//	Also spins for more accuracy if needed
	void QuickSleep(float milliseconds);

	// Opens URL in the default browser
	void OpenUrl(const std::string& url);

	struct MemoryUsage
	{
		uint64_t total_physical = 0;	// size of physical memory on whole system (in bytes)
		uint64_t total_virtual = 0;		// size of virtual address space on whole system (in bytes)
		uint64_t process_physical = 0;	// size of currently committed physical memory by application (in bytes)
		uint64_t process_virtual = 0;	// size of currently mapped virtual memory by application (in bytes)
	};
	MemoryUsage GetMemoryUsage();

	// Returns a good looking memory size string as either bytes, KB, MB or GB
	std::string GetMemorySizeText(size_t sizeInBytes);

	// Returns a good looking timer duration text as either milliseconds, seconds, minutes or hours
	std::string GetTimerDurationText(float timerSeconds);

	// Get error message from platform-specific error code, for example HRESULT on windows
	std::string GetPlatformErrorString(wi::platform::error_type code);

	// Lossless compression of byte array; level = 0 means "default compression level", currently 3
	bool Compress(const uint8_t* src_data, size_t src_size, wi::vector<uint8_t>& dst_data, int level = 0);

	// Lossless decompression of byte array that was compressed with wi::helper::Compress()
	bool Decompress(const uint8_t* src_data, size_t src_size, wi::vector<uint8_t>& dst_data);

	// Hash the contents of a file:
	size_t HashByteData(const uint8_t* data, size_t size);

	// Returns string for paste operation
	std::wstring GetClipboardText();

	// Copies text to clipboard
	void SetClipboardText(const std::wstring& wstr);
};
