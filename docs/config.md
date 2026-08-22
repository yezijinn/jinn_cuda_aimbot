# Configuration Guide

The runtime config is `config.ini`. It is created automatically the first time the app starts if it does not already exist.

Most settings are saved as simple root-level `key = value` lines. The only true INI section currently written by the app is `[Games]`, which stores game sensitivity profiles.

For most users, the GUI is the safest way to edit settings. This page exists so you can also understand and edit the file directly.

## Basic Rules

- Use `true` or `false` for booleans.
- Use a dot for decimal values, for example `0.35`.
- Button lists are comma-separated, for example `RightMouseButton,LeftShift`.
- Paths may be relative to the executable folder.
- If a value is outside its accepted range, the app clamps it or falls back to a valid default.

The defaults below are the generated defaults for a fresh config. A few compat-key fallbacks in source are different for backward compatibility; those are called out where they matter.

## Quick Backend Setup

The current build is CUDA/TensorRT only. Use TensorRT engine models:

```ini
backend = TRT
ai_model = Jinn.engine
capture_use_cuda = false
```

Circle FOV can remain enabled for circular target filtering.

## Capture

| Key | Default | Meaning |
|---|---:|---|
| `capture_method` | `duplication_api` | Capture source. Common values are `duplication_api`, `winrt`, `virtual_camera`, and `udp_capture`. |
| `capture_target` | `monitor` | Target type for capture. Usually `monitor`. |
| `capture_window_title` | empty | Window title used when a window capture target is selected. |
| `udp_ip` | `0.0.0.0` | Sender IP filter for UDP capture. Use `0.0.0.0` to accept frames from any sender. |
| `udp_port` | `1234` | UDP capture port. |
| `detection_resolution` | `320` | Square inference/capture processing size. Valid values are `256`, `320`, `416`, `512`, `640`, and `960`. Higher can improve detail but costs performance. |
| `capture_fps` | `60` | Requested capture rate. |
| `monitor_idx` | `0` | Monitor index for monitor capture. |
| `circle_fov_enabled` | `true` | Enables the current circular FOV limiter. |
| `circle_fov_radius_percent` | `100` | Circle FOV radius as a percent of the processed capture area. Clamped to `1..100`. |
| `circle_fov_show_preview` | `true` | Shows Circle FOV in the GUI preview when available. |
| `capture_borders` | `true` | Include window borders when applicable. |
| `capture_cursor` | `true` | Include cursor when applicable. |
| `virtual_camera_name` | `None` | Camera name for virtual camera capture. |
| `virtual_camera_width` | `1920` | Requested virtual camera width. |
| `virtual_camera_heigth` | `1080` | Requested virtual camera height. The key is currently spelled `heigth` in the config for compatibility. |

### UDP Capture

Use UDP capture when another PC or process sends a screen/camera stream to this app over the network.

```ini
capture_method = udp_capture
udp_ip = 0.0.0.0
udp_port = 1234
detection_resolution = 320
capture_fps = 60
```

The receiver expects an MJPEG byte stream over UDP. Each frame must be a normal JPEG image; the app finds frames by JPEG start/end markers and decodes them with OpenCV. This is not RTP, RTSP, or a custom packet-header protocol.

`udp_ip = 0.0.0.0` is the recommended diagnostic setting because it accepts any sender. Set `udp_ip` to a specific sender IPv4 address only when you want to ignore packets from other machines. The app listens on `udp_port`; make sure that UDP port is allowed through Windows Firewall on the receiver PC.

For FFmpeg sender examples, see [UDP capture over LAN](guides/udp-capture.md).

### Circle FOV

Use `circle_fov_enabled` for normal circular aim limiting and overlay visualization.

## Targeting

| Key | Default | Meaning |
|---|---:|---|
| `aim_offset_x` | `0.5` | Normalized X aim point within the target bounding box. `0.0` = left edge, `1.0` = right edge, `0.5` = center. Clamped `0.0..1.0`. |
| `aim_offset_y` | `0.5` | Normalized Y aim point within the target bounding box. `0.0` = top edge, `1.0` = bottom edge, `0.5` = center. Clamped `0.0..1.0`. |
| `auto_aim` | `false` | Enables automatic aim behavior when supported by current controls and buttons. |
| `tracker_enabled` | `true` | Enables the simple persistent target tracker. When disabled, aiming falls back to nearest-target selection each detection frame. |
| `tracker_overlay_table_enabled` | `true` | Shows the target-track information table in the Tracker overlay tab. |
| `targeting_mode` | `closest_center` | Target selection mode. `closest_center` picks the target nearest the crosshair center. `largest_box` picks the largest bounding box. |

