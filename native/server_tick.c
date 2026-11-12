#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "server_tick.h"

/* The game sizes its per-frame work for a client that must keep drawing: at most 48 cleaning strokes and
   MoonSettings.cleaningWorkBudgetMs of cleaning per frame, 3 far robots per frame, 1 ms of dust serving per frame.
   A server has no frames to protect, but it runs the same budgets at a fixed 30 ticks/s, so once mods add robots,
   machines or vehicles the queues grow faster than they drain and the world falls behind while the CPU idles.
   This module gives the server its own tick: it raises the tick rate while there is a backlog (every per-frame
   budget then runs more often), drains the cleaning queue past the 48-stroke cap within a time budget, and drops
   GPU-only engine calls that only produce errors (each one costs a stack trace) on a server with no GPU. */
typedef void Object;
typedef const void Method;
static struct {
 void *(__cdecl *domain_get)(void);
 const void **(__cdecl *domain_get_assemblies)(void *,size_t *);
 const void *(__cdecl *assembly_get_image)(const void *);
 const char *(__cdecl *image_get_name)(const void *);
 void *(__cdecl *class_from_name)(const void *,const char *,const char *);
 const Method *(__cdecl *class_get_method_from_name)(void *,const char *,int);
 void *(__cdecl *class_get_field_from_name)(void *,const char *);
 size_t (__cdecl *field_get_offset)(void *);
 void *(__cdecl *object_get_class)(Object *);
 Object *(__cdecl *runtime_invoke)(const Method *,Object *,void **,Object **);
} il;
enum {STROKES_A_FRAME=48,STROKE_QUEUE=4096,STROKE_QUEUE_NEARLY_FULL=3500,MAX_DRAIN_PASSES=64};
static void (*logger)(const char *);
static bool installed,catchup=true,gpu_skipped;
static int base_rate=30,max_rate=120,rate_now;
static LARGE_INTEGER frequency;
static size_t stroke_count_offset,held_offset,list_size_offset;static bool list_size_known;
static const Method *set_frame_rate,*set_vsync;
static void (__cdecl *real_world_update)(Object *,const Method *);
static void (__cdecl *real_process_strokes)(Object *,bool,float,const Method *);
static int64_t last_frame;
static struct {unsigned frames,updates;double frame_ms,frame_max,world_ms,world_max;} window;
static struct {double tps,frame_avg,frame_max,world_avg,world_max;} shown;
static int queued_now,held_now;static uint64_t drained;
static ULONGLONG window_start,calm_since,rate_changed,slow_since,next_warning,world_since;static bool behind,world_seen;

