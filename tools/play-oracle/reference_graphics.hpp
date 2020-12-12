#pragma once
#include "gs/GSHandler.h"
#include <atomic>
#include <filesystem>
struct ResearchTraffic { std::atomic<uint64> registers{0},vertices{0},uploads{0}; };
CGSHandler::FactoryFunction vulkanReference(ResearchTraffic&);
void captureReference(CGSHandler*,const std::filesystem::path& prefix);