The aim point is computed as `targetX = box.x + box.width * aim_offset_x`, `targetY = box.y + box.height * aim_offset_y`. This point is used uniformly across target selection, locking, Kalman prediction measurement, overlay drawing, and auto-trigger zone judgment. The fields `class_player` and `class_head` are stored in the config for compatibility but have no special routing semantics in the current version.

## Mouse Movement and Tracking

| Key | Default | Meaning |
|---|---:|---|
| `fovX` | `85` | Horizontal game FOV used for movement conversion. Missing-key fallback for older configs is `121`. |
| `fovY` | `55` | Vertical game FOV used for movement conversion. Missing-key fallback for older configs is `90`. |
| `minSpeedMultiplier` | `0.1` | Minimum movement multiplier. |
| `maxSpeedMultiplier` | `0.1` | Maximum movement multiplier. |
| `predictionInterval` | `0.01` | Prediction time step. |
| `prediction_futurePositions` | `20` | Number of predicted future positions to keep/draw. |
| `draw_futurePositions` | `true` | Draws predicted future positions in supported overlays. |
| `kalman_enabled` | `true` | Enables Kalman smoothing/tracking. |
| `kalman_process_noise_position` | `40.0` | Position process noise. Higher reacts faster but can be less stable. |
| `kalman_process_noise_velocity` | `1800.0` | Velocity process noise. Higher follows quick movement more aggressively. |
| `kalman_measurement_noise` | `35.0` | Detection measurement noise. Higher trusts detections less. |
| `kalman_velocity_damping` | `0.08` | Damps velocity over time. |
| `kalman_max_velocity` | `20000.0` | Caps estimated velocity. |
| `kalman_warmup_frames` | `2` | Frames before Kalman output is considered warmed up. |
| `kalman_compensate_detection_delay` | `true` | Compensates for capture/inference delay. |
| `kalman_additional_prediction_ms` | `0.0` | Extra prediction time in milliseconds. |
| `kalman_reset_timeout_sec` | `0.5` | Resets tracking after this long without detections. |
| `snapRadius` | `1.5` | Close target snap radius. |
| `nearRadius` | `25.0` | Radius where near-target behavior starts. |
| `speedCurveExponent` | `3.0` | Curve shape for speed scaling. |
| `snapBoostFactor` | `1.15` | Extra speed near snap radius. |
| `easynorecoil` | `false` | Enables simple recoil compensation. |
| `easynorecoilstrength` | `0.0` | Recoil compensation strength. |
| `input_method` | `WIN32` | Output/control method. See below. |

## Input Method

Valid values:

```text
WIN32, KMBOX_NET, KMBOX_A, MAKCU
```

| Method | Plain meaning |
|---|---|
| `WIN32` | Standard Windows mouse events. |
| `KMBOX_NET` | Network kmbox control. |
| `KMBOX_A` | kmbox A serial/HID style control. |
| `MAKCU` | MAKCU serial control. |

`WIN32` is the easiest first test, but it uses standard Windows synthetic mouse events. Some games ignore or block that input path. If detection boxes are visible but the game does not react to aim movement or auto-shoot, switch to a supported external input device such as KMBOX or MAKCU, and confirm that method is connected.

## Wind Mouse

| Key | Default | Meaning |
|---|---:|---|
| `wind_mouse_enabled` | `false` | Enables wind-mouse style movement. |
| `wind_G` | `18.0` | Gravity term. |
| `wind_W` | `15.0` | Wind term. |
| `wind_M` | `10.0` | Max step term. |
| `wind_D` | `8.0` | Distance term. |

## Device Control Sections

### Kmbox Net

| Key | Default | Meaning |
|---|---:|---|
| `kmbox_net_ip` | `192.168.2.188` | Device IP address. |
| `kmbox_net_port` | `8808` | Device port. |
| `kmbox_net_uuid` | `0E0A3CAB` | Device UUID/token. |

### Kmbox A

| Key | Default | Meaning |
|---|---:|---|
| `kmbox_a_pidvid` | empty | Combined PID/VID string in `PPPPVVVV` format. |

### MAKCU

| Key | Default | Meaning |
|---|---:|---|
| `makcu_baudrate` | `115200` | Serial baud rate. |
| `makcu_port` | `COM0` | Serial port. |

## Mouse Shooting

| Key | Default | Meaning |
|---|---:|---|
| `auto_shoot` | `false` | Enables automatic shooting behavior. |
| `bScope_multiplier` | `1.0` | Scope multiplier. Missing-key fallback is `1.2` for older configs. |

## AI

