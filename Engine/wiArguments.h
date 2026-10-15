#pragma once

#include <string>

// This can be used to parse and retrieve startup/command arguments of the application
namespace wi::arguments
{
	void Parse(const wchar_t* args);
    void Parse(int argc, char *argv[]);
	bool HasArgument(const std::string& value);
	// SIMTARY: the text after `prefix` of the first argument that starts with it
	//	("--connect=" -> "10.0.0.2:27015"); false when none does.
	bool GetArgumentValue(const std::string& prefix, std::string& value);
}
