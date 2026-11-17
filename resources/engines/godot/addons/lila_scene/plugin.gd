@tool
extends EditorPlugin

const LilaSceneImport := preload("lila_scene_import.gd")

var _importer: EditorImportPlugin


func _enter_tree() -> void:
	var settings := EditorInterface.get_editor_settings()
	if not settings.has_setting(LilaSceneImport.BATCH_SETTING):
		settings.set_setting(LilaSceneImport.BATCH_SETTING, "")
	settings.set_initial_value(LilaSceneImport.BATCH_SETTING, "", false)
	settings.add_property_info({
		"name": LilaSceneImport.BATCH_SETTING,
		"type": TYPE_STRING,
		"hint": PROPERTY_HINT_GLOBAL_FILE,
	})
	_importer = LilaSceneImport.new()
	add_import_plugin(_importer)


func _exit_tree() -> void:
	remove_import_plugin(_importer)
	_importer = null
