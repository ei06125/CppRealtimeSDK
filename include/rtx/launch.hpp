// Bind versioned launch descriptions to callbacks explicitly registered by the application.
// Configuration resolves names; it never loads executable code or runs a callback.
#pragma once
#include "rtx/scheduler.hpp"
#include <istream>
#include <unordered_map>
namespace rtx {
using CallbackRegistry=std::unordered_map<std::string,std::function<void(const Context&)>>;
// Versioned text configuration; numeric durations are exact integer nanoseconds.
// Resolves callbacks and validates the complete launch graph before returning.
// Loading never invokes application callbacks. Throws invalid_argument on malformed input.
std::vector<Node> load_launch(std::istream& input,const CallbackRegistry& callbacks);
}
