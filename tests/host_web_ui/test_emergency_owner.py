"""Actual placeholder touch producer -> registered Main dispatch/mailbox/owner."""
import os
from pathlib import Path
import tempfile
import unittest
from test_network_startup import function, execute_tls_callback

ROOT = Path(__file__).resolve().parents[2]
PREFIX = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "katmed_ui.h"
#include "katmed_core.h"
#include "katmed_morse.h"
#define T(s) (s)
#define LV_STATE_DISABLED 1
typedef struct {int unused;} lv_obj_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
'''
# These doubles are graphics/unselected-view boundaries, not emergency policy.
STUBS = r'''
static const char *feedback;
static unsigned feedback_calls;
static unsigned observed_feedback, unavailable_feedback, ordinary_feedback;
static void show_feedback(const char *text){
    feedback=text;++feedback_calls;
    if(!strcmp(text,"Signal erfasst"))++observed_feedback;
    if(!strcmp(text,"Speichern derzeit nicht verfügbar"))++unavailable_feedback;
    if(!strcmp(text,"Gespeichert"))++ordinary_feedback;
}
static void update_status(void){assert(false);}
static void build_settings(void){assert(false);}
static void lv_obj_delete(lv_obj_t *obj){(void)obj;assert(false);}
static void lv_obj_move_foreground(lv_obj_t *obj){(void)obj;assert(false);}
static void lv_obj_remove_state(lv_obj_t *obj,int state){(void)obj;(void)state;assert(false);}
static void sync_brightness_slider(void){assert(false);}
static void lv_obj_get_coords(lv_obj_t *obj,lv_area_t *area){assert(obj==ui.emergency_button);*area=(lv_area_t){20,20,80,80};}
static unsigned notice_kind(void){return 0;}
static uint64_t notice_id(unsigned kind){(void)kind;assert(false);return 0;}
static void return_to_settings(uint64_t now){(void)now;assert(false);}
static bool interaction_input(KatmedInteractionInputKind kind,uint64_t target,uint64_t impulse,uint64_t grant,uint64_t now){(void)kind;(void)target;(void)impulse;(void)grant;(void)now;assert(false);return false;}
static void interaction_cancel(uint64_t now){(void)now;assert(false);}
static bool send_hit(int x,int y){(void)x;(void)y;return false;}
static void finish_morse_start_contact(void){assert(false);}
void katmed_morse_stop(KatmedMorse *morse){(void)morse;assert(false);}
static void lv_obj_set_hidden(lv_obj_t *obj,bool hidden){(void)obj;(void)hidden;assert(false);}
static uint32_t lv_color_hex(uint32_t value){return value;}
static void lv_obj_set_style_bg_color(lv_obj_t *obj,uint32_t value,unsigned selector){(void)obj;(void)value;(void)selector;assert(false);}
static void cancel_brightness_preview(void){assert(false);}
static void cancel_edit(void *event){(void)event;assert(false);}
static void destroy_menu(void){assert(false);}
static void change_page(int dx,int dy,uint64_t now){(void)dx;(void)dy;(void)now;assert(false);}
static bool at_edge(int y){(void)y;return false;}
static bool brightness_hit(int16_t x,int16_t y){(void)x;(void)y;return false;}
'''
SUFFIX = r'''
void host_emergency_attach(const UiServices *services){
    ui=(UiContext){.view=UI_VIEW_PLACEHOLDER,.touch_armed=true,.width=172,.height=320,.emergency_button=(lv_obj_t*)1};
    ui_set_services(services);feedback=NULL;feedback_calls=0;observed_feedback=unavailable_feedback=ordinary_feedback=0;
}
void host_emergency_press(void){
    static uint64_t now=1000;now+=1000;
    ui_touch_sample(false,50,50,true,now);
    ui_touch_sample(true,50,50,true,now+10);
    ui_touch_sample(false,50,50,true,now+110);
}
void host_emergency_duplicate_release(void){ui_touch_sample(false,50,50,true,UINT64_C(1000000));}
void host_emergency_invalid_press(void){
    ui_touch_sample(false,50,50,true,UINT64_C(2000000));
    ui_touch_sample(true,50,50,true,UINT64_C(2000010));
    ui_touch_sample(false,50,50,true,UINT64_C(2000020)); /* below actual 30ms minimum */
}
const char *host_emergency_feedback(void){return feedback;}
unsigned host_emergency_feedback_calls(void){return feedback_calls;}
unsigned host_emergency_observed_feedback(void){return observed_feedback;}
unsigned host_emergency_failed_feedback(void){return unavailable_feedback;}
unsigned host_emergency_ordinary_feedback(void){return ordinary_feedback;}
'''


def fixture(text):
    # Include the actual context/enums, actual public signatures and whole
    # production touch/submit/feedback functions. No hand-copied button logic.
    start = text.index('typedef enum { EDIT_NONE')
    end = text.index('} UiContext;', start) + len('} UiContext;')
    names = ['void ui_set_services(', 'static bool same_round(',
             'static const char *result_text(', 'static UiSubmitResult submit(const',
             'static UiSubmitResult request_action(', 'void ui_deferred_result(',
             'void ui_touch_sample(']
    return PREFIX + text[start:end] + '\nstatic UiContext ui;\n' + STUBS + '\n'.join(function(text, name) for name in names) + SUFFIX


class EmergencyOwner(unittest.TestCase):
    def test_real_placeholder_touch_and_Main_owner_in_both_targets(self):
        with tempfile.TemporaryDirectory(prefix='katmed-emergency-owner-') as tmp:
            source = Path(tmp) / 'actual_emergency_ui.c'
            source.write_text(fixture((ROOT / 'firmware/components/katmed_ui/ui.c').read_text()))
            # The test runner supplies only a source path in its own temporary
            # directory; no device, credentials or external command arguments.
            for chip in ('3', '6'):
                with self.subTest(chip=chip):
                    prior_source = os.environ.get('KATMED_TEST_EMERGENCY_UI_SOURCE')
                    prior_chip = os.environ.get('KATMED_TEST_CHIP')
                    os.environ['KATMED_TEST_EMERGENCY_UI_SOURCE'] = str(source)
                    os.environ['KATMED_TEST_CHIP'] = chip
                    try:
                        print(execute_tls_callback(['bash', str(ROOT / 'tests/host_store/check_main_submit.sh'), 'emergency_owner']))
                    finally:
                        for key, value in [('KATMED_TEST_EMERGENCY_UI_SOURCE', prior_source), ('KATMED_TEST_CHIP', prior_chip)]:
                            if value is None:
                                os.environ.pop(key, None)
                            else:
                                os.environ[key] = value
