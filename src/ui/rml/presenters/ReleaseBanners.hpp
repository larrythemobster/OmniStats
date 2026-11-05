#pragma once

#include <memory>
#include <string>

class SessionState;
class RemoteConfig;

std::string RenderReleaseBanners(const std::shared_ptr<SessionState>& state, const RemoteConfig* remoteConfig);
