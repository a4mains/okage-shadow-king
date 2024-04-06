#pragma once
#include "gs/GSHandler.h"
#include <atomic>
#include <filesystem>
#include <vector>
struct ResearchTraffic { std::atomic<uint64> registers{0},vertices{0},uploads{0}; bool retain_register_tail=false; };
CGSHandler::FactoryFunction vulkanReference(ResearchTraffic&);
void captureReference(CGSHandler*,const std::filesystem::path& prefix);
void captureReferenceRegisters(CGSHandler*,const std::filesystem::path&);
// Raw GS alpha retained for bounded shadow raster differential tests.
std::vector<uint8> captureShadowPixels(CGSHandler*);
