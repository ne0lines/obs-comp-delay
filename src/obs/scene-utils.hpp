#pragma once

#include <obs.h>

#include <string>
#include <vector>

namespace comp_delay::obs_ui {

std::vector<std::string> listSceneNames();
obs_source_t *getSceneRefByName(const std::string &name);
bool switchToSceneByName(const std::string &name);
bool sceneContainsSourceId(obs_source_t *sceneSource, const char *sourceId);

} // namespace comp_delay::obs_ui
