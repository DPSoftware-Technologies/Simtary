#include "wiVersion.h"
#include <string>

#define ORIGINAL_SUPPORTERS \
"Nemerle, James Webb, Quifeng Jin, TheGameCreators, Joseph Goldin, Yuri, Sergey K, Yukawa Kanta, Dragon Josh, John, LurkingNinja, Bernardo Del Castillo, " \
"Invictus, Scott Hunt, Yazan Altaki, Tuan NV, Robert MacGregor, cybernescence, Alexander Dahlin, blueapples, Delhills, NI NI, Sherief, ktopoet, Justin Macklin, " \
"Cédric Fabre, TogetherTeam, Bartosz Boczula, Arne Koenig, Ivan Trajchev, nathants, Fahd Ahmed, Gabriel Jadderson, SAS_Controller, Dominik Madarász, Segfault, " \
"Mike amanfo, Dennis Brakhane, rookie, Peter Moore, therealjtgill, Nicolas Embleton, Desuuc, radino1977, Anthony Curtis, manni heck, Matthias Hölzl, Phyffer, " \
"Lucas Pinheiro, Tapkaara, gpman, Anthony Python, Gnowos, Klaus, slaughternaut, Paul Brain, Connor Greaves, Alexandr, Lee Bamber, MCAlarm MC2, Titoutan, Willow, " \
"Aldo, lokimx, K. Osterman, Nomad, ykl, Alex Krokos, Timmy, Avaflow, mat, Hexegonel Samael Michael, Joe Spataro, soru, GeniokV, Mammoth, Ignacio, datae, Jason Rice, " \
"MarsBEKET, Tim, Twisty, Zelf ieats kiezen, Romildo Franco, zNachoh, Dmitriy, Alex Minerva, Stefan Kent, Natty, Sunny Krishna, Vilmos Malárik, Ferrata, Rossakis, " \
"Stefana Andrei, Taylor, Gunnar Kriik, 赟 杨, Rex, Lemon Brother, flxy, meta_leap, Edik, jusik5348, Agnares, Fred Naar, Saki Asui, DarkRaVen, Ray, Russell Searle, Alexandr Dem'yanenko, " \
"Portaloffreedom, nxrighthere, わさび わさび, Durak, ipso, Frånken, Shryder, Patryk Bański, Marcus Nordenstam, Morty Karim, Jarrid Kamphenkel, Makario Gim, Filoteo Pasquini, " \
"Me, Xiu Shui"

namespace wi::version
{
    // ============================================================
    //  ORIGINAL WICKED ENGINE VERSION (upstream base)
    // ============================================================
    // main engine core
    const int OGmajor = 0;
    // minor features, major updates, breaking compatibility changes
    const int OGminor = 72;
    // minor bug fixes, alterations, refactors, updates
    const int OGrevision = 59;
    const std::string OGversion_string =
        std::to_string(OGmajor) + "." + std::to_string(OGminor) + "." + std::to_string(OGrevision);

    // ============================================================
    //  SIMTARY (HMMWV4) VERSION
    //  Format: A-BB-CCCC-D-EEE-FFFF-RB-MA-MI
    //  read "simtasion.txt"
    // ============================================================

    // --- SemVer core (MA-MI-RE) ---
    // main engine core
    const int reborn = 4;
    // minor features, major updates, breaking compatibility changes
    const int major = 1;
    // minor bug fixes, alterations, refactors, updates
    const int minor = 2;

    // --- Identity ---
    const std::string codename = "Detroit";      // The engine codename
    const std::string architecture = "Diesel";   // The main arch of engine

    // --- Build descriptor fields (A-BB-CCCC-D-EEE-FFFF) ---
    const int    channel      = 6;        // A    : 6 = In-Dev
    const int    platformArch  = 0;       // BB   : 0 = All Arch
    const std::string platformOS = "000D"; // CCCC : 000 = All Platform, D = Default (not combined)
    const int    appTarget    = 1;        // D    : 1 = Client
    const std::string design  = "00D";    // EEE  : 00 = Everything, D = Default (not combined)
    const int    buildBy      = 0;        // FFFF : 0000 = Official (DPSoftware / PlatoonLabs)

    // Plain SemVer string (e.g. "0.0.0")
    const std::string version_string =
        std::to_string(reborn) + "." + std::to_string(major) + "." + std::to_string(minor);

    // Helper: zero-padded integer to fixed width
    static std::string padNum(int value, int width)
    {
        std::string s = std::to_string(value);
        while ((int)s.size() < width)
            s = "0" + s;
        return s;
    }

    // Full descriptor string: A-BB-CCCC-D-EEE-FFFF-MA-MI-RE
    const std::string descriptor_string =
        padNum(channel, 1)      + "-" +
        padNum(platformArch, 2) + "-" +
        platformOS              + "-" +
        padNum(appTarget, 1)    + "-" +
        design                  + "-" +
        padNum(buildBy, 4)      + "-" +
        padNum(reborn, 2)        + "-" +
        padNum(major, 2)        + "-" +
        padNum(minor, 2);

    // ============================================================
    //  ACCESSORS
    // ============================================================
    int GetMajor()
    {
        return reborn;
    }
    int GetMinor()
    {
        return major;
    }
    int GetRevision()
    {
        return minor;
    }
    const char* GetVersionString()
    {
        return version_string.c_str();
    }

    // --- Simtary descriptor accessors ---
    int GetChannel()
    {
        return channel;
    }
    int GetPlatformArch()
    {
        return platformArch;
    }
    const char* GetPlatformOS()
    {
        return platformOS.c_str();
    }
    int GetAppTarget()
    {
        return appTarget;
    }
    const char* GetDesign()
    {
        return design.c_str();
    }
    int GetBuildBy()
    {
        return buildBy;
    }
    const char* GetDescriptorString()
    {
        return descriptor_string.c_str();
    }
    const char* GetCodename()
    {
        return codename.c_str();
    }
    const char* GetArchitecture()
    {
        return architecture.c_str();
    }

    // --- Original Wicked Engine version accessors ---
    int GetOGMajor()
    {
        return OGmajor;
    }
    int GetOGMinor()
    {
        return OGminor;
    }
    int GetOGRevision()
    {
        return OGrevision;
    }
    const char* GetOGVersionString()
    {
        return OGversion_string.c_str();
    }

    const char* GetCreditsString()
    {
        static const char* credits = R"(
Simtary (HMMWV4)
-----------
Development by: DPSoftware Technologies
Based on Wicked Engine by Turánszki János
Original Contributors: Silas Oler, Matteo De Carlo, Amer Koleci, James Webb, Megumumpkin, Preben Eriksen, Dennis Brakhane, Stanislav Denisov
Original Wicked Engine Patreon supporters
---------------------------
)" ORIGINAL_SUPPORTERS;
        return credits;
    }
    const char* GetSupportersString()
    {
        return ORIGINAL_SUPPORTERS;
    }
}