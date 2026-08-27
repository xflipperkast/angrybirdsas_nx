#include "input.h"
#include "jni_fake.h"
#include "android_ndk.h"
#include <switch.h>
#include <GLES2/gl2.h>
#include <time.h>
#include <string.h>

#define TOUCH_INVALID_FINGER_ID UINT32_MAX
#define CURSOR_SPEED_PX 14.0f

static PadState g_pad;
static float g_cursor_x=640.0f,g_cursor_y=360.0f;
static int g_controller_pressed;
static int64_t g_pointer_down_ms;
static int g_cursor_visible=-1;
static int g_previous_docked_state=-1;
static int g_touch_active;
static u32 g_touch_finger_id=TOUCH_INVALID_FINGER_ID;
static float g_touch_x,g_touch_y;

static int64_t now_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static int is_docked(void){
    return appletGetOperationMode()==AppletOperationMode_Console;
}

void input_init(void){
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    g_cursor_x=(float)android_width()*0.5f;
    g_cursor_y=(float)android_height()*0.5f;
}

static void inject_pointer_event(UnityInjectFn fn,void*env,void*thiz,int action,float x,float y){
    if(!fn)return;
    int64_t n=now_ms();
    if(action==0)g_pointer_down_ms=n;
    FakeMotionEvent e={0};
    e.action=action;
    e.x=x;
    e.y=y;
    e.pressure=action==1?0.0f:1.0f;
    e.size=0.08f;
    e.source=0x1002;
    e.down_time_ms=g_pointer_down_ms;
    e.event_time_ms=n;
    void*obj=jni_make_motion_event(&e);
    (void)fn(env,thiz,obj,0);
}

static int update_touch_input(UnityInjectFn fn,void *env,void *thiz){
    HidTouchScreenState ts={0};
    hidGetTouchScreenStates(&ts,1);
    const int was_active=g_touch_active;
    int match=-1;

    if(g_touch_active){
        for(int i=0;i<ts.count;i++){
            if(ts.touches[i].finger_id==g_touch_finger_id){match=i;break;}
        }
    }else if(ts.count>0){

        if(g_controller_pressed){
            inject_pointer_event(fn,env,thiz,1,g_cursor_x,g_cursor_y);
            g_controller_pressed=0;
        }
        match=0;
        g_touch_active=1;
        g_touch_finger_id=ts.touches[0].finger_id;
    }

    if(g_touch_active&&match>=0){
        float x=(float)ts.touches[match].x*(float)android_width()/1280.0f;
        float y=(float)ts.touches[match].y*(float)android_height()/720.0f;
        float maxx=(float)android_width()-1.0f,maxy=(float)android_height()-1.0f;
        if(x<0)x=0;
        if(y<0)y=0;
        if(x>maxx)x=maxx;
        if(y>maxy)y=maxy;
        inject_pointer_event(fn,env,thiz,was_active?2:0,x,y);
        g_touch_x=x;g_touch_y=y;g_cursor_x=x;g_cursor_y=y;
    }else if(was_active){
        inject_pointer_event(fn,env,thiz,1,g_touch_x,g_touch_y);
        g_touch_active=0;
        g_touch_finger_id=TOUCH_INVALID_FINGER_ID;
    }

    return was_active||g_touch_active;
}

