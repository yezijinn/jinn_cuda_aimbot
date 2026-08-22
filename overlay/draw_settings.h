#ifndef DRAW_SETTINGS_H
#define DRAW_SETTINGS_H

#include <cstddef>
#include <string>
#include <vector>

std::vector<std::string> getAvailableModels();

void draw_capture_general_settings();
void draw_capture_source_settings();
void draw_capture_preview();
void draw_capture_and_model_settings();
void draw_performance_settings();
void draw_model_path_settings();
void draw_hotkey_profile(std::size_t slot);
void draw_tracker();
void draw_mouse();
void draw_mouse_movement();
void draw_mouse_prediction();
void draw_mouse_assist();
void draw_mouse_profiles();
void draw_mouse_input();
void draw_ai();
void draw_global_ai_settings();
void draw_buttons();
void draw_overlay();
void draw_stats_summary();
void draw_stats();
void draw_debug();
void draw_tracker();

#endif // DRAW_SETTINGS_H
