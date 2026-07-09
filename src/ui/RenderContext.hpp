#pragma once
#include "imgui.h"
#include "core/Config.hpp"
#include "core/SessionState.hpp"

class DatabaseManager;

struct RenderSnapshot;
struct MmrGraphParams;

struct RenderContext {
    SessionState& state;
    ConfigData& config;
    DatabaseManager* db;
    RenderSnapshot* snap;
    float dpiScale;
    std::string* pendingBallchasingToken;

    ImFont* fontRegular;
    ImFont* fontBold;
    ImFont* fontSmall;
    ImFont* fontSmallBold;
    ImFont* fontMono;
};
