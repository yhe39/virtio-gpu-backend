#ifndef __VDISPLAY_PROTOCOL_H__
#define __VDISPLAY_PROTOCOL_H__

enum dpy_evt_type {
    DPY_EVENT_SURFACE_SET   =0x100,
    DPY_EVENT_SURFACE_UPDATE,
    DPY_EVENT_SET_MODIFIER,
    DPY_EVENT_CURSOR_DEFINE,
    DPY_EVENT_CURSOR_MOVE,
    DPY_EVENT_DISPLAY_INFO,
    DPY_EVENT_HOTPLUG,
    DPY_EVENT_EXIT
};

#define DISPLAY_MAGIC_CODE  0x5566


struct dpy_evt_header {
    int e_type;
    int e_magic;
    int e_size;
};

#endif  /* __VDISPLAY_PROTOCOL_H__ */
