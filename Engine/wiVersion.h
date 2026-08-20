#ifndef Simtary_VERSION_DEFINED
#define Simtary_VERSION_DEFINED

namespace wi::version
{
	// major features
	int GetMajor();
	// minor features, major bug fixes
	int GetMinor();
	// minor bug fixes, alterations
	int GetRevision();
	const char* GetVersionString();

	// --- Simtary build descriptor (A-BB-CCCC-D-EEE-FFFF-MA-MI-RE) ---
	int GetChannel();
	int GetPlatformArch();
	const char* GetPlatformOS();
	int GetAppTarget();
	const char* GetDesign();
	int GetBuildBy();
	const char* GetDescriptorString();
	const char* GetCodename();
	const char* GetArchitecture();

	// --- Original Wicked Engine version (upstream base) ---
	int GetOGMajor();
	int GetOGMinor();
	int GetOGRevision();
	const char* GetOGVersionString();

	// Full formatted credits with creator, contributors, supporters...
	const char* GetCreditsString();

	// Only the supporters
	const char* GetSupportersString();
}

#endif // Simtary_VERSION_DEFINED
