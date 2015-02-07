


#ifndef _DRM_SAREA_H_
#define _DRM_SAREA_H_

#include "drm.h"

/* SAREA area needs to be at least a page */
#if defined(__alpha__)
#define SAREA_MAX                       0x2000U
#elif defined(__ia64__)
#define SAREA_MAX                       0x10000U	/* 64kB */
#else
/* Intel 830M driver needs at least 8k SAREA */
#define SAREA_MAX                       0x2000U
#endif

/** Maximum number of drawables in the SAREA */
#define SAREA_MAX_DRAWABLES		256

#define SAREA_DRAWABLE_CLAIMED_ENTRY    0x80000000

/** SAREA drawable */
struct drm_sarea_drawable {
	unsigned int stamp;
	unsigned int flags;
};

/** SAREA frame */
struct drm_sarea_frame {
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
	unsigned int fullscreen;
};

/** SAREA */
struct drm_sarea {
    /** first thing is always the DRM locking structure */
	struct drm_hw_lock lock;
    /** \todo Use readers/writer lock for drm_sarea::drawable_lock */
	struct drm_hw_lock drawable_lock;
	struct drm_sarea_drawable drawableTable[SAREA_MAX_DRAWABLES];	/**< drawables */
	struct drm_sarea_frame frame;	/**< frame */
	drm_context_t dummy_context;
};

#ifndef __KERNEL__
typedef struct drm_sarea_drawable drm_sarea_drawable_t;
typedef struct drm_sarea_frame drm_sarea_frame_t;
typedef struct drm_sarea drm_sarea_t;
#endif

#endif				/* _DRM_SAREA_H_ */