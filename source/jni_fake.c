#include "jni_fake.h"
#include "config.h"
#include "installer.h"
#include "android_ndk.h"
#include "trace_log.h"
#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

enum { FK_CLASS=1, FK_OBJECT, FK_STRING, FK_METHOD, FK_FIELD, FK_DIRECT, FK_ARRAY, FK_MOTION };
typedef struct FakeObj {
    uint32_t magic, kind;
    const char *class_name;
    char *name;
    char *sig;
    void *data;
    size_t size;
    FakeMotionEvent motion;
} FakeObj;

#define F_MAGIC 0x4a4e4958u
#define MAX_FAKE 4096
static FakeObj *g_objs[MAX_FAKE];
static int g_obj_count;
static Mutex g_obj_lock;
static int g_ready;
static void *g_env_table[235];
static void *g_vm_table[8];
static void **g_env_iface = g_env_table;
static void **g_vm_iface = g_vm_table;
void *fake_env = &g_env_iface;
void *fake_vm = &g_vm_iface;
void *fake_context_obj;
void *fake_unityplayer_thiz;
void *fake_surface_obj;
volatile int jni_quit_requested;

static FakeObj *as_obj(void *p) {
    FakeObj *o=(FakeObj*)p;
    return o && o->magic==F_MAGIC ? o : NULL;
}
static char *xstrdup(const char *s){ if(!s)s=""; size_t n=strlen(s)+1; char *p=malloc(n); if(p)memcpy(p,s,n); return p; }
static FakeObj *obj_new(int kind,const char *cls,const char *name,const char *sig){
    FakeObj *o=calloc(1,sizeof(*o)); if(!o)return NULL;
    o->magic=F_MAGIC;o->kind=kind;o->class_name=cls?xstrdup(cls):NULL;o->name=name?xstrdup(name):NULL;o->sig=sig?xstrdup(sig):NULL;
    mutexLock(&g_obj_lock); if(g_obj_count<MAX_FAKE)g_objs[g_obj_count++]=o; mutexUnlock(&g_obj_lock);
    return o;
}
static void *make_object(const char *cls){return obj_new(FK_OBJECT,cls,NULL,NULL);}
static void *make_class(const char *cls){return obj_new(FK_CLASS,cls,cls,NULL);}
static const char *class_name(void *p){FakeObj*o=as_obj(p);return o&&o->class_name?o->class_name:"java/lang/Object";}

void *jni_new_string(const char *s){return obj_new(FK_STRING,"java/lang/String",s?s:"",NULL);}
const char *jni_string_utf8(void *p){FakeObj*o=as_obj(p);return o&&o->kind==FK_STRING&&o->name?o->name:"";}

