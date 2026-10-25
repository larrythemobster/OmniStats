#pragma once

#include <memory>
#include "core/effects/SideEffects.hpp"

class DatabaseManager;
class DiscordManager;
class MMRFetcher;
class AccountManagerService;
class SessionState;

class EffectRouter {
  public:
    static void Route(SideEffects&& effects,
                      std::shared_ptr<DatabaseManager> dbManager,
                      std::shared_ptr<DiscordManager> discordManager,
                      std::shared_ptr<MMRFetcher> mmrFetcher,
                      std::shared_ptr<AccountManagerService> accountManager = nullptr,
                      std::shared_ptr<SessionState> sessionState = nullptr);
};