| Key | Default | Meaning |
|---|---:|---|
| `backend` | `TRT` | Inference backend. Current builds support `TRT` only. |
| `ai_model` | `Jinn.engine` | Model file. Missing-key fallback for older configs is `sunxds_0.8.0.engine`. |
| `confidence_threshold` | `0.45` | Minimum detection confidence. Clamped `0.1..0.9`. Missing-key fallback is `0.5`. |
| `nms_threshold` | `0.50` | Non-max suppression threshold. Clamped `0.1..0.9`. |
| `max_detections` | `8` | Maximum detections kept per frame. Clamped `1..20`. |
| `export_enable_fp8` | `false` | TensorRT FP8 export option. CUDA builds only. |
| `export_enable_fp16` | `true` | TensorRT FP16 export option. CUDA builds only. |

`fixed_input_size` exists as an internal runtime config field but is not currently written to the generated config file.

## CUDA

These keys are written only in CUDA builds. Since the current build is always CUDA/TensorRT, they are always present.

| Key | Default | Meaning |
|---|---:|---|
| `use_cuda_graph` | `false` | Enables CUDA graph path where supported. |
| `use_pinned_memory` | `false` | Generated default for pinned memory. Missing-key fallback is `true`. |
| `cuda_device_index` | `0` | Index of the GPU device to use. |
| `gpuMemoryReserveMB` | `2048` | GPU memory reserve in megabytes. |
| `enableGpuExclusiveMode` | `true` | Enables exclusive GPU mode behavior where supported by the app. |
| `capture_use_cuda` | `false` | Allows CUDA capture path usage. |

CUDA capture can still create CPU copies when preview, debugging, data collection, or other CPU-readable features need pixels.

## System

| Key | Default | Meaning |
|---|---:|---|
| `cpuCoreReserveCount` | `4` | CPU cores to avoid using heavily. |
| `systemMemoryReserveMB` | `2048` | System memory reserve. |

## Buttons

| Key | Default | Meaning |
|---|---:|---|
| `button_targeting` | `RightMouseButton` | Aim/targeting button list. |
| `button_shoot` | `LeftMouseButton` | Shoot button list. |
| `button_zoom` | `RightMouseButton` | Zoom/scope button list. |
| `button_exit` | `F12` | Exit hotkey. |
| `button_pause` | `None` | Pause hotkey. |
| `button_reload_config` | `None` | Reload config hotkey. |
| `button_open_overlay` | `F10` | Open overlay hotkey. |
| `enable_arrows_settings` | `false` | Enables arrow-key settings behavior. |

Use `None` to disable a button where supported.

## Overlay

| Key | Default | Meaning |
|---|---:|---|
| `overlay_opacity` | `255` | Overlay opacity, `0..255`. |
| `overlay_ui_scale` | `1.0` | Overlay UI scale. |
| `overlay_exclude_from_capture` | `false` | Attempts to keep overlay out of captured frames. |
| `overlay_x` | `0` | Overlay editor window X position. Auto-saved after moving. |
| `overlay_y` | `0` | Overlay editor window Y position. Auto-saved after moving. |
| `overlay_width` | `760` | Overlay editor window width. Auto-saved after resizing. |
| `overlay_height` | `480` | Overlay editor window height. Auto-saved after resizing. |

## Depth

The following depth-related keys are not present in the current config schema. They may appear in experimental or unsupported configs but are not actively loaded or saved by the current build.

| Key | Reference Default | Meaning |
|---|---:|---|
| `depth_inference_enabled` | `true` | Enables depth inference feature. |
| `depth_model_path` | `depth_anything_v2.engine` | Depth model path. |
| `depth_fps` | `100` | Depth update FPS. Minimum `0`. |
| `depth_colormap` | `18` | OpenCV colormap index. Clamped `0..21`. |
| `depth_mask_enabled` | `false` | Enables depth mask. |
| `depth_mask_fps` | `5` | Depth mask update FPS. Minimum `0`. |
| `depth_mask_near_percent` | `20` | Near-depth percent. Clamped `1..100`. |
| `depth_mask_expand` | `0` | Expand mask pixels. Clamped `0..128`. |
| `depth_mask_hold_frames` | `0` | Hold mask for extra frames. Clamped `0..120`. |
| `depth_mask_alpha` | `90` | Mask alpha. Clamped `0..255`. |
| `depth_mask_invert` | `false` | Inverts depth mask. |
| `depth_debug_overlay_enabled` | `false` | Shows depth debug overlay. |

## Game Overlay

