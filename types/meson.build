lib_wlr_types = static_library('wlr_types', [
        'wlr_input_device.c',
        'wlr_keyboard.c',
        'wlr_output.c',
        'wlr_pointer.c',
        'wlr_region.c',
        'wlr_surface.c',
        'wlr_tablet_pad.c',
        'wlr_tablet_tool.c',
        'wlr_touch.c',
        'wlr_xdg_shell_v6.c',
    ],
    include_directories: wlr_inc,
    dependencies: [wayland_server, pixman, wlr_protos])
