#include "ui/rml/presenters/OverlayPresenter.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
bool OverlayPresenter::Bind(Rml::DataModelConstructor& constructor) {
    return m_model.Bind(constructor);
}

void OverlayPresenter::Reset() {
    m_model.Reset();
}

bool OverlayPresenter::Update(const RmlRenderSnapshot& snap,
                              const ConfigData& config,
                              const std::shared_ptr<SessionState>& state,
                              const Tournaments::TournamentScheduleService* tournaments) {
    return m_model.Update(snap, config, state, tournaments);
}