| Key | Default | Meaning |
|---|---:|---|
| `game_overlay_enabled` | `false` | Enables in-game overlay rendering. |
| `game_overlay_max_fps` | `0` | Overlay FPS cap. `0` means uncapped/default behavior. |
| `game_overlay_draw_boxes` | `true` | Draw detection boxes. |
| `game_overlay_compensate_latency` | `true` | Shifts overlay boxes/icons using frame age and mouse movement recorded after capture. |
| `game_overlay_draw_future` | `true` | Draw predicted future positions. |
| `game_overlay_draw_wind_tail` | `true` | Draw wind mouse trail. |
| `game_overlay_draw_frame` | `true` | Draw frame border. |
| `game_overlay_draw_circle_fov` | `true` | Draw Circle FOV in the game overlay. |
| `game_overlay_show_target_correction` | `true` | Draw target correction indicator. |
| `game_overlay_box_a/r/g/b` | `255/0/255/0` | Box color as alpha/red/green/blue. |
| `game_overlay_frame_a/r/g/b` | `180/255/255/255` | Frame color as alpha/red/green/blue. |
| `game_overlay_box_thickness` | `2.0` | Detection box line thickness. |
| `game_overlay_frame_thickness` | `1.5` | Frame line thickness. |
| `game_overlay_future_point_radius` | `5.0` | Future-point radius. |
| `game_overlay_future_alpha_falloff` | `1.0` | Future-point alpha falloff. |
| `game_overlay_icon_enabled` | `false` | Enables drawing an icon marker. |
| `game_overlay_icon_path` | `icon.png` | Icon file path. |
| `game_overlay_icon_width` | `64` | Icon width. |
| `game_overlay_icon_height` | `64` | Icon height. |
| `game_overlay_icon_offset_x` | `0.0` | Icon X offset. |
| `game_overlay_icon_offset_y` | `0.0` | Icon Y offset. |
| `game_overlay_icon_anchor` | `center` | Icon anchor: `center`, `top`, `bottom`, or `head`. |
| `game_overlay_icon_class` | `-1` | Class to draw icon for. `-1` means all. |

## Data Collection

| Key | Default | Meaning |
|---|---:|---|
| `collect_data_while_playing` | `false` | Save data while running. |
| `collect_only_when_aimbot_running` | `false` | Collect only while the aimbot is actively running. |
| `collect_only_when_targets_present` | `true` | Collect only frames with targets. |
| `collect_save_every_n_frames` | `300` | Save interval. Clamped `1..600`. |
| `collect_jpeg_quality` | `100` | JPEG quality. Clamped `50..100`. |
| `collect_output_dir` | empty | Output folder. |
| `auto_label_data` | `true` | Write labels automatically. |
| `auto_label_min_conf` | `0.20` | Auto-label confidence. Clamped `0.01..0.99`. |
| `auto_label_max_boxes` | `8` | Auto-label box limit. Clamped `1..200`. |
| `auto_label_record_classes` | empty | Optional class filter list. |

Data collection needs CPU-readable frames, so it can change capture performance diagnostics.

## Classes

Class indexing is driven by the model's output tensor shape. The program infers the class count (NC, valid range 1..19) automatically at startup; do not hardcode class counts. `MAX_CLASSES = 80` is the internal storage capacity of the `class_enabled[]` array and is not the accepted NC range.

Global class enablement (`class_enabled[i]`) is the program-level allow-list. Each of the 3 hotkey profiles maintains its own local class subset, which must be a subset of the global enabled set. Classes not in the global enabled set never participate in target selection, locking, mouse movement, or auto-trigger.

The `class_player` and `class_head` fields are still written and read from the config file for compatibility, but carry no special routing semantics in the current version. Aim offset is controlled exclusively by `aim_offset_x`/`aim_offset_y` in normalized box coordinates. There is no head-specific aim path, no body/head merging logic, and no `disable_headshot` field.

## Debug

| Key | Default | Meaning |
|---|---:|---|
| `show_window` | `true` | Shows debug/preview window. This can require CPU frame copies. |
| `show_fps` | `false` | Shows FPS counter. |
| `screenshot_button` | `None` | Screenshot hotkey. |
| `screenshot_delay` | `500` | Screenshot delay in milliseconds. |
| `verbose` | `false` | Enables more logging. |


## Game Profiles

The active profile is selected by:

```ini
active_game = UNIFIED
```

Profiles are stored under `[Games]`:

```ini
[Games]
UNIFIED = 1,0.022,0.022
```

Format:

```text
name = sensitivity,yaw,pitch[,fovScaled,baseFOV]
```

Examples:

```ini
[Games]
UNIFIED = 1,0.022,0.022
MY_GAME = 2.5,0.02,0.02,true,90
```

If `active_game` is missing or invalid, the app falls back to an available profile.