static void say(const char *text){if(logger)logger(text);}
static double elapsed(int64_t from,int64_t to){return (double)(to-from)*1000.0/(double)frequency.QuadPart;}
static int64_t counter(void){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
static int env_int(const char *name,int fallback,int low,int high){
 char value[16];DWORD n=GetEnvironmentVariableA(name,value,sizeof(value));if(!n||n>=sizeof(value))return fallback;
 char *end=NULL;long parsed=strtol(value,&end,10);if(end==value||*end){char text[160];snprintf(text,sizeof(text),"%s must be a whole number; using %d",name,fallback);say(text);return fallback;}
 return parsed<low?low:parsed>high?high:(int)parsed;
}
static void *code_of(const Method *m){
 if(!m)return NULL;
 void *p=*(void *const *)m;MEMORY_BASIC_INFORMATION info;
 if(!p||!VirtualQuery(p,&info,sizeof(info))||info.State!=MEM_COMMIT)return NULL;
 DWORD flags=info.Protect&0xff;
 return !(info.Protect&PAGE_GUARD)&&(flags==PAGE_EXECUTE||flags==PAGE_EXECUTE_READ||flags==PAGE_EXECUTE_READWRITE||flags==PAGE_EXECUTE_WRITECOPY)?p:NULL;
}
static bool hook(void *k,const char *name,int argc,void *replacement,void **original){
 void *p=code_of(k?il.class_get_method_from_name(k,name,argc):NULL);
 if(!p){char text[160];snprintf(text,sizeof(text),"Server tick: %s (%d arguments) is unavailable",name,argc);say(text);return false;}
 MH_STATUS result=MH_CreateHook(p,replacement,original);if(result!=MH_OK){say(MH_StatusToString(result));return false;}
 return MH_EnableHook(p)==MH_OK;
}
static const void *image_named(const char *name){
 size_t count=0;const void **assemblies=il.domain_get_assemblies(il.domain_get(),&count);
 for(size_t i=0;i<count;i++){const void *image=il.assembly_get_image(assemblies[i]);if(strcmp(il.image_get_name(image),name)==0)return image;}
 return NULL;
}
static int queued(Object *world){return *(const int32_t *)((const uint8_t *)world+stroke_count_offset);}
static void sample(Object *world){
 queued_now=queued(world);
 Object *held=*(Object *const *)((const uint8_t *)world+held_offset);if(!held){held_now=0;return;}
 if(!list_size_known){void *size=il.class_get_field_from_name(il.object_get_class(held),"_size");if(size)list_size_offset=il.field_get_offset(size);list_size_known=true;}
 if(list_size_offset)held_now=*(const int32_t *)((const uint8_t *)held+list_size_offset);
}
static void __cdecl world_update(Object *self,const Method *info){
 int64_t start=counter();real_world_update(self,info);double ms=elapsed(start,counter());
 window.updates++;window.world_ms+=ms;if(ms>window.world_max)window.world_max=ms;
 sample(self);world_seen=true;
}
/* complete=true is the game's own save-time flush, which already drains everything. Otherwise keep calling the
   game's processor (each call is one ordinary 48-stroke, cleaningWorkBudgetMs pass) until the queue is empty, this
   tick's drain budget is spent, or a pass makes no progress (robot strokes wait for their CPU share, which every
   pass tops up, and strokes over unloaded ground move to the held list). */
static void __cdecl process_strokes(Object *self,bool complete,float robot_ms,const Method *info){
 int64_t start=counter();real_process_strokes(self,complete,robot_ms,info);
 if(complete||!catchup)return;
 double budget=0.4*1000.0/(double)(rate_now>0?rate_now:base_rate);
 for(unsigned pass=0;pass<MAX_DRAIN_PASSES;pass++){
  int before=queued(self);if(before<=0||elapsed(start,counter())>=budget)break;
  real_process_strokes(self,false,robot_ms,info);
  int after=queued(self);if(after>=before)break;
  drained+=(uint64_t)(before-after);
 }
}
/* Graphics.CopyTexture and Material.SetPass have nothing to do without a GPU; with the null device they log an error
   with a stack trace every time, and the game calls CopyTexture for every cleaned patch of ground. */
static void __cdecl no_gpu_copy(void){}
static bool __cdecl no_gpu_pass(Object *self,int pass,const Method *info){(void)self;(void)pass;(void)info;return false;}

bool MoonTick_Install(HMODULE module,const void *game_image,void (*log_line)(const char *)){
 logger=log_line;if(installed)return true;
#define API(name) do{FARPROC address=GetProcAddress(module,"il2cpp_" #name);memcpy(&il.name,&address,sizeof(address));if(!il.name){say("Server tick: missing il2cpp_" #name);return false;}}while(0)
 API(domain_get);API(domain_get_assemblies);API(assembly_get_image);API(image_get_name);API(class_from_name);
 API(class_get_method_from_name);API(class_get_field_from_name);API(field_get_offset);API(object_get_class);API(runtime_invoke);
#undef API
 QueryPerformanceFrequency(&frequency);
 base_rate=env_int("MOON_TICK_RATE",30,10,240);max_rate=env_int("MOON_TICK_RATE_MAX",120,base_rate,240);
 catchup=env_int("MOON_CATCHUP",1,0,1)==1;rate_now=base_rate;
 const void *core=image_named("UnityEngine.CoreModule.dll");if(!core){say("Server tick: UnityEngine.CoreModule is missing");return false;}
 void *world=il.class_from_name(game_image,"MoonCleaners","MoonWorld");
 void *count=world?il.class_get_field_from_name(world,"strokeCount"):NULL,*held=world?il.class_get_field_from_name(world,"heldStrokes"):NULL;
 if(!count||!held){say("Server tick: MoonWorld's stroke queue was not found");return false;}
 stroke_count_offset=il.field_get_offset(count);held_offset=il.field_get_offset(held);
 void *app=il.class_from_name(core,"UnityEngine","Application"),*quality=il.class_from_name(core,"UnityEngine","QualitySettings");
 set_frame_rate=app?il.class_get_method_from_name(app,"set_targetFrameRate",1):NULL;
 set_vsync=quality?il.class_get_method_from_name(quality,"set_vSyncCount",1):NULL;
 if(!set_frame_rate){say("Server tick: Application.targetFrameRate is unavailable");return false;}
 if(!hook(world,"Update",0,world_update,(void **)&real_world_update))return false;
 if(catchup&&!hook(world,"ProcessToolStrokes",2,process_strokes,(void **)&real_process_strokes)){catchup=false;say("Server tick: cleaning catch-up is off");}
 void *graphics=il.class_from_name(core,"UnityEngine","Graphics"),*material=il.class_from_name(core,"UnityEngine","Material");
 gpu_skipped=hook(graphics,"CopyTexture",2,no_gpu_copy,NULL)&&hook(graphics,"CopyTexture",6,no_gpu_copy,NULL)&&
  hook(graphics,"CopyTexture",12,no_gpu_copy,NULL)&&hook(material,"SetPass",1,no_gpu_pass,NULL);
 installed=true;
 char text[200];snprintf(text,sizeof(text),"Server tick: %d ticks/s, up to %d while catching up (catch-up %s)%s",base_rate,max_rate,catchup?"on":"off",gpu_skipped?"; GPU-only calls skipped":"");say(text);
 return true;
}
void MoonTick_Frame(void){
 if(!installed)return;
 int64_t now=counter();
 if(last_frame){double ms=elapsed(last_frame,now);window.frames++;window.frame_ms+=ms;if(ms>window.frame_max)window.frame_max=ms;}
 last_frame=now;
}
static void apply_rate(int rate){
 Object *exception=NULL;void *args[]={&rate};il.runtime_invoke(set_frame_rate,NULL,args,&exception);
 if(set_vsync){int zero=0;args[0]=&zero;exception=NULL;il.runtime_invoke(set_vsync,NULL,args,&exception);}
}
static void describe(char *out,size_t size){
 snprintf(out,size,"%.0f ticks/s (target %d), world update %.1f ms avg / %.1f ms max per tick, %d cleaning strokes queued",
  shown.tps,rate_now,shown.world_avg,shown.world_max,queued_now);
}
static void judge(ULONGLONG now){
 bool full=queued_now>=STROKE_QUEUE_NEARLY_FULL;
 bool slow=full||(now-rate_changed>=3000&&shown.tps<0.75*rate_now);
 char text[400],detail[240];
 if(!slow){
  slow_since=0;
  if(behind){behind=false;describe(detail,sizeof(detail));snprintf(text,sizeof(text),"Server caught up: %s",detail);say(text);}
  return;
 }
 if(!slow_since)slow_since=now;
 if(now-slow_since<10000||now<next_warning)return;
 behind=true;next_warning=now+60000;describe(detail,sizeof(detail));
 snprintf(text,sizeof(text),"Server is falling behind: %s%s. Mods that add robots, machines or vehicles add work to every tick; type perf for details.",
  detail,full?" (the game drops new strokes at 4096)":"");say(text);
}
bool MoonTick_Govern(ULONGLONG now){
 if(!installed)return false;
 if(!window_start)window_start=now;
 if(now-window_start>=1000){
  double seconds=(double)(now-window_start)/1000.0;
  shown.tps=window.frames/seconds;shown.frame_avg=window.frames?window.frame_ms/window.frames:0;shown.frame_max=window.frame_max;
  shown.world_avg=window.updates?window.world_ms/window.updates:0;shown.world_max=window.world_max;
  memset(&window,0,sizeof(window));window_start=now;
  /* Loading the world stalls frames for seconds; that is not falling behind. */
  if(world_seen&&!world_since)world_since=now;
  if(world_since&&now-world_since>=30000)judge(now);
 }
 int rate=rate_now;
 if(catchup&&queued_now>STROKES_A_FRAME){rate=max_rate;calm_since=0;}
 else if(!calm_since)calm_since=now;
 else if(now-calm_since>=5000)rate=base_rate;
 if(rate!=rate_now){rate_now=rate;rate_changed=now;}
 /* The game's own frame pacing may set the frame rate again, so it is reapplied on every host tick. */
 apply_rate(rate_now);
 return true;
}
void MoonTick_WriteJson(FILE *f){
 if(!installed)return;
 fprintf(f,",\"tick\":{\"target_rate\":%d,\"base_rate\":%d,\"max_rate\":%d,\"catchup\":%s,\"ticks_per_second\":%.1f,\"frame_ms_avg\":%.2f,\"frame_ms_max\":%.2f,"
  "\"world_ms_avg\":%.2f,\"world_ms_max\":%.2f,\"strokes_queued\":%d,\"strokes_held\":%d,\"strokes_caught_up\":%llu,\"behind\":%s}",
  rate_now,base_rate,max_rate,catchup?"true":"false",shown.tps,shown.frame_avg,shown.frame_max,shown.world_avg,shown.world_max,
  queued_now,held_now,(unsigned long long)drained,behind?"true":"false");
}
bool MoonTick_Command(const char *verb){
 if(strcmp(verb,"perf")!=0)return false;
 if(!installed){say("Performance figures need the dedicated headless server");return true;}
 char text[400];
 snprintf(text,sizeof(text),"Server performance: %.0f ticks/s (target %d, range %d-%d, catch-up %s%s); tick %.1f ms avg / %.1f ms max; world update %.1f ms avg / %.1f ms max",
  shown.tps,rate_now,base_rate,max_rate,catchup?"on":"off",behind?", falling behind":"",shown.frame_avg,shown.frame_max,shown.world_avg,shown.world_max);say(text);
 snprintf(text,sizeof(text),"Cleaning: %d strokes queued (the game holds at most %d), %d waiting for unloaded ground, %llu cleaned by catch-up since start",
  queued_now,STROKE_QUEUE,held_now,(unsigned long long)drained);say(text);
 return true;
}
