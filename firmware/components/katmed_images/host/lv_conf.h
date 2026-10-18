#ifndef LV_CONF_H
#define LV_CONF_H

/* The host exercise uses the same pinned TJpgD tile decoder as firmware. */
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (64 * 1024U)
#define LV_USE_TJPGD 1
#define LV_CACHE_DEF_SIZE 0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
