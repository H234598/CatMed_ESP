#ifndef KATMED_DISPLAY_UI_MAILBOX_H
#define KATMED_DISPLAY_UI_MAILBOX_H
#include "katmed_ui.h"
#ifdef __cplusplus
#define DISPLAY_MB_ZERO {}
#else
#define DISPLAY_MB_ZERO {0}
#endif
#define DISPLAY_MB_PAYLOAD .state=0,.preview_pending=false,.preview_value=0,.sequence=0,.request=DISPLAY_MB_ZERO,.result=UI_SUBMIT_UNAVAILABLE,.settings=DISPLAY_MB_ZERO,.night=DISPLAY_MB_ZERO
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#define DISPLAY_MB_MUTEX portMUX_TYPE
#define DISPLAY_MB_INITIALIZER {.lock=portMUX_INITIALIZER_UNLOCKED,DISPLAY_MB_PAYLOAD}
#define DISPLAY_MB_LOCK(m) portENTER_CRITICAL(&(m)->lock)
#define DISPLAY_MB_UNLOCK(m) portEXIT_CRITICAL(&(m)->lock)
#else
#include <pthread.h>
#define DISPLAY_MB_MUTEX pthread_mutex_t
#define DISPLAY_MB_INITIALIZER {.lock=PTHREAD_MUTEX_INITIALIZER,DISPLAY_MB_PAYLOAD}
#define DISPLAY_MB_LOCK(m) pthread_mutex_lock(&(m)->lock)
#define DISPLAY_MB_UNLOCK(m) pthread_mutex_unlock(&(m)->lock)
#endif
/* One bounded UI intent, like the existing app-loop bridges. Only POD copies
 * under this lock: never Store, transport, LVGL, callbacks or allocation. */
typedef struct {
    DISPLAY_MB_MUTEX lock;
    unsigned state; /* 0 idle,1 queued,2 app-loop processing,3 result awaiting UI */
    bool preview_pending;
    uint8_t preview_value; /* 0 cancel; latest value replaces prior unconsumed preview */
    uint32_t sequence;
    UiRequest request;
    UiSubmitResult result;
    KatmedDisplaySettings settings;
    KatmedNightCopyStatus night;
} DisplayUiMailbox;
static inline bool display_mb_preview(DisplayUiMailbox *m,uint8_t value)
{
    if(value>100)return false;
    DISPLAY_MB_LOCK(m);
    if(value && m->state){DISPLAY_MB_UNLOCK(m);return false;}
    m->preview_value=value;m->preview_pending=true;
    DISPLAY_MB_UNLOCK(m);return true;
}
static inline bool display_mb_take_preview(DisplayUiMailbox *m,uint8_t *value)
{
    DISPLAY_MB_LOCK(m);
    const bool pending=m->preview_pending;
    if(pending){*value=m->preview_value;m->preview_pending=false;}
    DISPLAY_MB_UNLOCK(m);return pending;
}
static inline bool display_mb_submit(DisplayUiMailbox *m,const UiRequest *r)
{
    DISPLAY_MB_LOCK(m);
    if(m->state){DISPLAY_MB_UNLOCK(m);return false;}
    if(++m->sequence==0)++m->sequence;
    if(r->kind==UI_REQUEST_SET_BRIGHTNESS){m->preview_value=0;m->preview_pending=true;}
    m->request=*r;m->state=1;DISPLAY_MB_UNLOCK(m);return true;
}
static inline bool display_mb_take(DisplayUiMailbox *m,UiRequest *r,uint32_t *sequence)
{
    DISPLAY_MB_LOCK(m);
    if(m->state!=1){DISPLAY_MB_UNLOCK(m);return false;}
    *r=m->request;*sequence=m->sequence;m->state=2;
    DISPLAY_MB_UNLOCK(m);return true;
}
static inline bool display_mb_complete(DisplayUiMailbox *m,uint32_t sequence,UiSubmitResult result)
{
    DISPLAY_MB_LOCK(m);
    bool match=m->state==2 && m->sequence==sequence;
    if(match){m->result=result;m->state=3;}
    DISPLAY_MB_UNLOCK(m);return match;
}
static inline bool display_mb_result(DisplayUiMailbox *m,UiRequest *r,UiSubmitResult *result)
{
    DISPLAY_MB_LOCK(m);
    if(m->state!=3){DISPLAY_MB_UNLOCK(m);return false;}
    *r=m->request;*result=m->result;m->state=0;
    DISPLAY_MB_UNLOCK(m);return true;
}
static inline void display_mb_publish(DisplayUiMailbox *m,KatmedDisplaySettings settings,KatmedNightCopyStatus night)
{
    DISPLAY_MB_LOCK(m);m->settings=settings;m->night=night;DISPLAY_MB_UNLOCK(m);
}
static inline void display_mb_snapshot(DisplayUiMailbox *m,KatmedDisplaySettings *settings,KatmedNightCopyStatus *night)
{
    DISPLAY_MB_LOCK(m);if(settings)*settings=m->settings;if(night)*night=m->night;DISPLAY_MB_UNLOCK(m);
}
#endif
