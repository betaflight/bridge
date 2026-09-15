# Post-project build tweaks for this board (see the top-level CMakeLists.txt).

# The BSP sources use memcpy without including <string.h>; force the include
# rather than patching the fetched component.
idf_build_get_property(_components BUILD_COMPONENTS)
if("waveshare__esp32_s3_touch_lcd_4b" IN_LIST _components)
    idf_component_get_property(_bsp_lib waveshare__esp32_s3_touch_lcd_4b COMPONENT_LIB)
    target_compile_options(${_bsp_lib} PRIVATE "SHELL:-include string.h")
endif()
