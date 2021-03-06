#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wlr/util/log.h>
#include "backend/drm.h"
#include "backend/drm-util.h"

struct atomic {
	drmModeAtomicReq *req;
	int cursor;
	bool failed;
};

static void atomic_begin(struct wlr_drm_crtc *crtc, struct atomic *atom) {
	if (!crtc->atomic) {
		crtc->atomic = drmModeAtomicAlloc();
		if (!crtc->atomic) {
			wlr_log_errno(L_ERROR, "Allocation failed");
			atom->failed = true;
			return;
		}
	}

	atom->req = crtc->atomic;
	atom->cursor = drmModeAtomicGetCursor(atom->req);
	atom->failed = false;
}

static bool atomic_end(int drm_fd, struct atomic *atom) {
	if (atom->failed) {
		return false;
	}

	uint32_t flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK;

	if (drmModeAtomicCommit(drm_fd, atom->req, flags, NULL)) {
		wlr_log_errno(L_ERROR, "Atomic test failed");
		drmModeAtomicSetCursor(atom->req, atom->cursor);
		return false;
	}

	return true;
}

static bool atomic_commit(int drm_fd, struct atomic *atom, struct wlr_output_state *output,
		uint32_t flag) {
	if (atom->failed) {
		return false;
	}

	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK | flag;

	int ret = drmModeAtomicCommit(drm_fd, atom->req, flags, output);
	if (ret) {
		wlr_log_errno(L_ERROR, "Atomic commit failed");
	}

	drmModeAtomicSetCursor(atom->req, 0);

	return !ret;
}

static inline void atomic_add(struct atomic *atom, uint32_t id, uint32_t prop, uint64_t val) {
	if (!atom->failed && drmModeAtomicAddProperty(atom->req, id, prop, val) < 0) {
		wlr_log_errno(L_ERROR, "Failed to add atomic DRM property");
		atom->failed = true;
	}
}

static void set_plane_props(struct atomic *atom, struct wlr_drm_plane *plane,
		uint32_t crtc_id, uint32_t fb_id, bool set_crtc_xy) {
	uint32_t id = plane->id;
	const union wlr_drm_plane_props *props = &plane->props;

	// The src_* properties are in 16.16 fixed point
	atomic_add(atom, id, props->src_x, 0);
	atomic_add(atom, id, props->src_y, 0);
	atomic_add(atom, id, props->src_w, plane->width << 16);
	atomic_add(atom, id, props->src_h, plane->height << 16);
	atomic_add(atom, id, props->crtc_w, plane->width);
	atomic_add(atom, id, props->crtc_h, plane->height);
	atomic_add(atom, id, props->fb_id, fb_id);
	atomic_add(atom, id, props->crtc_id, crtc_id);
	if (set_crtc_xy) {
		atomic_add(atom, id, props->crtc_x, 0);
		atomic_add(atom, id, props->crtc_y, 0);
	}
}

static bool atomic_crtc_pageflip(struct wlr_drm_backend *backend,
		struct wlr_output_state *output,
		struct wlr_drm_crtc *crtc,
		uint32_t fb_id, drmModeModeInfo *mode) {
	if (mode) {
		if (crtc->mode_id) {
			drmModeDestroyPropertyBlob(backend->fd, crtc->mode_id);
		}

		if (drmModeCreatePropertyBlob(backend->fd, mode, sizeof(*mode), &crtc->mode_id)) {
			wlr_log_errno(L_ERROR, "Unable to create property blob");
			return false;
		}
	}

	struct atomic atom;

	atomic_begin(crtc, &atom);
	atomic_add(&atom, output->connector, output->props.crtc_id, crtc->id);
	atomic_add(&atom, crtc->id, crtc->props.mode_id, crtc->mode_id);
	atomic_add(&atom, crtc->id, crtc->props.active, 1);
	set_plane_props(&atom, crtc->primary, crtc->id, fb_id, true);
	return atomic_commit(backend->fd, &atom,
			output, mode ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0);
}

static void atomic_conn_enable(struct wlr_drm_backend *backend,
		struct wlr_output_state *output, bool enable) {
	struct wlr_drm_crtc *crtc = output->crtc;
	struct atomic atom;

	atomic_begin(crtc, &atom);
	atomic_add(&atom, crtc->id, crtc->props.active, enable);
	atomic_end(backend->fd, &atom);
}

bool legacy_crtc_set_cursor(struct wlr_drm_backend *backend,
		struct wlr_drm_crtc *crtc, struct gbm_bo *bo);

static bool atomic_crtc_set_cursor(struct wlr_drm_backend *backend,
		struct wlr_drm_crtc *crtc, struct gbm_bo *bo) {
	if (!crtc || !crtc->cursor) {
		return true;
	}

	struct wlr_drm_plane *plane = crtc->cursor;
	// We can't use atomic operations on fake planes
	if (plane->id == 0) {
		return legacy_crtc_set_cursor(backend, crtc, bo);
	}

	struct atomic atom;

	atomic_begin(crtc, &atom);

	if (bo) {
		set_plane_props(&atom, plane, crtc->id, get_fb_for_bo(bo), false);
	} else {
		atomic_add(&atom, plane->id, plane->props.fb_id, 0);
		atomic_add(&atom, plane->id, plane->props.crtc_id, 0);
	}

	return atomic_end(backend->fd, &atom);
}

bool legacy_crtc_move_cursor(struct wlr_drm_backend *backend,
		struct wlr_drm_crtc *crtc, int x, int y);

static bool atomic_crtc_move_cursor(struct wlr_drm_backend *backend,
		struct wlr_drm_crtc *crtc, int x, int y) {
	struct wlr_drm_plane *plane = crtc->cursor;
	// We can't use atomic operations on fake planes
	if (plane->id == 0) {
		return legacy_crtc_move_cursor(backend, crtc, x, y);
	}

	struct atomic atom;

	atomic_begin(crtc, &atom);
	atomic_add(&atom, plane->id, plane->props.crtc_x, x);
	atomic_add(&atom, plane->id, plane->props.crtc_y, y);
	return atomic_end(backend->fd, &atom);
}

const struct wlr_drm_interface atomic_iface = {
	.conn_enable = atomic_conn_enable,
	.crtc_pageflip = atomic_crtc_pageflip,
	.crtc_set_cursor = atomic_crtc_set_cursor,
	.crtc_move_cursor = atomic_crtc_move_cursor,
};