static int utf16_length_utf8(const char *s){
    const unsigned char *p=(const unsigned char*)(s?s:"");
    size_t units=0;
    while(*p){
        unsigned c=*p;
        if(c<0x80){p+=1;units+=1;}
        else if((c&0xe0)==0xc0 && p[1] && (p[1]&0xc0)==0x80){p+=2;units+=1;}
        else if((c&0xf0)==0xe0 && p[1] && p[2] && (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80){p+=3;units+=1;}
        else if((c&0xf8)==0xf0 && p[1] && p[2] && p[3] && (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80 && (p[3]&0xc0)==0x80){p+=4;units+=2;}
        else{p+=1;units+=1;}
        if(units>0x7fffffffU)return 0x7fffffff;
    }
    return (int)units;
}
void *jni_new_direct_buffer(void *ptr,size_t cap){FakeObj*o=obj_new(FK_DIRECT,"java/nio/DirectByteBuffer",NULL,NULL);if(o){o->data=ptr;o->size=cap;}return o;}
void *jni_make_motion_event(const FakeMotionEvent *ev){static FakeObj*o;if(!o)o=obj_new(FK_MOTION,"android/view/MotionEvent",NULL,NULL);if(o&&ev)o->motion=*ev;return o;}
void jni_request_quit(void){jni_quit_requested=1;}

static uintptr_t j_stub_u(void *env,...){(void)env;return 0;}
static void j_stub_v(void *env,...){(void)env;}
static float j_stub_f(void *env,...){(void)env;return 0.0f;}
static double j_stub_d(void *env,...){(void)env;return 0.0;}
static int j_GetVersion(void *env){(void)env;return JNI_VERSION_1_6;}
static void *j_FindClass(void *env,const char *name){(void)env;return make_class(name?name:"java/lang/Object");}
static void *j_NewGlobalRef(void*e,void*o){(void)e;return o;} static void j_DeleteRef(void*e,void*o){(void)e;(void)o;}
static int j_IsSameObject(void*e,void*a,void*b){(void)e;return a==b;} static void *j_NewLocalRef(void*e,void*o){(void)e;return o;}
static void *j_GetObjectClass(void*e,void*o){(void)e;return make_class(class_name(o));}
static int j_IsInstanceOf(void*e,void*o,void*c){
    (void)e;
    if(!o||!c)return 0;
    FakeObj *obj=as_obj(o), *cls=as_obj(c);

    if(obj&&obj->kind==FK_MOTION){
        const char *target=cls&&cls->class_name?cls->class_name:"";
        int yes=!strcmp(target,"android/view/MotionEvent") ||
                !strcmp(target,"android/view/InputEvent") ||
                !strcmp(target,"java/lang/Object");
        return yes;
    }

    return o!=NULL;
}
static void *j_GetMethodID(void*e,void*c,const char*n,const char*s){
    (void)e;const char *owner=class_name(c);
    if(owner&&!strcmp(owner,"android/view/MotionEvent")){
    }
    return obj_new(FK_METHOD,owner,n?n:"",s?s:"");
}
static void *j_GetStaticMethodID(void*e,void*c,const char*n,const char*s){
    return j_GetMethodID(e,c,n,s);
}
static void *j_GetFieldID(void*e,void*c,const char*n,const char*s){(void)e;return obj_new(FK_FIELD,class_name(c),n?n:"",s?s:"");}
static void *j_GetStaticFieldID(void*e,void*c,const char*n,const char*s){return j_GetFieldID(e,c,n,s);}

static void *file_obj(const char *path){FakeObj*o=obj_new(FK_OBJECT,"java/io/File",path,NULL);return o;}
static void *object_result(void *obj, FakeObj *mid, const JValue *args){
    const char *n=mid&&mid->name?mid->name:""; const char *cls=mid&&mid->class_name?mid->class_name:"";
    if(!strcmp(n,"getApplicationContext")||!strcmp(n,"getApplication")||!strcmp(n,"getBaseContext")) return fake_context_obj;
    if(!strcmp(n,"getPackageName")) {
        const char *pkg=installer_package_name();
        return jni_new_string(pkg?pkg:GAME_PACKAGE);
    }
    if(!strcmp(n,"getPackageManager")) return make_object("android/content/pm/PackageManager");
    if(!strcmp(n,"getApplicationInfo")) return make_object("android/content/pm/ApplicationInfo");
    if(!strcmp(n,"getPackageInfo")) return make_object("android/content/pm/PackageInfo");
    if(!strcmp(n,"getPackageCodePath")||!strcmp(n,"getPackageResourcePath")||!strcmp(n,"getSourceDir"))
        return jni_new_string(DATA_ROOT);
    if(!strcmp(n,"getFilesDir")) return file_obj(ANDROID_FILES_DIR);
    if(!strcmp(n,"getCacheDir")) return file_obj(ANDROID_CACHE_DIR);
    if(!strcmp(n,"getExternalFilesDir")) return file_obj(ANDROID_EXTERNAL_FILES_DIR);
    if(!strcmp(n,"getAbsolutePath")||!strcmp(n,"getCanonicalPath")||!strcmp(n,"getPath")){FakeObj*o=as_obj(obj);return jni_new_string(o&&o->name?o->name:ANDROID_DATA_DIR);}
    if(!strcmp(n,"getClassLoader")) return make_object("java/lang/ClassLoader");
    if(!strcmp(n,"getAssets")) return make_object("android/content/res/AssetManager");
    if(!strcmp(n,"getResources")) return make_object("android/content/res/Resources");
    if(!strcmp(n,"getDisplayMetrics")) return make_object("android/util/DisplayMetrics");
    if(!strcmp(n,"getSystemService")) {
        const char *service=args?jni_string_utf8(args[0].l):"";
        if(!strcmp(service,"audio")) {
            trace_log_printf("ANDROID","AudioManager getSystemService(audio)");
            return make_object("android/media/AudioManager");
        }
        return make_object("java/lang/Object");
    }
    if(!strcmp(n,"getProperty") && (!strcmp(cls,"android/media/AudioManager") ||
                                     !strcmp(class_name(obj),"android/media/AudioManager"))) {
        const char *key=args?jni_string_utf8(args[0].l):"";
        if(!strcmp(key,"android.media.property.OUTPUT_SAMPLE_RATE")) {
            trace_log_printf("ANDROID","AudioManager getProperty OUTPUT_SAMPLE_RATE -> 48000");
            return jni_new_string("48000");
        }
        if(!strcmp(key,"android.media.property.OUTPUT_FRAMES_PER_BUFFER")) {
            trace_log_printf("ANDROID","AudioManager getProperty OUTPUT_FRAMES_PER_BUFFER -> 1024");
            return jni_new_string("1024");
        }
        return NULL;
    }
    if(!strcmp(n,"getWindowManager")) return make_object("android/view/WindowManager");
    if(!strcmp(n,"getDefaultDisplay")||!strcmp(n,"getDisplay")) return make_object("android/view/Display");
    if(!strcmp(n,"getWindow")) return make_object("android/view/Window");
    if(!strcmp(n,"getDecorView")||!strcmp(n,"getCurrentFocus")) return make_object("android/view/View");
    if(!strcmp(n,"getMainLooper")||!strcmp(n,"myLooper")) return make_object("android/os/Looper");
    if(!strcmp(n,"getDefault")) return make_object("java/util/Locale");
    if(!strcmp(n,"getLanguage")) return jni_new_string("en");
    if(!strcmp(n,"getCountry")) return jni_new_string("US");
    if(!strcmp(n,"getIntent")) return make_object("android/content/Intent");
    if(!strcmp(n,"getData")||!strcmp(n,"getExtras")) return NULL;
    if(!strcmp(n,"toString")){FakeObj*o=as_obj(obj);return jni_new_string(o&&o->name?o->name:(o&&o->class_name?o->class_name:cls));}
    if(!strcmp(n,"getName")) return jni_new_string(cls);
    if(!strcmp(n,"getClass")) return make_class(class_name(obj));
    return make_object("java/lang/Object");
}
static int int_result(void *obj,FakeObj*mid,const JValue*args){
    (void)args;const char*n=mid&&mid->name?mid->name:"";FakeObj*o=as_obj(obj);
    if(o&&o->kind==FK_MOTION){
        if(!strcmp(n,"getAction")||!strcmp(n,"getActionMasked"))return o->motion.action;
        if(!strcmp(n,"getActionIndex"))return 0;
        if(!strcmp(n,"getActionButton"))return 0;
        if(!strcmp(n,"getPointerCount"))return 1;
        if(!strcmp(n,"getPointerId"))return 0;
        if(!strcmp(n,"getToolType"))return 1;
        if(!strcmp(n,"getSource"))return o->motion.source;
        if(!strcmp(n,"getButtonState"))return o->motion.button_state;
        if(!strcmp(n,"getMetaState"))return o->motion.meta_state;
        if(!strcmp(n,"getDeviceId"))return 0;
        if(!strcmp(n,"getDisplayId"))return 0;
        if(!strcmp(n,"getFlags"))return 0;
        if(!strcmp(n,"getEdgeFlags"))return 0;
        if(!strcmp(n,"getClassification"))return 0;
        if(!strcmp(n,"getHistorySize"))return 0;
    }
    if (!strcmp(n,"getWidth")) return (int)android_width();
    if (!strcmp(n,"getHeight")) return (int)android_height();
    if (!strcmp(n,"getRotation")) return 0;
    if (!strcmp(n,"hashCode")) return (int)((uintptr_t)obj>>4);
    if (!strcmp(n,"size")) {
        if (o && o->kind==FK_ARRAY) return (int)o->size;
    }
    if (!strcmp(n,"length")) {
        if (o && o->kind==FK_ARRAY) return (int)o->size;
        if (o && o->kind==FK_STRING) return utf16_length_utf8(o->name);
    }
    return 0;
}
static int64_t long_result(void*obj,FakeObj*mid,const JValue*args){(void)args;FakeObj*o=as_obj(obj);const char*n=mid&&mid->name?mid->name:"";if(o&&o->kind==FK_MOTION){if(!strcmp(n,"getEventTime"))return o->motion.event_time_ms;if(!strcmp(n,"getDownTime"))return o->motion.down_time_ms;}return 0;}
static float motion_axis_value(const FakeObj *o,int axis){
    if(!o)return 0.0f;

    switch(axis){
        case 0: return o->motion.x;
        case 1: return o->motion.y;
        case 2: return o->motion.pressure;
        case 3: return o->motion.size;
        case 4: case 5: case 6: case 7: return o->motion.size;
        default:return 0.0f;
    }
}
static float float_result(void*obj,FakeObj*mid,const JValue*args){FakeObj*o=as_obj(obj);const char*n=mid&&mid->name?mid->name:"";if(o&&o->kind==FK_MOTION){if(!strcmp(n,"getX")||!strcmp(n,"getRawX"))return o->motion.x;if(!strcmp(n,"getY")||!strcmp(n,"getRawY"))return o->motion.y;if(!strcmp(n,"getPressure"))return o->motion.pressure;if(!strcmp(n,"getSize"))return o->motion.size;if(!strcmp(n,"getXPrecision")||!strcmp(n,"getYPrecision"))return 1.0f;if(!strcmp(n,"getAxisValue")){int axis=args?args[0].i:0;return motion_axis_value(o,axis);}}if(!strcmp(n,"getDensity"))return 1.0f;return 0.0f;}

#define MAKE_CALLS(PREFIX,BASEOBJ) \
static int PREFIX##Int(void*e,void*o,void*m,...){(void)e;return int_result(o,as_obj(m),NULL);} \
static int PREFIX##IntV(void*e,void*o,void*m,va_list a){(void)e;(void)a;return int_result(o,as_obj(m),NULL);} \
static int PREFIX##IntA(void*e,void*o,void*m,const JValue*a){(void)e;return int_result(o,as_obj(m),a);} \
static int64_t PREFIX##Long(void*e,void*o,void*m,...){(void)e;return long_result(o,as_obj(m),NULL);} \
static int64_t PREFIX##LongV(void*e,void*o,void*m,va_list a){(void)e;(void)a;return long_result(o,as_obj(m),NULL);} \
static int64_t PREFIX##LongA(void*e,void*o,void*m,const JValue*a){(void)e;return long_result(o,as_obj(m),a);} \
static float PREFIX##Float(void*e,void*o,void*m,...){(void)e;FakeObj*id=as_obj(m);JValue av[2]={{0},{0}};const JValue*ap=NULL;if(id&&id->name&&!strcmp(id->name,"getAxisValue")){va_list va;va_start(va,m);av[0].i=va_arg(va,int);va_end(va);ap=av;}return float_result(o,id,ap);} \
static float PREFIX##FloatV(void*e,void*o,void*m,va_list a){(void)e;FakeObj*id=as_obj(m);JValue av[2]={{0},{0}};const JValue*ap=NULL;if(id&&id->name&&!strcmp(id->name,"getAxisValue")){va_list cp;va_copy(cp,a);av[0].i=va_arg(cp,int);va_end(cp);ap=av;}return float_result(o,id,ap);} \
static float PREFIX##FloatA(void*e,void*o,void*m,const JValue*a){(void)e;return float_result(o,as_obj(m),a);} \
static double PREFIX##Double(void*e,void*o,void*m,...){return (double)PREFIX##Float(e,o,m);} \
static double PREFIX##DoubleV(void*e,void*o,void*m,va_list a){return (double)PREFIX##FloatV(e,o,m,a);} \
static double PREFIX##DoubleA(void*e,void*o,void*m,const JValue*a){return (double)PREFIX##FloatA(e,o,m,a);} \
static __attribute__((unused)) int PREFIX##Bool(void*e,void*o,void*m,...){(void)e;(void)o;FakeObj*id=as_obj(m);const char*n=id&&id->name?id->name:"";if(!strcmp(n,"isFinishing"))return 0;if(!strcmp(n,"isTablet"))return 0;return 0;} \
static void PREFIX##Void(void*e,void*o,void*m,...){(void)e;(void)o;FakeObj*id=as_obj(m);if(id&&id->name&&(!strcmp(id->name,"finish")||!strcmp(id->name,"quit")))jni_request_quit();}
MAKE_CALLS(j_Call,0)

static int object_call_needs_arg0(FakeObj *mid){
    if(!mid||!mid->name)return 0;
    return !strcmp(mid->name,"getSystemService") || !strcmp(mid->name,"getProperty");
}
static void *j_CallObject(void*e,void*o,void*m,...){
    (void)e; FakeObj *id=as_obj(m); JValue av[1]={{0}}; const JValue *ap=NULL;
    if(object_call_needs_arg0(id)){
        va_list va; va_start(va,m); av[0].l=va_arg(va,void*); va_end(va); ap=av;
    }
    return object_result(o,id,ap);
}
static void *j_CallObjectV(void*e,void*o,void*m,va_list a){
    (void)e; FakeObj *id=as_obj(m); JValue av[1]={{0}}; const JValue *ap=NULL;
    if(object_call_needs_arg0(id)){
        va_list cp; va_copy(cp,a); av[0].l=va_arg(cp,void*); va_end(cp); ap=av;
    }
    return object_result(o,id,ap);
}
static void *j_CallObjectA(void*e,void*o,void*m,const JValue*a){
    (void)e; return object_result(o,as_obj(m),a);
}

static void *j_NewObject(void*e,void*c,void*m,...){(void)e;FakeObj*cl=as_obj(c);FakeObj*id=as_obj(m);void*o=make_object(cl&&cl->class_name?cl->class_name:"java/lang/Object");if(cl&&cl->class_name&&!strcmp(cl->class_name,"java/io/File")&&id&&id->name&&!strcmp(id->name,"<init>")){va_list a;va_start(a,m);void*s=va_arg(a,void*);va_end(a);FakeObj*fo=as_obj(o);free(fo->name);fo->name=xstrdup(jni_string_utf8(s));}return o;}
static void *j_NewObjectV(void*e,void*c,void*m,va_list a){(void)e;(void)m;FakeObj*cl=as_obj(c);void*o=make_object(cl&&cl->class_name?cl->class_name:"java/lang/Object");if(cl&&cl->class_name&&!strcmp(cl->class_name,"java/io/File")){void*s=va_arg(a,void*);FakeObj*fo=as_obj(o);fo->name=xstrdup(jni_string_utf8(s));}return o;}
static void *j_NewObjectA(void*e,void*c,void*m,const JValue*a){(void)e;(void)m;FakeObj*cl=as_obj(c);void*o=make_object(cl&&cl->class_name?cl->class_name:"java/lang/Object");if(cl&&cl->class_name&&!strcmp(cl->class_name,"java/io/File")&&a){FakeObj*fo=as_obj(o);fo->name=xstrdup(jni_string_utf8(a[0].l));}return o;}

static int j_is_class_for_name(FakeObj *mid){
    if(!mid || !mid->name || strcmp(mid->name,"forName")) return 0;

    if(mid->sig && strstr(mid->sig,")Ljava/lang/Class;")) return 1;

    return 1;
}
static void *motion_event_obtain_clone(void *src_obj){
    FakeObj *src=as_obj(src_obj);
    if(!src || src->kind!=FK_MOTION) return NULL;

    enum { MOTION_CLONE_SLOTS = 8 };
    static FakeObj *pool[MOTION_CLONE_SLOTS];
    static unsigned next_slot;
    unsigned slot=next_slot++ % MOTION_CLONE_SLOTS;
    FakeObj *dst=pool[slot];
    if(!dst){
        dst=obj_new(FK_MOTION,"android/view/MotionEvent",NULL,NULL);
        pool[slot]=dst;
    }
    if(!dst) return NULL;
    dst->motion=src->motion;

    return dst;
}

static int j_is_motion_obtain(FakeObj *mid){
    if(!mid || !mid->name || strcmp(mid->name,"obtain")) return 0;
    if(mid->class_name && !strcmp(mid->class_name,"android/view/MotionEvent")) return 1;
    return mid->sig && !strcmp(mid->sig,"(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;");
}

static void *j_class_for_name(void *name_obj){
    const char *src=jni_string_utf8(name_obj);
    if(!src||!*src)return make_class("java/lang/Object");
    size_t n=strlen(src);
    char *norm=malloc(n+1);
    if(!norm)return make_class("java/lang/Object");
    for(size_t i=0;i<n;i++)norm[i]=(src[i]=='.')?'/':src[i];
    norm[n]=0;
    void *r=make_class(norm);
    free(norm);
    return r;
}

static void *j_CallStaticObject(void*e,void*c,void*m,...){
    (void)e; FakeObj *mid=as_obj(m);
    if(j_is_class_for_name(mid)){
        va_list a; va_start(a,m); void *name=va_arg(a,void*); va_end(a);
        return j_class_for_name(name);
    }
    if(j_is_motion_obtain(mid)){
        va_list a; va_start(a,m); void *src=va_arg(a,void*); va_end(a);
        return motion_event_obtain_clone(src);
    }
    return object_result(c,mid,NULL);
}
static void *j_CallStaticObjectV(void*e,void*c,void*m,va_list a){
    (void)e; FakeObj *mid=as_obj(m);
    if(j_is_class_for_name(mid)){
        va_list cp; va_copy(cp,a); void *name=va_arg(cp,void*); va_end(cp);
        return j_class_for_name(name);
    }
    if(j_is_motion_obtain(mid)){
        va_list cp; va_copy(cp,a); void *src=va_arg(cp,void*); va_end(cp);
        return motion_event_obtain_clone(src);
    }
    return object_result(c,mid,NULL);
}
static void *j_CallStaticObjectA(void*e,void*c,void*m,const JValue*a){
    (void)e; FakeObj *mid=as_obj(m);
    if(j_is_class_for_name(mid))return j_class_for_name(a?a[0].l:NULL);
    if(j_is_motion_obtain(mid))return motion_event_obtain_clone(a?a[0].l:NULL);
    return object_result(c,mid,a);
}
static int j_CallStaticInt(void*e,void*c,void*m,...){(void)e;return int_result(c,as_obj(m),NULL);} static int j_CallStaticIntV(void*e,void*c,void*m,va_list a){(void)a;return j_CallStaticInt(e,c,m);} static int j_CallStaticIntA(void*e,void*c,void*m,const JValue*a){(void)a;return j_CallStaticInt(e,c,m);}
static int64_t j_CallStaticLong(void*e,void*c,void*m,...){(void)e;return long_result(c,as_obj(m),NULL);} static float j_CallStaticFloat(void*e,void*c,void*m,...){(void)e;return float_result(c,as_obj(m),NULL);} static double j_CallStaticDouble(void*e,void*c,void*m,...){return (double)j_CallStaticFloat(e,c,m);} static __attribute__((unused)) int j_CallStaticBool(void*e,void*c,void*m,...){(void)e;(void)c;(void)m;return 0;}

static int j_is_system_load_library(FakeObj *mid){
    return mid && mid->class_name && mid->name && !strcmp(mid->class_name,"java/lang/System") && !strcmp(mid->name,"loadLibrary");
}
static void j_system_load_library(FakeObj *mid, void *arg){
    (void)mid;
    (void)arg;
}
static void j_CallStaticVoid(void*e,void*c,void*m,...){
    (void)e;(void)c; FakeObj*id=as_obj(m);
    if(j_is_system_load_library(id)){va_list a;va_start(a,m);void *arg=va_arg(a,void*);va_end(a);j_system_load_library(id,arg);}
}
static void j_CallStaticVoidV(void*e,void*c,void*m,va_list a){(void)e;(void)c;FakeObj*id=as_obj(m);if(j_is_system_load_library(id))j_system_load_library(id,va_arg(a,void*));}
static void j_CallStaticVoidA(void*e,void*c,void*m,const JValue*a){(void)e;(void)c;FakeObj*id=as_obj(m);if(j_is_system_load_library(id))j_system_load_library(id,a?a[0].l:NULL);}

static void *j_GetObjectField(void*e,void*o,void*f){
    (void)e;(void)o;
    FakeObj*id=as_obj(f);
    const char*n=id&&id->name?id->name:"";
    if(!strcmp(n,"currentActivity"))return fake_context_obj;
    if(!strcmp(n,"dataDir")) return jni_new_string(ANDROID_DATA_DIR);
    if(!strcmp(n,"sourceDir")||!strcmp(n,"publicSourceDir")||!strcmp(n,"nativeLibraryDir"))
        return jni_new_string(DATA_ROOT);
    if(!strcmp(n,"versionName")){
        const char*v=installer_version_name();
        return v?jni_new_string(v):NULL;
    }
    return make_object(id&&id->sig?id->sig:"java/lang/Object");
}
static int j_GetIntField(void*e,void*o,void*f){
    (void)e;(void)o;
    FakeObj*id=as_obj(f);
    const char*n=id&&id->name?id->name:"";
    if(!strcmp(n,"widthPixels"))return (int)android_width();
    if(!strcmp(n,"heightPixels"))return (int)android_height();
    if(!strcmp(n,"densityDpi"))return 160;
    if(!strcmp(n,"versionCode")){int v=installer_version_code();return v>=0?v:0;}
    if(!strcmp(n,"minSdkVersion")){int v=installer_min_sdk();return v>=0?v:0;}
    if(!strcmp(n,"targetSdkVersion")){int v=installer_target_sdk();return v>=0?v:0;}
    return 0;
}
static float j_GetFloatField(void*e,void*o,void*f){(void)e;(void)o;FakeObj*id=as_obj(f);if(id&&id->name&&!strcmp(id->name,"density"))return 1.0f;return 0.0f;}
static double j_GetDoubleField(void*e,void*o,void*f){return (double)j_GetFloatField(e,o,f);}
static void *j_GetStaticObjectField(void*e,void*c,void*f){(void)e;(void)c;FakeObj*id=as_obj(f);const char*n=id&&id->name?id->name:"";if(!strcmp(n,"currentActivity"))return fake_context_obj;if(!strcmp(n,"MANUFACTURER"))return jni_new_string("Nintendo");if(!strcmp(n,"MODEL")||!strcmp(n,"DEVICE"))return jni_new_string("Nintendo Switch");if(!strcmp(n,"PRODUCT"))return jni_new_string("nx");if(!strcmp(n,"AUDIO_SERVICE"))return jni_new_string("audio");if(!strcmp(n,"PROPERTY_OUTPUT_SAMPLE_RATE"))return jni_new_string("android.media.property.OUTPUT_SAMPLE_RATE");if(!strcmp(n,"PROPERTY_OUTPUT_FRAMES_PER_BUFFER"))return jni_new_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER");return make_object("java/lang/Object");}
static int j_GetStaticIntField(void*e,void*c,void*f){(void)e;(void)c;FakeObj*id=as_obj(f);if(id&&id->name&&!strcmp(id->name,"SDK_INT"))return 35;return 0;}

static void *j_NewString(void*e,const uint16_t*u,int n){(void)e;if(!u||n<=0)return jni_new_string("");char*s=malloc((size_t)n+1);if(!s)return NULL;for(int i=0;i<n;i++)s[i]=(u[i]<128)?(char)u[i]:'?';s[n]=0;void*r=jni_new_string(s);free(s);return r;}
static int j_GetStringLength(void*e,void*s){(void)e;return utf16_length_utf8(jni_string_utf8(s));}
static const uint16_t *j_GetStringChars(void*e,void*s,uint8_t*copy){(void)e;const char*p=jni_string_utf8(s);size_t n=strlen(p);uint16_t*w=calloc(n+1,sizeof(uint16_t));if(!w)return NULL;for(size_t i=0;i<n;i++)w[i]=(uint8_t)p[i];if(copy)*copy=1;return w;}
static void j_ReleaseStringChars(void*e,void*s,const uint16_t*p){(void)e;(void)s;free((void*)p);}
static void *j_NewStringUTF(void*e,const char*s){(void)e;return jni_new_string(s);}
static int j_GetStringUTFLength(void*e,void*s){return j_GetStringLength(e,s);}
static const char *j_GetStringUTFChars(void*e,void*s,uint8_t*copy){(void)e;if(copy)*copy=0;return jni_string_utf8(s);}
static void j_ReleaseStringUTFChars(void*e,void*s,const char*p){(void)e;(void)s;(void)p;}

static FakeObj *array_new(size_t count,size_t elem,const char*cls){FakeObj*o=obj_new(FK_ARRAY,cls,NULL,NULL);if(!o)return NULL;o->size=count;o->data=calloc(count?count:1,elem);return o;}
static int j_GetArrayLength(void*e,void*a){(void)e;FakeObj*o=as_obj(a);return o&&o->kind==FK_ARRAY?(int)o->size:0;}
static void *j_NewObjectArray(void*e,int n,void*c,void*init){(void)e;FakeObj*o=array_new(n,sizeof(void*),"[Ljava/lang/Object;");if(o&&init)for(int i=0;i<n;i++)((void**)o->data)[i]=init;(void)c;return o;}
static void *j_GetObjectArrayElement(void*e,void*a,int i){(void)e;FakeObj*o=as_obj(a);return o&&o->kind==FK_ARRAY&&i>=0&&(size_t)i<o->size?((void**)o->data)[i]:NULL;}
static void j_SetObjectArrayElement(void*e,void*a,int i,void*v){(void)e;FakeObj*o=as_obj(a);if(o&&o->kind==FK_ARRAY&&i>=0&&(size_t)i<o->size)((void**)o->data)[i]=v;}
#define ARR_NEW(NAME,SZ,CLS) static void*j_New##NAME##Array(void*e,int n){(void)e;return array_new(n,SZ,CLS);}
ARR_NEW(Boolean,1,"[Z") ARR_NEW(Byte,1,"[B") ARR_NEW(Char,2,"[C") ARR_NEW(Short,2,"[S") ARR_NEW(Int,4,"[I") ARR_NEW(Long,8,"[J") ARR_NEW(Float,4,"[F") ARR_NEW(Double,8,"[D")
static void *j_GetArrayElements(void*e,void*a,uint8_t*copy){(void)e;FakeObj*o=as_obj(a);if(copy)*copy=0;return o&&o->kind==FK_ARRAY?o->data:NULL;}
static void j_ReleaseArrayElements(void*e,void*a,void*p,int mode){(void)e;(void)a;(void)p;(void)mode;}
static void j_GetByteArrayRegion(void*e,void*a,int start,int len,void*out){(void)e;FakeObj*o=as_obj(a);if(o&&o->data&&start>=0&&len>=0&&(size_t)(start+len)<=o->size)memcpy(out,(uint8_t*)o->data+start,len);}
static void j_SetByteArrayRegion(void*e,void*a,int start,int len,const void*in){(void)e;FakeObj*o=as_obj(a);if(o&&o->data&&start>=0&&len>=0&&(size_t)(start+len)<=o->size)memcpy((uint8_t*)o->data+start,in,len);}

static int j_RegisterNatives(void*e,void*c,const void*m,int n){(void)e;(void)c;(void)m;(void)n;return 0;}
static int j_GetJavaVM(void*e,void**vm){(void)e;if(vm)*vm=fake_vm;return JNI_OK;}
static int j_ExceptionCheck(void*e){(void)e;return 0;} static void*j_ExceptionOccurred(void*e){(void)e;return NULL;} static void j_ExceptionClear(void*e){(void)e;}
static void *j_NewDirectByteBuffer(void*e,void*p,int64_t n){(void)e;return jni_new_direct_buffer(p,n>0?(size_t)n:0);}
static void *j_GetDirectBufferAddress(void*e,void*b){(void)e;FakeObj*o=as_obj(b);return o&&o->kind==FK_DIRECT?o->data:NULL;}
static int64_t j_GetDirectBufferCapacity(void*e,void*b){(void)e;FakeObj*o=as_obj(b);return o&&o->kind==FK_DIRECT?(int64_t)o->size:-1;}
static void*j_GetPrimitiveArrayCritical(void*e,void*a,uint8_t*c){return j_GetArrayElements(e,a,c);} static void j_ReleasePrimitiveArrayCritical(void*e,void*a,void*p,int m){j_ReleaseArrayElements(e,a,p,m);}

static int vm_Destroy(void*vm){(void)vm;return JNI_OK;} static int vm_Attach(void*vm,void**penv,void*args){(void)vm;(void)args;if(penv)*penv=fake_env;return JNI_OK;} static int vm_Detach(void*vm){(void)vm;return JNI_OK;} static int vm_GetEnv(void*vm,void**penv,int version){(void)vm;(void)version;if(penv)*penv=fake_env;return JNI_OK;}

static void set_slot(int i,void*p){if(i>=0&&i<(int)(sizeof(g_env_table)/sizeof(g_env_table[0])))g_env_table[i]=p;}
void jni_init(void){
    if (g_ready) return;
    mutexInit(&g_obj_lock);
    g_ready=1;
    jni_quit_requested=0;
    for(size_t i=0;i<sizeof(g_env_table)/sizeof(g_env_table[0]);i++)g_env_table[i]=(void*)j_stub_u;
    set_slot(4,j_GetVersion);set_slot(6,j_FindClass);set_slot(15,j_ExceptionOccurred);set_slot(17,j_ExceptionClear);
    set_slot(21,j_NewGlobalRef);set_slot(22,j_DeleteRef);set_slot(23,j_DeleteRef);set_slot(24,j_IsSameObject);set_slot(25,j_NewLocalRef);
    set_slot(28,j_NewObject);set_slot(29,j_NewObjectV);set_slot(30,j_NewObjectA);set_slot(31,j_GetObjectClass);set_slot(32,j_IsInstanceOf);set_slot(33,j_GetMethodID);
    set_slot(34,j_CallObject);set_slot(35,j_CallObjectV);set_slot(36,j_CallObjectA);
    for (int i=37;i<=48;i++) set_slot(i,j_stub_u);
    set_slot(49,j_CallInt);set_slot(50,j_CallIntV);set_slot(51,j_CallIntA);set_slot(52,j_CallLong);set_slot(53,j_CallLongV);set_slot(54,j_CallLongA);set_slot(55,j_CallFloat);set_slot(56,j_CallFloatV);set_slot(57,j_CallFloatA);set_slot(58,j_CallDouble);set_slot(59,j_CallDoubleV);set_slot(60,j_CallDoubleA);set_slot(61,j_CallVoid);set_slot(62,j_stub_v);set_slot(63,j_stub_v);
    for(int i=64;i<=93;i++)set_slot(i,j_stub_u);
    set_slot(94,j_GetFieldID);set_slot(95,j_GetObjectField);set_slot(100,j_GetIntField);set_slot(102,j_GetFloatField);set_slot(103,j_GetDoubleField);for(int i=104;i<=112;i++)set_slot(i,j_stub_v);
    set_slot(113,j_GetStaticMethodID);set_slot(114,j_CallStaticObject);set_slot(115,j_CallStaticObjectV);set_slot(116,j_CallStaticObjectA);for(int i=117;i<=128;i++)set_slot(i,j_stub_u);set_slot(129,j_CallStaticInt);set_slot(130,j_CallStaticIntV);set_slot(131,j_CallStaticIntA);set_slot(132,j_CallStaticLong);set_slot(133,j_stub_u);set_slot(134,j_stub_u);set_slot(135,j_CallStaticFloat);set_slot(136,j_stub_f);set_slot(137,j_stub_f);set_slot(138,j_CallStaticDouble);set_slot(139,j_stub_d);set_slot(140,j_stub_d);set_slot(141,j_CallStaticVoid);set_slot(142,j_CallStaticVoidV);set_slot(143,j_CallStaticVoidA);
    set_slot(144,j_GetStaticFieldID);set_slot(145,j_GetStaticObjectField);set_slot(150,j_GetStaticIntField);for(int i=154;i<=162;i++)set_slot(i,j_stub_v);
    set_slot(163,j_NewString);set_slot(164,j_GetStringLength);set_slot(165,j_GetStringChars);set_slot(166,j_ReleaseStringChars);set_slot(167,j_NewStringUTF);set_slot(168,j_GetStringUTFLength);set_slot(169,j_GetStringUTFChars);set_slot(170,j_ReleaseStringUTFChars);
    set_slot(171,j_GetArrayLength);set_slot(172,j_NewObjectArray);set_slot(173,j_GetObjectArrayElement);set_slot(174,j_SetObjectArrayElement);set_slot(175,j_NewBooleanArray);set_slot(176,j_NewByteArray);set_slot(177,j_NewCharArray);set_slot(178,j_NewShortArray);set_slot(179,j_NewIntArray);set_slot(180,j_NewLongArray);set_slot(181,j_NewFloatArray);set_slot(182,j_NewDoubleArray);
    for (int i=183;i<=190;i++) set_slot(i,j_GetArrayElements);
    for (int i=191;i<=198;i++) set_slot(i,j_ReleaseArrayElements);
    set_slot(200,j_GetByteArrayRegion);set_slot(208,j_SetByteArrayRegion);
    set_slot(215,j_RegisterNatives);set_slot(216,j_stub_u);set_slot(217,j_stub_u);set_slot(218,j_stub_u);set_slot(219,j_GetJavaVM);set_slot(222,j_GetPrimitiveArrayCritical);set_slot(223,j_ReleasePrimitiveArrayCritical);set_slot(224,j_GetStringChars);set_slot(225,j_ReleaseStringChars);set_slot(228,j_ExceptionCheck);set_slot(229,j_NewDirectByteBuffer);set_slot(230,j_GetDirectBufferAddress);set_slot(231,j_GetDirectBufferCapacity);
    g_vm_table[0]=g_vm_table[1]=g_vm_table[2]=NULL;g_vm_table[3]=(void*)vm_Destroy;g_vm_table[4]=(void*)vm_Attach;g_vm_table[5]=(void*)vm_Detach;g_vm_table[6]=(void*)vm_GetEnv;g_vm_table[7]=(void*)vm_Attach;
    fake_context_obj=make_object("android/app/Activity");fake_unityplayer_thiz=make_object("com/unity3d/player/UnityPlayer");fake_surface_obj=make_object("android/view/Surface");
}