static void update_controller_cursor(UnityInjectFn fn,void *env,void *thiz,u64 held,u64 down,u64 up){
    HidAnalogStickState st=padGetStickPos(&g_pad,0);
    g_cursor_x+=((float)st.x/32767.0f)*CURSOR_SPEED_PX;
    g_cursor_y-=((float)st.y/32767.0f)*CURSOR_SPEED_PX;

    float maxx=(float)android_width()-1.0f,maxy=(float)android_height()-1.0f;
    if(g_cursor_x<0)g_cursor_x=0;
    if(g_cursor_y<0)g_cursor_y=0;
    if(g_cursor_x>maxx)g_cursor_x=maxx;
    if(g_cursor_y>maxy)g_cursor_y=maxy;

    if(down&HidNpadButton_A){
        inject_pointer_event(fn,env,thiz,0,g_cursor_x,g_cursor_y);
        g_controller_pressed=1;
    }else if(g_controller_pressed&&(held&HidNpadButton_A)){

        inject_pointer_event(fn,env,thiz,2,g_cursor_x,g_cursor_y);
    }
    if((up&HidNpadButton_A)&&g_controller_pressed){
        inject_pointer_event(fn,env,thiz,1,g_cursor_x,g_cursor_y);
        g_controller_pressed=0;
    }
}

void input_feed(UnityInjectFn fn,void*env,void*thiz){
    padUpdate(&g_pad);
    const u64 held=padGetButtons(&g_pad);
    const u64 down=padGetButtonsDown(&g_pad);
    const u64 up=padGetButtonsUp(&g_pad);
    const int docked=is_docked();

    if(docked!=g_previous_docked_state){
        if(g_cursor_visible<0||docked)g_cursor_visible=docked?1:0;
        g_previous_docked_state=docked;
    }

    if(down&HidNpadButton_Plus){
        g_cursor_visible=1;

    }
    if(down&HidNpadButton_Minus){
        if(g_controller_pressed){
            inject_pointer_event(fn,env,thiz,1,g_cursor_x,g_cursor_y);
            g_controller_pressed=0;
        }
        g_cursor_visible=0;

    }

    const int touch_busy=!docked?update_touch_input(fn,env,thiz):0;
    if(!touch_busy&&g_cursor_visible>0){
        update_controller_cursor(fn,env,thiz,held,down,up);
    }
}

int input_cursor_visible(void){return g_cursor_visible>0;}

static void draw_cursor_rect(int x,int y_top,int w,int h,int sw,int sh,float r,float g,float b){
    if(w<=0||h<=0||x>=sw||y_top>=sh||x+w<=0||y_top+h<=0)return;
    if(x<0){w+=x;x=0;}
    if(y_top<0){h+=y_top;y_top=0;}
    if(x+w>sw)w=sw-x;
    if(y_top+h>sh)h=sh-y_top;
    if(w<=0||h<=0)return;
    glScissor(x,sh-(y_top+h),w,h);
    glClearColor(r,g,b,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void input_draw_cursor_overlay(void){
    if(g_cursor_visible<=0)return;
    const int sw=android_width(),sh=android_height();
    if(sw<=0||sh<=0)return;

    GLint old_fb=0,old_scissor[4]={0};
    GLfloat old_clear[4]={0};
    GLboolean old_mask[4]={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
    const GLboolean old_scissor_enabled=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&old_fb);
    glGetIntegerv(GL_SCISSOR_BOX,old_scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,old_clear);
    glGetBooleanv(GL_COLOR_WRITEMASK,old_mask);

    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);

    const int x=(int)(g_cursor_x+0.5f),y=(int)(g_cursor_y+0.5f);

    draw_cursor_rect(x-12,y-3,25,7,sw,sh,0.0f,0.0f,0.0f);
    draw_cursor_rect(x-3,y-12,7,25,sw,sh,0.0f,0.0f,0.0f);
    draw_cursor_rect(x-10,y-1,21,3,sw,sh,1.0f,1.0f,1.0f);
    draw_cursor_rect(x-1,y-10,3,21,sw,sh,1.0f,1.0f,1.0f);

    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)old_fb);
    glColorMask(old_mask[0],old_mask[1],old_mask[2],old_mask[3]);
    glClearColor(old_clear[0],old_clear[1],old_clear[2],old_clear[3]);
    glScissor(old_scissor[0],old_scissor[1],old_scissor[2],old_scissor[3]);
    if(!old_scissor_enabled)glDisable(GL_SCISSOR_TEST);
}
