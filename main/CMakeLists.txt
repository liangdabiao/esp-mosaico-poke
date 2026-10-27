idf_component_register(
    SRCS "main.c" "game.c"
         "pk/assets.c" "pk/battle.c" "pk/battle_escape.c" "pk/capture.c"
         "pk/combat.c" "pk/crc32.c" "pk/encounter.c" "pk/evolution.c"
         "pk/exp.c" "pk/items.c" "pk/nurture.c" "pk/party.c"
         "pk/pokemon_names.c" "pk/pk_assets_device.c" "pk/pk_draw.c" "pk/pk_map.c" "pk/pk_world.c"
         "pk/pk_store.c"
         "pkaudio/wq_sfx.c" "pkaudio/rtttl_player.c" "pkaudio/wq_volume.c"
    INCLUDE_DIRS "." "pk"
    # bsp: ES8311 音频（wq_sfx/rtttl_player）；nvs_flash: 存档。
    REQUIRES mosaico_game_app mosaico_raylib_fast esp-gsp nvs_flash
             esp-mosaico-bsp esp_codec_dev)

# 游戏数据资产：7 个 .bin 链进 flash（pk_assets_device.c 按文件名推符号）。
set(PK_ASSETS gen1.bin gen1_front.bin gen1_back.bin palettes.bin ui.bin
              moves.bin font16.bin)
foreach(asset IN LISTS PK_ASSETS)
    target_add_binary_data(${COMPONENT_LIB} "assets/${asset}" BINARY)
endforeach()

# GSP needs an image to bind the Raylib framebuffer. This generated placeholder
# is embedded in the application; no external game resource partition is used.
set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas.png")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py"
    "${canvas_png}" RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the game canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("scene.json.in" "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)
