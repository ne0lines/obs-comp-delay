#include "scene-utils.hpp"

#include <obs-frontend-api.h>

#include <cstring>
#include <unordered_set>

namespace comp_delay::obs_ui {

std::vector<std::string> listSceneNames()
{
	std::vector<std::string> names;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *source = scenes.sources.array[i];
		const char *name = obs_source_get_name(source);
		if (name && *name)
			names.emplace_back(name);
	}

	obs_frontend_source_list_free(&scenes);
	return names;
}

obs_source_t *getSceneRefByName(const std::string &name)
{
	if (name.empty())
		return nullptr;

	obs_source_t *result = nullptr;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *source = scenes.sources.array[i];
		const char *sceneName = obs_source_get_name(source);
		if (sceneName && name == sceneName) {
			result = obs_source_get_ref(source);
			break;
		}
	}

	obs_frontend_source_list_free(&scenes);
	return result;
}

bool switchToSceneByName(const std::string &name)
{
	obs_source_t *scene = getSceneRefByName(name);
	if (!scene)
		return false;

	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
	return true;
}

namespace {

struct SourceSearch {
	const char *sourceId = nullptr;
	bool found = false;
	std::unordered_set<obs_source_t *> visited;
};

bool sceneContainsSourceIdRecursive(obs_source_t *sceneSource, SourceSearch &search);

bool enumSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<SourceSearch *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	const char *id = obs_source_get_unversioned_id(source);
	if (id && search->sourceId && std::strcmp(id, search->sourceId) == 0) {
		search->found = true;
		return false;
	}

	if (sceneContainsSourceIdRecursive(source, *search))
		return false;

	return true;
}

bool sceneContainsSourceIdRecursive(obs_source_t *sceneSource, SourceSearch &search)
{
	if (!sceneSource || !search.sourceId || search.found)
		return search.found;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	if (!search.visited.insert(sceneSource).second)
		return false;

	obs_scene_enum_items(scene, enumSceneItem, &search);
	return search.found;
}

} // namespace

bool sceneContainsSourceId(obs_source_t *sceneSource, const char *sourceId)
{
	if (!sceneSource || !sourceId)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	SourceSearch search;
	search.sourceId = sourceId;
	search.visited.insert(sceneSource);
	obs_scene_enum_items(scene, enumSceneItem, &search);
	return search.found;
}

} // namespace comp_delay::obs_ui
