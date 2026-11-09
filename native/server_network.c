#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "MinHook.h"
#include "server_network.h"
#include "client_password.h"

/* Public Steamworks SDK 1.57 ABI, matching this game's redistributable.
   These are transport/API structures, not guessed IL2CPP object layouts. */
typedef struct {int32_t kind,bytes;union {uint64_t steam_id;uint8_t raw[128];} data;} NetIdentity;
typedef struct {
 void *data;int32_t size;uint32_t connection;NetIdentity peer;
 int64_t user,time,number;void *free_data,*release;int32_t channel,flags;
 int64_t message_user;uint16_t lane,pad;
} NetMessage;
typedef struct {int32_t user,id;void *data;int32_t size;} CallbackMessage;
typedef struct {int32_t availability;char debug[256];} AuthenticationStatus;
typedef struct {int32_t availability,measuring,config,relays;char debug[256];} RelayStatus;
typedef struct {
 NetIdentity remote;int64_t user;uint32_t listener;uint8_t address[18];uint16_t pad;
 uint32_t remote_pop,relay_pop;int32_t state,reason;char debug[128],description[128];
 int32_t flags;uint32_t reserved[63];
} ConnectionInfo;
_Static_assert(sizeof(NetIdentity)==136,"Steam identity ABI");
_Static_assert(sizeof(NetMessage)==216&&offsetof(NetMessage,peer)==16,"Steam message ABI");
_Static_assert(sizeof(CallbackMessage)==24,"Steam callback ABI");
_Static_assert(sizeof(ConnectionInfo)==696&&offsetof(ConnectionInfo,reason)==180&&offsetof(ConnectionInfo,debug)==184,"Steam connection ABI");
_Static_assert(sizeof(AuthenticationStatus)==260&&sizeof(RelayStatus)==272,"Steam status ABI");

enum {MODE_OFF,MODE_SERVER,MODE_CLIENT};
enum {CONTROL_CHANNEL=3,CONTROL_VERSION=3,LEGACY_VERSION=2,MAX_PEERS=15,MAX_TICKET=2048,MAX_PASSWORD=128,LOOK_MAX=96,KNOWN_NAMES=64,CONTROL_MAGIC=0x5344534d};
enum {AUTH_REQUEST=1,ROSTER=2,REJECT=3,PING=4,GOODBYE=5,WAIT_WORLD=6,PASSWORD_REJECTED=7,LOOK=8};
#pragma pack(push,1)
typedef struct {uint32_t magic;uint16_t version,type;uint32_t bytes;} ControlHeader;
typedef struct {uint64_t id;char name[64];char look[LOOK_MAX];} RosterEntry;
typedef struct {uint64_t server;uint32_t count;RosterEntry entries[MAX_PEERS+1];} RosterPayload;
typedef struct {uint32_t bytes;uint16_t password_length;char name[64];uint8_t password[MAX_PASSWORD],ticket[MAX_TICKET];} AuthPayload;
#pragma pack(pop)

typedef const void Method;
typedef void Object;
static int mode,game_port=27025,query_port=27026;
static bool ports_valid=true;
static bool configured,installed,server_started,backend_ready,world_ready,client_connected,client_join_started;
static bool ticket_ready,ticket_failed;
static uint64_t server_id,self_id;
static uint32_t client_ticket;
static uint8_t ticket_bytes[MAX_TICKET];static uint32_t ticket_size;
static ULONGLONG next_auth,next_ping,next_roster,ticket_requested_at;
static ULONGLONG client_last_heard,client_auth_started,next_server_start,next_backend_ready;
static const char *net_phase="off";
static void (*logger)(const char *);
static HMODULE game_module,steam_module;
static const void *game_image,*steam_image;
static void *server_api,*messages_api,*user_api;
static int32_t server_pipe;
static char server_name[64]="Moon Sweepers Dedicated";
static char server_password[MAX_PASSWORD+1],client_password[MAX_PASSWORD+1];
static size_t server_password_length,client_password_length;
static bool server_password_valid,client_password_ready;
static uint64_t allowlist[MAX_PEERS];static unsigned allow_count;
static bool allowlist_set,allowlist_valid=true,client_stopped,shutdown_requested;
static uint64_t published_roster_hash;
static int control_results[8]={-999,-999,-999,-999,-999,-999,-999,-999},last_auth_status=-999,last_relay_status=-999,last_relay_config=-999,last_relay_reach=-999;
static unsigned request_logs,failure_logs,validation_logs;static ULONGLONG next_diagnostic;
static struct {int32_t id,size;} callback_shapes[32];static unsigned callback_shape_count;
static void *sockets_api,*utils_api;
typedef struct {uint64_t id;bool authenticated;char name[64];char look[LOOK_MAX];ULONGLONG heard;uint16_t version;
 ULONGLONG asked,first_sent,wait_logged;uint64_t sent,next_report;bool entered;} Peer;
static Peer peers[MAX_PEERS];
/* The game resolves crewmate names through the client Steam API, which the
   dedicated server does not have, so names come from the auth payload and are
   remembered after a peer leaves (vehicle owners are looked up by name later). */
static struct {uint64_t id;char name[64];} known_names[KNOWN_NAMES];static unsigned known_next;
enum {MAX_BANS=256};
static struct {uint64_t id;char name[64];} bans[MAX_BANS];static unsigned ban_count;static char ban_path[1024];
static bool look_dirty;static ULONGLONG next_look;static char last_look[LOOK_MAX];

static struct {
 const void **(__cdecl *assemblies)(void *,size_t *);void *(__cdecl *domain)(void);
 const void *(__cdecl *image)(const void *);const char *(__cdecl *image_name)(const void *);
 void *(__cdecl *klass)(const void *,const char *,const char *);
 const Method *(__cdecl *method)(void *,const char *,int);
 void *(__cdecl *field)(void *,const char *);
 void (__cdecl *field_get)(Object *,void *,void *);void (__cdecl *field_set)(Object *,void *,void *);
 void (__cdecl *static_get)(void *,void *);void (__cdecl *static_set)(void *,void *);
 Object *(__cdecl *invoke)(const Method *,Object *,void **,Object **);
 Object *(__cdecl *new_object)(void *);Object *(__cdecl *new_string)(const char *);
 void *(__cdecl *object_class)(Object *);void *(__cdecl *unbox)(Object *);
 void (__cdecl *class_init)(void *);uint32_t (__cdecl *array_header)(void);
 uintptr_t (__cdecl *array_length)(Object *);int (__cdecl *string_length)(Object *);
 const wchar_t *(__cdecl *string_chars)(Object *);void (__cdecl *exception)(Object *,char *,int);
} il;
static struct {
 bool (__cdecl *gs_init)(uint32_t,uint16_t,uint16_t,uint16_t,int,const char *);
 void *(__cdecl *gs)(void);void *(__cdecl *gs_messages)(void);void *(__cdecl *client_messages)(void);
 void *(__cdecl *gs_sockets)(void);void *(__cdecl *client_sockets)(void);void *(__cdecl *utils)(void);void *(__cdecl *user)(void);
 void (__cdecl *set_product)(void *,const char *);void (__cdecl *set_description)(void *,const char *);
 void (__cdecl *set_mod)(void *,const char *);void (__cdecl *set_name)(void *,const char *);
 void (__cdecl *set_dedicated)(void *,bool);void (__cdecl *set_advertise)(void *,bool);
 void (__cdecl *set_capacity)(void *,int);void (__cdecl *logon)(void *);
 bool (__cdecl *logged_on)(void *);uint64_t (__cdecl *gs_id)(void *);
 int (__cdecl *begin_auth)(void *,const void *,int,uint64_t);void (__cdecl *end_auth)(void *,uint64_t);
 int (__cdecl *has_license)(void *,uint64_t,uint32_t);
 int (__cdecl *init_auth)(void *);void (__cdecl *shutdown)(void);
 int (__cdecl *auth_status)(void *,AuthenticationStatus *);
 void (__cdecl *init_relay)(void *);int (__cdecl *relay_status)(void *,RelayStatus *);
 void (__cdecl *dispatch_init)(void);int32_t (__cdecl *gs_pipe)(void);
 void (__cdecl *run_frame)(int32_t);bool (__cdecl *next_callback)(int32_t,CallbackMessage *);
 void (__cdecl *free_callback)(int32_t);
 int (__cdecl *send)(void *,const NetIdentity *,const void *,uint32_t,int,int);
 int (__cdecl *receive)(void *,int,NetMessage **,int);
 bool (__cdecl *accept)(void *,const NetIdentity *);bool (__cdecl *close)(void *,const NetIdentity *);
 bool (__cdecl *close_channel)(void *,const NetIdentity *,int);
 int (__cdecl *session_info)(void *,const NetIdentity *,void *,void *);
 void (__cdecl *release)(NetMessage *);
 uint64_t (__cdecl *user_id)(void *);
 uint32_t (__cdecl *auth_ticket)(void *,void *,int,uint32_t *,const NetIdentity *);
 void (__cdecl *cancel_ticket)(void *,uint32_t);
 bool (__cdecl *set_config)(void *,int,int,intptr_t,int,const void *);
 int (__cdecl *get_config)(void *,int,int,intptr_t,int *,void *,size_t *);
} sdk;
/* These are IL2CPP-generated P/Invoke marshal stubs, resolved from this game's
   Steamworks.NativeMethods. Managed identities contain arrays, and managed out
   structs contain strings/arrays: they cannot be passed to the flat C API. */
static struct {
 const Method *send_method,*receive_method,*accept_method,*close_method,*close_channel_method,*info_method;
 int (__cdecl *send)(void *,const void *,const void *,uint32_t,int,int,const Method *);
 int (__cdecl *receive)(void *,int,Object *,int,const Method *);
 bool (__cdecl *accept)(void *,const void *,const Method *);
 bool (__cdecl *close)(void *,const void *,const Method *);
 bool (__cdecl *close_channel)(void *,const void *,int,const Method *);
 int (__cdecl *info)(void *,const void *,void *,void *,const Method *);
} marshal;
static bool (__cdecl *real_next_callback)(int32_t,CallbackMessage *);
static void (__cdecl *real_steam_destroy)(Object *,const Method *);
static bool (__cdecl *real_send_bytes)(Object *,uint64_t,Object *,int,int,bool,const Method *);
static void (__cdecl *real_receive_transfer)(Object *,uint64_t,Object *,const Method *);
static void (__cdecl *real_receive_world)(Object *,uint64_t,Object *,const Method *);
static unsigned packet_logs[2][3];
static struct {
 void *lobby_class,*member_class,*crew_class,*join_class;
 void *list_field,*changed_field,*member_id,*member_name,*member_look,*member_ready,*member_host,*member_local;
 const Method *steam_initialized,*set_initialized,*set_steam_id,*set_persona,*crew_create,*crew_instance;
 const Method *session_start,*in_menu,*newhire_active,*begin_join,*join_ctor,*persona,*look_local,*look_encode;
 Object *member_list;const Method *list_clear,*list_add;
} game;

static bool net_debug;
static void report(const char *text){if(logger)logger(text);}
/* Transport-level detail; the console shows only player-facing events unless MOON_NET_DEBUG=1. */
static void debug(const char *text){if(net_debug)report(text);}
/* Global Steam networking tuning (ESteamNetworkingConfigValue). The game raises
   these itself on clients; a dedicated server never ran that code, so world
   downloads and bursts were limited to Steam's small default send buffer/rate. */
static void tune_network(void *utils,bool server){
 if(!utils||!sdk.set_config)return;
 enum {GLOBAL_SCOPE=1,INT32=1,SEND_BUFFER=9,SEND_RATE_MIN=10,SEND_RATE_MAX=11,NAGLE_TIME=12,TIMEOUT_INITIAL=24};
 int32_t timeout=20000,buffer=0x400000,rate_min=0x100000,rate_max=0x800000,nagle=0;
 /* Relay negotiation over a slow path can exceed the 10 s default and fail with "Timed out attempting to connect". */
 sdk.set_config(utils,TIMEOUT_INITIAL,GLOBAL_SCOPE,0,INT32,&timeout);
 if(!server)return;
 sdk.set_config(utils,SEND_BUFFER,GLOBAL_SCOPE,0,INT32,&buffer);sdk.set_config(utils,SEND_RATE_MIN,GLOBAL_SCOPE,0,INT32,&rate_min);
 sdk.set_config(utils,SEND_RATE_MAX,GLOBAL_SCOPE,0,INT32,&rate_max);sdk.set_config(utils,NAGLE_TIME,GLOBAL_SCOPE,0,INT32,&nagle);
 /* Read the limits back: world downloads crawl if Steam silently kept its small defaults. */
 if(sdk.get_config){
  int32_t now_buffer=-1,now_min=-1,now_max=-1;int type=0;size_t size=sizeof(int32_t);
  sdk.get_config(utils,SEND_BUFFER,GLOBAL_SCOPE,0,&type,&now_buffer,&size);size=sizeof(int32_t);
  sdk.get_config(utils,SEND_RATE_MIN,GLOBAL_SCOPE,0,&type,&now_min,&size);size=sizeof(int32_t);
  sdk.get_config(utils,SEND_RATE_MAX,GLOBAL_SCOPE,0,&type,&now_max,&size);
  char text[160];snprintf(text,sizeof(text),"Steam send limits: buffer %d KB, rate %d-%d KB/s%s",now_buffer/1024,now_min/1024,now_max/1024,now_max==rate_max?"":" (tuning was not applied)");report(text);
 }else report("Steam send limits could not be verified");
}
static void phase(const char *text){if(strcmp(net_phase,text)!=0){net_phase=text;report(text);}}
static void env(const char *key,char *out,size_t size){DWORD n=GetEnvironmentVariableA(key,out,(DWORD)size);if(!n||n>=size)out[0]=0;}
static uint32_t steam_app_id(void){
 char value[16];env("SteamAppId",value,sizeof(value));
 if(!value[0])env("SteamGameId",value,sizeof(value));
 char *end=NULL;unsigned long long id=value[0]?strtoull(value,&end,10):0;
 return end&&*end==0&&id>=1&&id<=0xffffffffull?(uint32_t)id:5335510u;
}
static uint64_t parse_id(const char *text){char *end=NULL;unsigned long long id=strtoull(text,&end,10);return end&&*end==0?id:0;}
static bool password_size(const char *value,size_t *size){
 size_t n=0;for(;n<=MAX_PASSWORD&&value[n];n++)if((unsigned char)value[n]<32||(unsigned char)value[n]>126)return false;
 if(n<8||n>MAX_PASSWORD)return false;
 *size=n;return true;
}
static bool parse_port(const char *value,int *output){
 unsigned parsed=0;if(!*value)return false;
 for(const unsigned char *p=(const unsigned char *)value;*p;p++){
  if(*p<'0'||*p>'9')return false;
  parsed=parsed*10+(*p-'0');if(parsed>65535)return false;
 }
 if(parsed<1024)return false;
 *output=(int)parsed;return true;
}
static bool is_banned(uint64_t id){for(unsigned i=0;i<ban_count;i++)if(bans[i].id==id)return true;return false;}
static void load_bans(void){
 if(!*ban_path)return;
 FILE *f=fopen(ban_path,"r");if(!f)return;
 char line[160];
 while(ban_count<MAX_BANS&&fgets(line,sizeof(line),f)){
  char *end=NULL;unsigned long long id=strtoull(line,&end,10);if(!id||end==line)continue;
  while(*end==' '||*end=='\t')end++;
  end[strcspn(end,"\r\n")]=0;
  bans[ban_count].id=id;snprintf(bans[ban_count].name,64,"%.63s",end);ban_count++;
 }
 fclose(f);
}
static void save_bans(void){
 if(!*ban_path)return;
 FILE *f=fopen(ban_path,"w");if(!f){report("Could not write the ban list");return;}
 for(unsigned i=0;i<ban_count;i++)fprintf(f,"%llu %s\n",(unsigned long long)bans[i].id,bans[i].name);
 fclose(f);
}
static void configure(void){
 if(configured)return;
 configured=true;
 char value[2048];env("MOON_NET_DEBUG",value,sizeof(value));net_debug=strcmp(value,"1")==0;
 env("MOON_DEDICATED",value,sizeof(value));if(strcmp(value,"1")==0)mode=MODE_SERVER;
 env("MOON_CONNECT",value,sizeof(value));if(*value){server_id=parse_id(value);mode=MODE_CLIENT;}
 env("MOON_GAME_PORT",value,sizeof(value));if(*value&&!parse_port(value,&game_port))ports_valid=false;
 env("MOON_QUERY_PORT",value,sizeof(value));if(*value&&!parse_port(value,&query_port))ports_valid=false;
 env("MOON_SERVER_NAME",value,sizeof(value));if(*value)snprintf(server_name,sizeof(server_name),"%.63s",value);
 value[0]=0;SetLastError(ERROR_SUCCESS);DWORD configured_ids=GetEnvironmentVariableA("MOON_ALLOWED_STEAM_IDS",value,sizeof(value));
 allowlist_set=configured_ids>0||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
 if(allowlist_set&&configured_ids==0)allowlist_set=false;
 if(configured_ids>=sizeof(value)){allowlist_valid=false;value[0]=0;}
 if(allowlist_set){char *p=value;while(*p){
  while(*p==' '||*p=='\t')p++;
  char *end=NULL;uint64_t id=strtoull(p,&end,10);
  if(end==p||!id||((id>>52)&15)!=1||allow_count>=MAX_PEERS){allowlist_valid=false;break;}
  allowlist[allow_count++]=id;p=end;while(*p==' '||*p=='\t')p++;
  if(!*p)break;
  if(*p++!=','||!*p){allowlist_valid=false;break;}
 }}
 int argc=0;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 if(argv){for(int i=1;i<argc;i++){
  if(wcscmp(argv[i],L"-moon-dedicated")==0)mode=MODE_SERVER;
  else if(wcscmp(argv[i],L"-moon-connect")==0&&i+1<argc){char text[64];WideCharToMultiByte(CP_UTF8,0,argv[++i],-1,text,sizeof(text),NULL,NULL);server_id=parse_id(text);mode=MODE_CLIENT;}
 }LocalFree(argv);}
 if(mode==MODE_SERVER){
  wchar_t wide[MAX_PASSWORD+2]={0};DWORD n=GetEnvironmentVariableW(L"MOON_SERVER_PASSWORD",wide,MAX_PASSWORD+2);
  if(n>=8&&n<=MAX_PASSWORD){server_password_valid=true;server_password_length=n;for(DWORD i=0;i<n;i++){if(wide[i]<32||wide[i]>126){server_password_valid=false;break;}server_password[i]=(char)wide[i];}}
  SecureZeroMemory(wide,sizeof(wide));
 }
 if(mode==MODE_SERVER){env("MOON_BAN_LIST",ban_path,sizeof(ban_path));load_bans();}
 if(mode==MODE_CLIENT&&!server_id){mode=MODE_OFF;net_phase="invalid-server-id";}
 if(game_port==query_port)ports_valid=false;
 if(mode==MODE_SERVER&&!ports_valid)net_phase="invalid-server-port";
}
bool MoonNet_Requested(void){configure();return mode!=MODE_OFF;}
bool MoonNet_IsDedicated(void){configure();return mode==MODE_SERVER;}
bool MoonNet_IsClient(void){configure();return mode==MODE_CLIENT;}
bool MoonNet_BackendReady(void){return backend_ready;}
bool MoonNet_IsStopped(void){return shutdown_requested||client_stopped;}
uint64_t MoonNet_ServerId(void){return server_id;}
int MoonNet_GamePort(void){configure();return game_port;}
int MoonNet_QueryPort(void){configure();return query_port;}
const char *MoonNet_Phase(void){return net_phase;}
unsigned MoonNet_AuthenticatedPeers(void){unsigned n=0;for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated)n++;return n;}
void MoonNet_SetWorldReady(bool ready){world_ready=ready;}
static NetIdentity identity(uint64_t id){NetIdentity value={0};value.kind=16;value.bytes=8;value.data.steam_id=id;return value;}
static uint64_t identity_id(const NetIdentity *id){return id&&id->kind==16&&id->bytes==8?id->data.steam_id:0;}
static Peer *peer(uint64_t id,bool create){Peer *free_slot=NULL;for(unsigned i=0;i<MAX_PEERS;i++){if(peers[i].id==id)return &peers[i];if(!peers[i].id&&!free_slot)free_slot=&peers[i];}if(create&&free_slot){memset(free_slot,0,sizeof(*free_slot));free_slot->id=id;free_slot->heard=GetTickCount64();return free_slot;}return NULL;}
static void remember_name(uint64_t id,const char *name){
 for(unsigned i=0;i<KNOWN_NAMES;i++)if(known_names[i].id==id){snprintf(known_names[i].name,64,"%.63s",name);return;}
 known_names[known_next].id=id;snprintf(known_names[known_next].name,64,"%.63s",name);known_next=(known_next+1)%KNOWN_NAMES;
}
static void clean_name(char *name){for(unsigned char *c=(unsigned char *)name;*c;c++)if(*c<32||*c==127)*c='?';}
static const char *name_of(uint64_t id){
 if(id==server_id)return server_name;
 for(unsigned i=0;i<KNOWN_NAMES;i++)if(known_names[i].id==id&&*known_names[i].name)return known_names[i].name;
 return "Crewmate";
}
static bool allowed(uint64_t id){if(is_banned(id))return false;if(!id||((id>>52)&15)!=1||!server_password_valid||!allowlist_valid)return false;if(!allowlist_set)return true;for(unsigned i=0;i<allow_count;i++)if(allowlist[i]==id)return true;return false;}
static bool password_matches(const AuthPayload *auth){
 /* Fixed work for every candidate, including different claimed lengths. */
 volatile uint32_t difference=(uint32_t)server_password_length^auth->password_length;
 for(unsigned i=0;i<MAX_PASSWORD;i++)difference|=(uint8_t)server_password[i]^auth->password[i];
 return server_password_valid&&difference==0;
}
static bool member(uint64_t id){if(mode==MODE_SERVER){Peer *p=peer(id,false);return backend_ready&&(id==server_id||(p&&p->authenticated));}return client_connected&&(id==server_id||peer(id,false)!=NULL);}
static void *klass(const char *name){return il.klass(game_image,"MoonCleaners",name);}
static const Method *method(void *k,const char *name,int count){return k?il.method(k,name,count):NULL;}
static Object *call(const Method *m,Object *self,void **args){if(!m){report("network: required managed method missing");return NULL;}Object *exception=NULL;Object *out=il.invoke(m,self,args,&exception);if(exception){char text[768]={0};il.exception(exception,text,sizeof(text));report(text);}return out;}
static bool boolean(const Method *m){Object *v=call(m,NULL,NULL);return v&&*(uint8_t *)il.unbox(v)!=0;}
static void static_value(const char *name,void *value){if(!game.lobby_class||!il.field)return;void *f=il.field(game.lobby_class,name);if(f)il.static_set(f,value);}
static void set_bool(const char *name,bool value){uint8_t b=value;static_value(name,&b);}
static void set_u64(const char *name,uint64_t value){static_value(name,&value);}
/* Unity 6000.6 field setters take reference objects directly; scalar values
   still use an address. Passing &s would store a native stack address in GC state. */
static void set_string(const char *name,const char *value){Object *s=il.new_string(value);static_value(name,s);}
static void set_member_field(Object *o,void *field,void *value){if(field)il.field_set(o,field,value);}
static void add_member(uint64_t id,const char *name,const char *look_text){
 Object *o=il.new_object(game.member_class);if(!o)return;
 Object *s=il.new_string(name),*look=il.new_string(look_text);uint8_t yes=1,host=id==server_id,local=id==self_id;
 set_member_field(o,game.member_id,&id);set_member_field(o,game.member_name,s);set_member_field(o,game.member_look,look);
 set_member_field(o,game.member_ready,&yes);set_member_field(o,game.member_host,&host);set_member_field(o,game.member_local,&local);
 void *args[]={o};call(game.list_add,game.member_list,args);
}
/* Remote players redraw their astronaut when MoonSteamLobby.Changed fires. The real
   lobby raises it after every member refresh; the adapter's roster has to do the same,
   otherwise a look that arrives after the player spawned is never shown. */
static bool notifying;
static void notify_changed(void){
 if(mode!=MODE_CLIENT||notifying||!game.changed_field)return;
 Object *action=NULL;il.static_get(game.changed_field,&action);if(!action)return;
 const Method *invoke=method(il.object_class(action),"Invoke",0);
 notifying=true;call(invoke,action,NULL);notifying=false;
}
static void publish_members(void){
 if(mode==MODE_SERVER?!backend_ready:!client_connected)return;
 if(!game.member_list)return;
 uint64_t hash=1469598103934665603ULL^server_id^self_id;
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated){hash=(hash^peers[i].id)*1099511628211ULL;for(const unsigned char *s=(const unsigned char *)peers[i].name;*s;s++)hash=(hash^*s)*1099511628211ULL;for(const unsigned char *s=(const unsigned char *)peers[i].look;*s;s++)hash=(hash^*s)*1099511628211ULL;}
 /* A roster heartbeat must not invalidate suspended C# list enumerators. */
 bool roster_changed=hash!=published_roster_hash;
 if(roster_changed){
  call(game.list_clear,game.member_list,NULL);add_member(server_id,server_name,"");
  for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated)add_member(peers[i].id,peers[i].name,peers[i].look);
  published_roster_hash=hash;
 }
 set_u64("lobby",server_id);set_u64("<HostId>k__BackingField",server_id);
 set_bool("<IsHost>k__BackingField",mode==MODE_SERVER);set_bool("<ShiftStarted>k__BackingField",true);set_bool("<Busy>k__BackingField",false);
 int privacy=2;static_value("<CurrentPrivacy>k__BackingField",&privacy);set_string("<CrewName>k__BackingField",server_name);set_string("<Status>k__BackingField","");set_bool("<StatusIsError>k__BackingField",false);
 if(roster_changed)notify_changed();
}
static void clear_members(void){if(game.member_list)call(game.list_clear,game.member_list,NULL);published_roster_hash=0;set_u64("lobby",0);set_u64("<HostId>k__BackingField",0);set_bool("<ShiftStarted>k__BackingField",false);set_bool("<IsHost>k__BackingField",false);}
/* Version-2 clients (before look relaying) are still served in their own wire format. */
static uint16_t reply_version;
static uint16_t peer_version(uint64_t id){Peer *p=mode==MODE_SERVER?peer(id,false):NULL;return p&&p->version?p->version:reply_version?reply_version:CONTROL_VERSION;}
static bool send_control(uint64_t to,uint16_t type,const void *payload,uint32_t bytes){
 if(!messages_api||bytes>4096)return false;
 uint8_t data[sizeof(ControlHeader)+4096];ControlHeader header={CONTROL_MAGIC,peer_version(to),type,bytes};
 memcpy(data,&header,sizeof(header));if(bytes)memcpy(data+sizeof(header),payload,bytes);
 NetIdentity remote=identity(to);int result=sdk.send(messages_api,&remote,data,sizeof(header)+bytes,0x28,CONTROL_CHANNEL);
 if(type<8&&control_results[type]!=result){control_results[type]=result;char text[192];snprintf(text,sizeof(text),"network: control send type=%u peer=%llu EResult=%d",type,(unsigned long long)to,result);debug(text);}
 return result==1;
}
static void send_roster(uint64_t to){
 if(!backend_ready||!server_id)return;
 if(!world_ready){send_control(to,WAIT_WORLD,NULL,0);return;}
 RosterPayload roster={0};roster.server=server_id;roster.count=1;roster.entries[0].id=server_id;snprintf(roster.entries[0].name,64,"%s",server_name);
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated){RosterEntry *e=&roster.entries[roster.count++];e->id=peers[i].id;snprintf(e->name,64,"%s",peers[i].name);snprintf(e->look,LOOK_MAX,"%s",peers[i].look);}
 if(peer_version(to)==LEGACY_VERSION){
  uint8_t legacy[12+(MAX_PEERS+1)*72];memcpy(legacy,&roster,12);
  for(uint32_t i=0;i<roster.count;i++){memcpy(legacy+12+i*72,&roster.entries[i].id,8);memcpy(legacy+12+i*72+8,roster.entries[i].name,64);}
  send_control(to,ROSTER,legacy,12+roster.count*72);return;
 }
 send_control(to,ROSTER,&roster,12+roster.count*sizeof(RosterEntry));
}
static void broadcast_roster(void){if(mode!=MODE_SERVER||!backend_ready)return;publish_members();for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated)send_roster(peers[i].id);}
static void drop_peer(Peer *p,const char *why){
 if(!p||!p->id)return;
 uint64_t id=p->id;
 {char text[256];const char *who=*p->name?p->name:"Unknown player";
  if(strcmp(why,"Disconnected")==0)snprintf(text,sizeof(text),"%s left the server (%llu)",who,(unsigned long long)id);
  else if(p->authenticated)snprintf(text,sizeof(text),"%s was removed: %s (%llu)",who,why,(unsigned long long)id);
  else snprintf(text,sizeof(text),"%s failed to join: %s (%llu)",who,why,(unsigned long long)id);
  report(text);}
 send_control(id,REJECT,why,(uint32_t)strlen(why));
 if(server_api)sdk.end_auth(server_api,id);
 NetIdentity remote=identity(id);if(messages_api)sdk.close(messages_api,&remote);memset(p,0,sizeof(*p));broadcast_roster();
}
static void stop_client(const char *reason,const char *state){
 report(reason);client_stopped=true;MoonNet_Shutdown();clear_members();phase(state);
}
static bool client_timed_out(ULONGLONG now){
 ULONGLONG heard=client_last_heard?client_last_heard:client_auth_started;
 if(heard&&now>=heard&&now-heard>(client_connected?45000:60000)){stop_client("network: server heartbeat timed out","client-server-timeout");return true;}
 return false;
}
static void consume_control(uint64_t from,const ControlHeader *header,const uint8_t *payload){
 if(shutdown_requested||client_stopped)return;
 if(mode==MODE_SERVER){
  if(!backend_ready)return;
  Peer *p=peer(from,false);
  if(header->type==AUTH_REQUEST){
   if(is_banned(from)){send_control(from,REJECT,"You are banned from this server",30);return;}
   if(!allowed(from)||header->bytes<offsetof(AuthPayload,ticket)){send_control(from,REJECT,"Not allowed",11);return;}
   const AuthPayload *auth=(const AuthPayload *)payload;
   if(!auth->bytes||auth->bytes>MAX_TICKET||header->bytes!=offsetof(AuthPayload,ticket)+auth->bytes)return;
   if(!password_matches(auth)){send_control(from,PASSWORD_REJECTED,NULL,0);return;}
   p=peer(from,true);if(!p){send_control(from,REJECT,"Server full",11);return;}p->heard=GetTickCount64();p->version=header->version;
   if(p->authenticated){send_roster(from);return;}
   if(*p->name)return;
   snprintf(p->name,sizeof(p->name),"%.63s",auth->name);p->name[63]=0;
   if(!*p->name)snprintf(p->name,sizeof(p->name),"Crewmate");
   clean_name(p->name);remember_name(from,p->name);
   {char text[192];snprintf(text,sizeof(text),"%s (%llu) is connecting",p->name,(unsigned long long)from);report(text);}
   int result=sdk.begin_auth(server_api,auth->ticket,(int)auth->bytes,from);
   {char text[192];snprintf(text,sizeof(text),"network: BeginAuthSession peer=%llu result=%d",(unsigned long long)from,result);debug(text);}
   if(result!=0){char why[96];snprintf(why,sizeof(why),"Steam rejected auth ticket (%d)",result);drop_peer(p,why);}
  } else if(p){p->heard=GetTickCount64();if(header->type==GOODBYE)drop_peer(p,"Disconnected");else if(header->type==PING&&p->authenticated)send_roster(from);
   else if(header->type==LOOK&&p->authenticated&&header->bytes<LOOK_MAX){
    char look[LOOK_MAX]={0};memcpy(look,payload,header->bytes);bool clean=true;
    for(unsigned i=0;look[i];i++)if((unsigned char)look[i]<32||(unsigned char)look[i]>126)clean=false;
    if(clean&&strcmp(look,p->look)!=0){snprintf(p->look,LOOK_MAX,"%s",look);broadcast_roster();}
   }}
 } else if(from==server_id){
  if(header->type==PASSWORD_REJECTED&&header->bytes==0){MoonPassword_NotifyRejected();stop_client("network: incorrect server password","client-wrong-password");}
  else if(header->type==REJECT)stop_client("network: server rejected authentication/session","client-rejected");
  else if(header->type==WAIT_WORLD&&header->bytes==0){client_last_heard=GetTickCount64();phase("waiting-server-world");}
  else if(header->type==ROSTER&&header->bytes>=12){
   const RosterPayload *r=(const RosterPayload *)payload;
   if(r->server!=server_id||!r->count||r->count>MAX_PEERS+1||header->bytes!=12+r->count*sizeof(RosterEntry))return;
   bool includes_self=false;for(uint32_t i=0;i<r->count;i++)if(r->entries[i].id==self_id)includes_self=true;
   if(!includes_self)return;
   client_last_heard=GetTickCount64();
   SecureZeroMemory(client_password,sizeof(client_password));
   memset(peers,0,sizeof(peers));
   for(uint32_t i=0;i<r->count;i++){const RosterEntry *e=&r->entries[i];if(e->id==server_id){snprintf(server_name,sizeof(server_name),"%.63s",e->name);continue;}Peer *p=peer(e->id,true);if(p){p->authenticated=true;snprintf(p->name,sizeof(p->name),"%.63s",e->name);snprintf(p->look,LOOK_MAX,"%.*s",LOOK_MAX-1,e->look);}}
   last_look[0]=0;look_dirty=true;client_connected=true;backend_ready=true;publish_members();phase("client-authenticated");
  }
 }
}
static void receive_control(void){
 if(!messages_api)return;
 NetMessage *batch[16];int count;
 /* Bound work from untrusted transport traffic so simulation gets every tick. */
 for(unsigned rounds=0;rounds<4;rounds++){count=sdk.receive(messages_api,CONTROL_CHANNEL,batch,16);if(count<0||count>16)return;for(int i=0;i<count;i++){
  NetMessage *m=batch[i];if(m&&m->data&&m->size>=(int)sizeof(ControlHeader)&&m->size<=(int)(sizeof(ControlHeader)+4096)){
   ControlHeader h;memcpy(&h,m->data,sizeof(h));uint64_t from=identity_id(&m->peer);
   if(from&&h.magic==CONTROL_MAGIC&&(h.version==CONTROL_VERSION||(mode==MODE_SERVER&&h.version==LEGACY_VERSION))&&h.bytes==(uint32_t)m->size-sizeof(h)){reply_version=h.version;consume_control(from,&h,(const uint8_t *)m->data+sizeof(h));reply_version=0;}
  }if(m)sdk.release(m);
 }if(count<16)break;}
}
static void transport_callback(CallbackMessage *message){
 if(!message->data)return;
 if(message->id==1251&&message->size>=(int)sizeof(NetIdentity)&&request_logs++<32){
  uint64_t id=identity_id((const NetIdentity *)message->data);char text[192];
  snprintf(text,sizeof(text),"network: session request peer=%llu allowed=%d",(unsigned long long)id,mode==MODE_SERVER?allowed(id):id==server_id);debug(text);
 }else if(message->id==1252&&failure_logs++<16){
  char text[384];
  if(message->size>=(int)sizeof(ConnectionInfo)){ConnectionInfo info;memcpy(&info,message->data,sizeof(info));snprintf(text,sizeof(text),"network: session failed peer=%llu state=%d reason=%d debug=%.127s",(unsigned long long)identity_id(&info.remote),info.state,info.reason,info.debug);}
  else snprintf(text,sizeof(text),"network: short session-failed callback bytes=%d",message->size);
  debug(text);
  if(mode==MODE_SERVER&&message->size>=(int)sizeof(ConnectionInfo)){ConnectionInfo info;memcpy(&info,message->data,sizeof(info));uint64_t id=identity_id(&info.remote);char line[256];snprintf(line,sizeof(line),"Connection with %s (%llu) failed: %.100s",name_of(id),(unsigned long long)id,info.debug);report(line);}
 }
}
static void callback_shape(CallbackMessage *message){
 for(unsigned i=0;i<callback_shape_count;i++)if(callback_shapes[i].id==message->id&&callback_shapes[i].size==message->size)return;
 if(callback_shape_count>=32)return;
 callback_shapes[callback_shape_count].id=message->id;callback_shapes[callback_shape_count++].size=message->size;
 char text[128];snprintf(text,sizeof(text),"network: %s callback id=%d bytes=%d",mode==MODE_SERVER?"server":"client",message->id,message->size);debug(text);
}
static void transport_diagnostic(void){
 ULONGLONG now=GetTickCount64();if(now<next_diagnostic)return;next_diagnostic=now+1000;
 if(sockets_api){AuthenticationStatus status={0};int current=sdk.auth_status(sockets_api,&status);if(current!=last_auth_status){last_auth_status=current;char text[384];snprintf(text,sizeof(text),"network: authentication status=%d debug=%.255s",current,status.debug);debug(text);}}
 if(utils_api){RelayStatus status={0};int current=sdk.relay_status(utils_api,&status);if(current!=last_relay_status||status.config!=last_relay_config||status.relays!=last_relay_reach){last_relay_status=current;last_relay_config=status.config;last_relay_reach=status.relays;char text[384];snprintf(text,sizeof(text),"network: relay status=%d config=%d relays=%d measuring=%d debug=%.255s",current,status.config,status.relays,status.measuring,status.debug);debug(text);}}
}
static const char *auth_failure(int response,int license){
 static const char *const reasons[]={"Steam authentication failed","not connected to Steam","no valid Moon Sweepers license","VAC banned","logged in elsewhere","Steam verification timed out","auth ticket was canceled","auth ticket already used","invalid auth ticket","banned by the publisher"};
 if(response==0&&license!=0)return reasons[2];
 return response>=0&&response<(int)(sizeof(reasons)/sizeof(reasons[0]))?reasons[response]:reasons[0];
}
static void handle_server_callback(CallbackMessage *message){
 callback_shape(message);
 transport_callback(message);
 if(message->id==101){phase("server-steam-connected");}
 else if(message->id==102||message->id==103){
  backend_ready=false;world_ready=false;
  next_backend_ready=GetTickCount64()+1000;
  for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id)drop_peer(&peers[i],"Steam server disconnected; reconnect required");
  server_id=self_id=0;clear_members();
  if(game.set_initialized){uint8_t no=0;void *args[]={&no};call(game.set_initialized,NULL,args);}
  if(game.set_steam_id){uint64_t zero=0;void *args[]={&zero};call(game.set_steam_id,NULL,args);}
  phase("server-steam-disconnected");
 }
 else if(message->id==1251&&message->size>=(int)sizeof(NetIdentity)){
  NetIdentity *remote=(NetIdentity *)message->data;uint64_t id=identity_id(remote);if(backend_ready&&allowed(id))sdk.accept(messages_api,remote);
 /* CSteamID is packed to alignment1 by the SDK, so this callback is20 bytes
    even on Win64: SteamID0, response8, ownerSteamID12. */
 }else if(message->id==143&&message->size>=20){
  uint64_t id;int32_t response;memcpy(&id,message->data,8);memcpy(&response,(uint8_t *)message->data+8,4);Peer *p=peer(id,false);
  if(p){uint32_t appid=steam_app_id();int license=response==0?sdk.has_license(server_api,id,appid):-1;bool licensed=response==0&&(appid!=5335510u||license==0);if(validation_logs++<32){char text[192];snprintf(text,sizeof(text),"network: Steam auth validation peer=%llu response=%d license=%d appid=%u",(unsigned long long)id,response,license,(unsigned)appid);debug(text);}if(licensed){p->authenticated=true;p->heard=GetTickCount64();phase("server-peer-authenticated");{char text[192];snprintf(text,sizeof(text),"%s joined the server (%u online)",p->name,MoonNet_AuthenticatedPeers());report(text);}broadcast_roster();}else drop_peer(p,auth_failure(response,licensed?0:license));}
 }
}
static bool __cdecl observe_callback(int32_t pipe,CallbackMessage *message){
 bool result=real_next_callback(pipe,message);
 if(result){callback_shape(message);transport_callback(message);}
 if(result&&message->id==163&&message->size>=8){uint32_t handle;int32_t response;memcpy(&handle,message->data,4);memcpy(&response,(uint8_t *)message->data+4,4);if(handle==client_ticket){ticket_ready=response==1;ticket_failed=response!=1;}}
 return result;
}
static bool start_server(void){
 if(shutdown_requested)return false;
 if(server_started)return true;
 ULONGLONG now=GetTickCount64();if(now<next_server_start)return false;next_server_start=now+5000;
 char appid[16];env("SteamAppId",appid,sizeof(appid));if(!appid[0])snprintf(appid,sizeof(appid),"%u",steam_app_id());SetEnvironmentVariableA("SteamAppId",appid);
 if(!sdk.gs_init(0,0,(uint16_t)game_port,(uint16_t)query_port,2,"1.0.5")){phase("server-init-failed");return false;}
 server_api=sdk.gs();messages_api=sdk.gs_messages();if(!server_api||!messages_api){sdk.shutdown();server_api=messages_api=NULL;phase("server-interface-failed");return false;}
 sdk.set_product(server_api,appid);sdk.set_description(server_api,"Moon Sweepers");sdk.set_mod(server_api,"Moon Sweepers");sdk.set_name(server_api,server_name);sdk.set_dedicated(server_api,true);sdk.set_advertise(server_api,false);sdk.set_capacity(server_api,16);
 sdk.dispatch_init();server_pipe=sdk.gs_pipe();sdk.logon(server_api);sockets_api=sdk.gs_sockets();utils_api=sdk.utils();if(utils_api){sdk.init_relay(utils_api);tune_network(utils_api,true);}
 server_started=true;phase("server-logging-on-anonymously");return true;
}
static void __cdecl server_start_steam(Object *self,const Method *info){(void)self;(void)info;start_server();}
static uint64_t __cdecl server_steam_id(const Method *info){(void)info;return server_id;}
static bool __cdecl lobby_present(const Method *info){(void)info;return mode==MODE_SERVER?backend_ready:client_connected;}
static bool __cdecl lobby_host(const Method *info){(void)info;return mode==MODE_SERVER&&backend_ready;}
static uint64_t __cdecl lobby_id(const Method *info){(void)info;return server_id;}
static bool __cdecl lobby_member(uint64_t id,const Method *info){(void)info;return member(id);}
static void __cdecl noop_static(const Method *info){(void)info;}
/* Lobby member data is gone, so a client's look travels over the control channel. */
static void __cdecl share_look(const Method *info){(void)info;if(mode==MODE_CLIENT)look_dirty=true;}
static Object *__cdecl player_name(uint64_t id,const Method *info){(void)info;return il.new_string(name_of(id));}
static void __cdecl noop_create(int privacy,int capacity,const Method *info){(void)privacy;(void)capacity;(void)info;}
static void __cdecl noop_privacy(int privacy,const Method *info){(void)privacy;(void)info;}
static void __cdecl noop_ready(bool ready,const Method *info){(void)ready;(void)info;}
static void __cdecl leave_direct(const Method *info){(void)info;if(mode==MODE_CLIENT)client_stopped=true;MoonNet_Shutdown();clear_members();phase("disconnected");}
static Object *__cdecl no_avatar(uint64_t id,const Method *info){(void)id;(void)info;return NULL;}
static void __cdecl refresh_members(const Method *info){(void)info;if(mode==MODE_SERVER?backend_ready:client_connected)publish_members();}
static void __cdecl start_shift(const Method *info){(void)info;if(mode==MODE_SERVER&&backend_ready)call(game.session_start,NULL,NULL);}
static int __cdecl net_send(const void *remote,const void *data,uint32_t size,int flags,int channel,const Method *info){(void)info;return messages_api?marshal.send(messages_api,remote,data,size,flags,channel,marshal.send_method):3;}
static int __cdecl net_receive(int channel,Object *array,int capacity,const Method *info){
 (void)info;if(!messages_api||!array||capacity<0)return 0;uintptr_t length=il.array_length(array);if((uintptr_t)capacity>length)capacity=(int)length;
 return marshal.receive(messages_api,channel,array,capacity,marshal.receive_method);
}
static bool __cdecl net_accept(const void *remote,const Method *info){(void)info;return messages_api&&marshal.accept(messages_api,remote,marshal.accept_method);}
static bool __cdecl net_close(const void *remote,const Method *info){(void)info;return messages_api&&marshal.close(messages_api,remote,marshal.close_method);}
static bool __cdecl net_close_channel(const void *remote,int channel,const Method *info){(void)info;return messages_api&&marshal.close_channel(messages_api,remote,channel,marshal.close_channel_method);}
static int __cdecl net_info(const void *remote,void *details,void *status,const Method *info){(void)info;return messages_api?marshal.info(messages_api,remote,details,status,marshal.info_method):0;}
static unsigned packet_kind(Object *array){return array&&il.array_length(array)?*((const uint8_t *)array+il.array_header()):0;}
static void packet_log(bool incoming,uint64_t remote,int channel,unsigned kind,int bytes,bool result){
 if(!net_debug||channel<1||channel>2||packet_logs[incoming?1:0][channel]++>=8)return;
 char text[192];snprintf(text,sizeof(text),"network: game %s peer=%llu channel=%d kind=0x%02x bytes=%d result=%d",incoming?"receive":"send",(unsigned long long)remote,channel,kind,bytes,result);debug(text);
}
/* Join progress, so a slow or stuck world download shows up in the console instead of just "Joining". */
static void track_send(uint64_t to,int channel,int bytes){
 if(mode!=MODE_SERVER||channel!=2)return;
 Peer *p=peer(to,false);if(!p||!p->authenticated)return;
 ULONGLONG now=GetTickCount64();char text[192];
 if(!p->first_sent){p->first_sent=now;p->next_report=4u<<20;snprintf(text,sizeof(text),"Sending the world to %s",p->name);report(text);}
 p->sent+=(uint64_t)(bytes>0?bytes:0);
 if(p->sent>=p->next_report){
  snprintf(text,sizeof(text),"World transfer to %s: %.1f MB sent in %.0f s",p->name,(double)p->sent/1048576.0,(double)(now-p->first_sent)/1000.0);report(text);
  while(p->next_report<=p->sent)p->next_report+=4u<<20;
 }
}
/* Ordinary world messages only start once the client has the world and is in the game. */
static void track_receive(uint64_t from,int channel,unsigned kind){
 if(mode!=MODE_SERVER)return;
 Peer *p=peer(from,false);if(!p||!p->authenticated)return;
 ULONGLONG now=GetTickCount64();char text[192];
 if(channel==2&&kind==0x14){
  if(!p->asked){p->asked=now;p->wait_logged=now;snprintf(text,sizeof(text),"%s is joining: asked for the world",p->name);report(text);}
  else if(!p->first_sent&&now-p->wait_logged>=15000){p->wait_logged=now;snprintf(text,sizeof(text),"%s has been waiting %.0f s for the world and nothing has been sent",p->name,(double)(now-p->asked)/1000.0);report(text);}
 }else if(channel==1&&p->first_sent&&!p->entered){
  p->entered=true;snprintf(text,sizeof(text),"%s entered the world (%.0f s after asking, %.1f MB sent)",p->name,(double)(now-p->asked)/1000.0,(double)p->sent/1048576.0);report(text);
 }
}
static bool __cdecl send_bytes(Object *self,uint64_t to,Object *data,int bytes,int channel,bool reliable,const Method *info){
 unsigned kind=packet_kind(data);bool result=real_send_bytes(self,to,data,bytes,channel,reliable,info);packet_log(false,to,channel,kind,bytes,result);if(result)track_send(to,channel,bytes);return result;
}
static void __cdecl receive_transfer(Object *self,uint64_t from,Object *data,const Method *info){
 packet_log(true,from,2,packet_kind(data),data?(int)il.array_length(data):0,true);track_receive(from,2,packet_kind(data));real_receive_transfer(self,from,data,info);
}
static void __cdecl receive_world(Object *self,uint64_t from,Object *data,const Method *info){
 packet_log(true,from,1,packet_kind(data),data?(int)il.array_length(data):0,true);track_receive(from,1,packet_kind(data));real_receive_world(self,from,data,info);
}
static void __cdecl steam_destroy(Object *self,const Method *info){MoonNet_Shutdown();uint8_t no=0;void *args[]={&no};call(game.set_initialized,NULL,args);real_steam_destroy(self,info);}

static void *managed_code(const Method *m){
 if(!m)return NULL;
 void *address=*(void *const *)m;MEMORY_BASIC_INFORMATION v;
 if(!address||VirtualQuery(address,&v,sizeof(v))!=sizeof(v)||v.State!=MEM_COMMIT||(v.Protect&PAGE_GUARD)||!(v.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return NULL;
 return address;
}
static bool hook_method(void *k,const char *name,int argc,void *replacement,void **original){
 const Method *m=method(k,name,argc);void *address=managed_code(m);if(!address){report(name);return false;}
 MH_STATUS result=MH_CreateHook(address,replacement,original);if(result!=MH_OK)return false;return MH_EnableHook(address)==MH_OK;
}
static bool load_apis(void){
 game_module=GetModuleHandleW(L"GameAssembly.dll");if(!game_module)return false;
#define IL(member,name) do{FARPROC p=GetProcAddress(game_module,"il2cpp_" name);memcpy(&il.member,&p,sizeof(p));if(!il.member){report("Missing il2cpp_" name);return false;}}while(0)
 IL(assemblies,"domain_get_assemblies");IL(domain,"domain_get");IL(image,"assembly_get_image");IL(image_name,"image_get_name");IL(klass,"class_from_name");IL(method,"class_get_method_from_name");IL(field,"class_get_field_from_name");IL(field_get,"field_get_value");IL(field_set,"field_set_value");IL(static_get,"field_static_get_value");IL(static_set,"field_static_set_value");IL(invoke,"runtime_invoke");IL(new_object,"object_new");IL(new_string,"string_new");IL(object_class,"object_get_class");IL(unbox,"object_unbox");IL(class_init,"runtime_class_init");IL(array_header,"array_object_header_size");IL(array_length,"array_length");IL(string_length,"string_length");IL(string_chars,"string_chars");IL(exception,"format_exception");
#undef IL
 steam_module=GetModuleHandleW(L"steam_api64.dll");
 if(!steam_module){wchar_t path[2048];GetModuleFileNameW(NULL,path,2048);wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return false;wcscpy(slash+1,L"MoonSweepers_Data\\Plugins\\x86_64\\steam_api64.dll");steam_module=LoadLibraryW(path);}
 if(!steam_module)return false;
#define SDK(member,name) do{FARPROC p=GetProcAddress(steam_module,name);memcpy(&sdk.member,&p,sizeof(p));if(!sdk.member){report("Missing " name);return false;}}while(0)
 SDK(gs_init,"SteamInternal_GameServer_Init");SDK(gs,"SteamAPI_SteamGameServer_v015");SDK(gs_messages,"SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002");SDK(client_messages,"SteamAPI_SteamNetworkingMessages_SteamAPI_v002");SDK(gs_sockets,"SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012");SDK(client_sockets,"SteamAPI_SteamNetworkingSockets_SteamAPI_v012");SDK(utils,"SteamAPI_SteamNetworkingUtils_SteamAPI_v004");SDK(user,"SteamAPI_SteamUser_v023");
 SDK(set_product,"SteamAPI_ISteamGameServer_SetProduct");SDK(set_description,"SteamAPI_ISteamGameServer_SetGameDescription");SDK(set_mod,"SteamAPI_ISteamGameServer_SetModDir");SDK(set_name,"SteamAPI_ISteamGameServer_SetServerName");SDK(set_dedicated,"SteamAPI_ISteamGameServer_SetDedicatedServer");SDK(set_advertise,"SteamAPI_ISteamGameServer_SetAdvertiseServerActive");SDK(set_capacity,"SteamAPI_ISteamGameServer_SetMaxPlayerCount");SDK(logon,"SteamAPI_ISteamGameServer_LogOnAnonymous");SDK(logged_on,"SteamAPI_ISteamGameServer_BLoggedOn");SDK(gs_id,"SteamAPI_ISteamGameServer_GetSteamID");SDK(begin_auth,"SteamAPI_ISteamGameServer_BeginAuthSession");SDK(end_auth,"SteamAPI_ISteamGameServer_EndAuthSession");SDK(has_license,"SteamAPI_ISteamGameServer_UserHasLicenseForApp");SDK(init_auth,"SteamAPI_ISteamNetworkingSockets_InitAuthentication");SDK(shutdown,"SteamGameServer_Shutdown");
 SDK(dispatch_init,"SteamAPI_ManualDispatch_Init");SDK(gs_pipe,"SteamGameServer_GetHSteamPipe");SDK(run_frame,"SteamAPI_ManualDispatch_RunFrame");SDK(next_callback,"SteamAPI_ManualDispatch_GetNextCallback");SDK(free_callback,"SteamAPI_ManualDispatch_FreeLastCallback");
 SDK(auth_status,"SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");SDK(init_relay,"SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");SDK(relay_status,"SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
 SDK(send,"SteamAPI_ISteamNetworkingMessages_SendMessageToUser");SDK(receive,"SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");SDK(accept,"SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");SDK(close,"SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");SDK(close_channel,"SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser");SDK(session_info,"SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");SDK(release,"SteamAPI_SteamNetworkingMessage_t_Release");SDK(user_id,"SteamAPI_ISteamUser_GetSteamID");SDK(auth_ticket,"SteamAPI_ISteamUser_GetAuthSessionTicket");SDK(cancel_ticket,"SteamAPI_ISteamUser_CancelAuthTicket");
#undef SDK
 {FARPROC p=GetProcAddress(steam_module,"SteamAPI_ISteamNetworkingUtils_SetConfigValue");memcpy(&sdk.set_config,&p,sizeof(p));}
 {FARPROC p=GetProcAddress(steam_module,"SteamAPI_ISteamNetworkingUtils_GetConfigValue");memcpy(&sdk.get_config,&p,sizeof(p));}
 size_t count=0;const void **assemblies=il.assemblies(il.domain(),&count);
 for(size_t i=0;i<count;i++){const void *image=il.image(assemblies[i]);if(strcmp(il.image_name(image),"com.rlabrecque.steamworks.net.dll")==0)steam_image=image;}
 return steam_image!=NULL;
}
static bool resolve_game(void){
 game.lobby_class=klass("MoonSteamLobby");game.member_class=il.klass(game_image,"MoonCleaners","MoonSteamLobby/Member");
 /* class_from_name uses the metadata's nested-name spelling inconsistently;
    enumerate nested types through the public runtime API if necessary. */
 if(!game.member_class){void *(__cdecl *nested)(void *,void **)=NULL;const char *(__cdecl *name)(void *)=NULL;FARPROC p=GetProcAddress(game_module,"il2cpp_class_get_nested_types");memcpy(&nested,&p,sizeof(p));p=GetProcAddress(game_module,"il2cpp_class_get_name");memcpy(&name,&p,sizeof(p));if(!nested||!name)return false;void *it=NULL,*k;while((k=nested(game.lobby_class,&it))!=NULL)if(strcmp(name(k),"Member")==0){game.member_class=k;break;}}
 game.crew_class=klass("MoonCrewSession");
 {void *(__cdecl *nested)(void *,void **)=NULL;const char *(__cdecl *name)(void *)=NULL;FARPROC p=GetProcAddress(game_module,"il2cpp_class_get_nested_types");memcpy(&nested,&p,sizeof(p));p=GetProcAddress(game_module,"il2cpp_class_get_name");memcpy(&name,&p,sizeof(p));if(!nested||!name)return false;void *it=NULL,*k;while((k=nested(game.crew_class,&it))!=NULL)if(strcmp(name(k),"CrewJoin")==0){game.join_class=k;break;}}
 if(!game.lobby_class||!game.member_class||!game.crew_class||!game.join_class)return false;
 il.class_init(game.lobby_class);game.list_field=il.field(game.lobby_class,"members");game.changed_field=il.field(game.lobby_class,"Changed");if(!game.list_field)return false;il.static_get(game.list_field,&game.member_list);if(!game.member_list)return false;
 void *list_class=il.object_class(game.member_list);game.list_clear=method(list_class,"Clear",0);game.list_add=method(list_class,"Add",1);
#define FIELD(member,name) do{game.member=il.field(game.member_class,name);if(!game.member)return false;}while(0)
 FIELD(member_id,"id");FIELD(member_name,"name");FIELD(member_look,"look");FIELD(member_ready,"ready");FIELD(member_host,"host");FIELD(member_local,"local");
#undef FIELD
 void *steam=klass("MoonSteam"),*session=klass("MoonSession");
#define METHOD(member,k,name,n) do{game.member=method(k,name,n);if(!game.member){report("Missing game method " name);return false;}}while(0)
 METHOD(steam_initialized,steam,"get_Initialized",0);METHOD(set_initialized,steam,"set_Initialized",1);METHOD(set_steam_id,steam,"set_SteamId",1);METHOD(set_persona,steam,"set_PersonaName",1);METHOD(persona,steam,"get_PersonaName",0);METHOD(crew_create,game.crew_class,"Create",0);METHOD(crew_instance,game.crew_class,"get_Instance",0);METHOD(session_start,session,"StartShift",0);METHOD(in_menu,session,"get_InMenu",0);METHOD(newhire_active,klass("MoonNewHire"),"get_Active",0);METHOD(begin_join,game.crew_class,"BeginJoin",1);METHOD(join_ctor,game.join_class,".ctor",3);
#undef METHOD
 {void *look=klass("MoonAstronautLook");game.look_local=method(look,"get_Local",0);game.look_encode=method(look,"Encode",0);}
 return game.list_clear&&game.list_add;
}
static bool resolve_marshalling(void){
 void *native=il.klass(steam_image,"Steamworks","NativeMethods");if(!native)return false;
#define STUB(member,name,n) do{marshal.member##_method=method(native,"ISteamNetworkingMessages_" name,n);void *address=managed_code(marshal.member##_method);if(!address){report("Missing networking marshal stub " name);return false;}memcpy(&marshal.member,&address,sizeof(address));}while(0)
 STUB(send,"SendMessageToUser",6);STUB(receive,"ReceiveMessagesOnChannel",4);STUB(accept,"AcceptSessionWithUser",2);STUB(close,"CloseSessionWithUser",2);STUB(close_channel,"CloseChannelWithUser",3);STUB(info,"GetSessionConnectionInfo",4);
#undef STUB
 return true;
}
bool MoonNet_Install(const void *image,void (*log_line)(const char *)){
 configure();logger=log_line;if(mode==MODE_OFF)return true;if(installed)return true;game_image=image;
 if(mode==MODE_SERVER&&!ports_valid){report("MOON_GAME_PORT and MOON_QUERY_PORT must be distinct numbers from 1024 to 65535");phase("server-port-invalid");return false;}
 if(mode==MODE_SERVER&&!server_password_valid){report("MOON_SERVER_PASSWORD is required: use 8..128 printable ASCII characters");phase("server-password-invalid");return false;}
 if(!load_apis()||!resolve_game()||(mode==MODE_SERVER&&!resolve_marshalling())){phase("network-install-failed");return false;}
 bool ok=true;
#define HOOK(k,name,n,fn) do{if(!hook_method(k,name,n,fn,NULL)){report("Network hook failed: " name);ok=false;}}while(0)
 HOOK(game.lobby_class,"get_InLobby",0,lobby_present);HOOK(game.lobby_class,"get_IsHost",0,lobby_host);HOOK(game.lobby_class,"get_LobbyId",0,lobby_id);HOOK(game.lobby_class,"get_HostId",0,lobby_id);HOOK(game.lobby_class,"get_ShiftStarted",0,lobby_present);
 HOOK(game.lobby_class,"IsMember",1,lobby_member);HOOK(game.lobby_class,"InSteamLobby",1,lobby_member);HOOK(game.lobby_class,"RefreshMembers",0,refresh_members);HOOK(game.lobby_class,"RefreshMembersNow",0,refresh_members);HOOK(game.lobby_class,"RefreshLobby",0,refresh_members);
 HOOK(game.lobby_class,"UpdatePresence",0,noop_static);HOOK(game.lobby_class,"ShareLook",0,share_look);HOOK(game.lobby_class,"InviteFriends",0,noop_static);HOOK(game.lobby_class,"PublishRemovals",0,noop_static);HOOK(game.lobby_class,"Create",2,noop_create);HOOK(game.lobby_class,"SetPrivacy",1,noop_privacy);HOOK(game.lobby_class,"SetReady",1,noop_ready);HOOK(game.lobby_class,"StartShift",0,start_shift);HOOK(game.lobby_class,"Leave",0,leave_direct);
 if(!hook_method(game.crew_class,"SendBytes",5,send_bytes,(void **)&real_send_bytes)||!hook_method(game.crew_class,"ReceiveTransfer",2,receive_transfer,(void **)&real_receive_transfer)||!hook_method(game.crew_class,"ReceiveWorld",2,receive_world,(void **)&real_receive_world))ok=false;
 if(mode==MODE_SERVER){
  HOOK(klass("MoonSteam"),"StartSteam",0,server_start_steam);
  void *network=il.klass(steam_image,"Steamworks","SteamNetworkingMessages");
  HOOK(network,"SendMessageToUser",5,net_send);HOOK(network,"ReceiveMessagesOnChannel",3,net_receive);HOOK(network,"AcceptSessionWithUser",1,net_accept);HOOK(network,"CloseSessionWithUser",1,net_close);HOOK(network,"CloseChannelWithUser",2,net_close_channel);HOOK(network,"GetSessionConnectionInfo",3,net_info);
  HOOK(il.klass(steam_image,"Steamworks","SteamUser"),"GetSteamID",0,server_steam_id);
  HOOK(game.lobby_class,"Avatar",1,no_avatar);HOOK(game.lobby_class,"PlayerName",1,player_name);
  if(!hook_method(klass("MoonSteam"),"OnDestroy",0,steam_destroy,(void **)&real_steam_destroy))ok=false;
 }else{
  MH_STATUS h=MH_CreateHook((void *)sdk.next_callback,observe_callback,(void **)&real_next_callback);if(h!=MH_OK||MH_EnableHook((void *)sdk.next_callback)!=MH_OK)ok=false;
 }
#undef HOOK
 if(!ok){phase("network-hook-failed");return false;}installed=true;
 if(mode==MODE_SERVER&&allowlist_set&&!allowlist_valid)report("Invalid MOON_ALLOWED_STEAM_IDS: all player authentication is blocked");
 phase(mode==MODE_SERVER?"server-waiting-start":"client-waiting-steam");return true;
}
/* Administrator commands (players, kick, ban, unban); run on the Unity thread. */
static bool text_matches(const char *name,const char *query,bool exact){
 size_t n=strlen(name),q=strlen(query);if(!q||q>n)return false;
 if(exact)return n==q&&_strnicmp(name,query,q)==0;
 for(size_t i=0;i+q<=n;i++)if(_strnicmp(name+i,query,q)==0)return true;
 return false;
}
static Peer *find_peer(const char *query,bool *ambiguous){
 *ambiguous=false;uint64_t id=parse_id(query);
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated&&id&&peers[i].id==id)return &peers[i];
 for(unsigned pass=0;pass<2;pass++){Peer *found=NULL;unsigned matches=0;
  for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated&&text_matches(peers[i].name,query,pass==0)){found=&peers[i];matches++;}
  if(matches==1)return found;
  if(matches>1){*ambiguous=true;return NULL;}}
 return NULL;
}
static void command_players(void){
 unsigned n=MoonNet_AuthenticatedPeers();char line[256];snprintf(line,sizeof(line),"Players online: %u",n);report(line);
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated){snprintf(line,sizeof(line),"  %s (%llu)",peers[i].name,(unsigned long long)peers[i].id);report(line);}
}
static void command_kick(const char *query,bool ban){
 char line[256];bool ambiguous=false;Peer *p=*query?find_peer(query,&ambiguous):NULL;
 if(!*query){report(ban?"Usage: ban <player name or SteamID64>":"Usage: kick <player name or SteamID64>");return;}
 if(!p&&ambiguous){snprintf(line,sizeof(line),"More than one player matches '%.60s'; use the SteamID64 (see 'players')",query);report(line);return;}
 uint64_t id=p?p->id:parse_id(query);
 if(!p&&(!id||((id>>52)&15)!=1)){snprintf(line,sizeof(line),"No player named or numbered '%.60s' is connected",query);report(line);return;}
 if(ban&&!is_banned(id)){
  if(ban_count>=MAX_BANS){report("Ban list is full");return;}
  bans[ban_count].id=id;snprintf(bans[ban_count].name,64,"%s",p?p->name:name_of(id));ban_count++;save_bans();
  snprintf(line,sizeof(line),"Banned %s (%llu)",bans[ban_count-1].name,(unsigned long long)id);report(line);
 }
 if(p)drop_peer(p,ban?"Banned by an administrator":"Kicked by an administrator");
 else if(!ban)report("That player is not connected");
}
static void command_unban(const char *query){
 char line[256];if(!*query){report("Usage: unban <SteamID64 or name>");return;}
 uint64_t id=parse_id(query);
 for(unsigned i=0;i<ban_count;i++)if((id&&bans[i].id==id)||(!id&&text_matches(bans[i].name,query,true))){
  snprintf(line,sizeof(line),"Unbanned %s (%llu)",bans[i].name,(unsigned long long)bans[i].id);report(line);
  memmove(&bans[i],&bans[i+1],(ban_count-i-1)*sizeof(bans[0]));ban_count--;save_bans();return;
 }
 snprintf(line,sizeof(line),"'%.60s' is not on the ban list",query);report(line);
}
bool MoonNet_Command(const char *verb,const char *arg){
 if(mode!=MODE_SERVER)return false;
 if(strcmp(verb,"players")==0)command_players();
 else if(strcmp(verb,"kick")==0)command_kick(arg,false);
 else if(strcmp(verb,"ban")==0)command_kick(arg,true);
 else if(strcmp(verb,"unban")==0)command_unban(arg);
 else return false;
 return true;
}
bool MoonNet_PlayerAt(unsigned index,uint64_t *id,char *name,size_t size){
 if(mode!=MODE_SERVER)return false;
 unsigned seen=0;
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated){if(seen++==index){*id=peers[i].id;snprintf(name,size,"%s",peers[i].name);return true;}}
 return false;
}
static void tick_server(void){
 if(!server_started&&!start_server())return;
 sdk.run_frame(server_pipe);CallbackMessage message;
 while(sdk.next_callback(server_pipe,&message)){handle_server_callback(&message);sdk.free_callback(server_pipe);}
 if(GetTickCount64()>=next_backend_ready&&sdk.logged_on(server_api)){
  uint64_t id=sdk.gs_id(server_api);
  if(id&&!backend_ready){server_id=id;self_id=id;backend_ready=true;if(sockets_api){int result=sdk.init_auth(sockets_api);char text[128];snprintf(text,sizeof(text),"network: InitAuthentication result=%d",result);debug(text);}uint8_t yes=1;void *a[]={&id};call(game.set_steam_id,NULL,a);Object *name=il.new_string(server_name);void *b[]={name};call(game.set_persona,NULL,b);void *c[]={&yes};call(game.set_initialized,NULL,c);publish_members();if(!call(game.crew_instance,NULL,NULL))call(game.crew_create,NULL,NULL);char text[128];snprintf(text,sizeof(text),"Anonymous server SteamID: %llu",(unsigned long long)id);report(text);phase("server-authenticated-anonymous");}
 }
 transport_diagnostic();receive_control();ULONGLONG now=GetTickCount64();
 if(now>=next_roster){next_roster=now+5000;broadcast_roster();}
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&now-peers[i].heard>45000)drop_peer(&peers[i],"Session timed out");
}
static void begin_client_join(void){
 if(client_join_started||!client_connected||!boolean(game.in_menu)||boolean(game.newhire_active))return;
 Object *crew=call(game.crew_instance,NULL,NULL);if(!crew)return;
 Object *join=il.new_object(game.join_class),*name=il.new_string(server_name);if(!join)return;
 void *ctor[]={&server_id,&server_id,name};call(game.join_ctor,join,ctor);void *args[]={join};call(game.begin_join,crew,args);client_join_started=true;phase("client-downloading-world");
}
static void tick_client(void){
 if(client_stopped||!boolean(game.steam_initialized))return;
 if(!user_api){user_api=sdk.user();messages_api=sdk.client_messages();if(!user_api||!messages_api)return;sockets_api=sdk.client_sockets();utils_api=sdk.utils();if(utils_api){sdk.init_relay(utils_api);tune_network(utils_api,false);}self_id=sdk.user_id(user_api);char text[128];snprintf(text,sizeof(text),"Client SteamID: %llu",(unsigned long long)self_id);report(text);}
 if(!client_password_ready){int result=MoonPassword_Read(client_password,sizeof(client_password));if(result==0){phase("client-waiting-password");return;}if(result<0){stop_client("network: password entry cancelled","client-password-cancelled");return;}if(!password_size(client_password,&client_password_length)){stop_client("network: password must contain 8..128 printable ASCII characters","client-password-invalid");return;}client_password_ready=true;}
 ULONGLONG now=GetTickCount64();
 if(!client_ticket){NetIdentity remote=identity(server_id);ticket_size=0;client_ticket=sdk.auth_ticket(user_api,ticket_bytes,MAX_TICKET,&ticket_size,&remote);ticket_requested_at=now;ticket_ready=false;ticket_failed=false;if(!client_ticket||!ticket_size||ticket_size>MAX_TICKET){stop_client("network: Steam could not issue an auth ticket","client-ticket-failed");return;}phase("client-waiting-ticket");}
 if(ticket_failed){stop_client("network: Steam rejected the auth ticket","client-ticket-rejected-by-steam");return;}
 if(!ticket_ready&&now-ticket_requested_at>30000){stop_client("network: Steam auth ticket callback timed out","client-ticket-timeout");return;}
 if(ticket_ready&&!client_connected&&now>=next_auth){
  next_auth=now+3000;AuthPayload auth={0};auth.bytes=ticket_size;auth.password_length=(uint16_t)client_password_length;memcpy(auth.password,client_password,client_password_length);Object *name=call(game.persona,NULL,NULL);
  if(name)WideCharToMultiByte(CP_UTF8,0,il.string_chars(name),il.string_length(name),auth.name,63,NULL,NULL);
  if(!*auth.name)snprintf(auth.name,64,"Crewmate");
  memcpy(auth.ticket,ticket_bytes,ticket_size);
  if(!client_auth_started)client_auth_started=now;
  send_control(server_id,AUTH_REQUEST,&auth,(uint32_t)offsetof(AuthPayload,ticket)+ticket_size);SecureZeroMemory(&auth,sizeof(auth));phase("client-authenticating");
 }
 transport_diagnostic();receive_control();
 if(client_stopped||shutdown_requested)return;
 if(client_connected&&(look_dirty||now>=next_look)&&game.look_local&&game.look_encode){
  look_dirty=false;next_look=now+5000;Object *look=call(game.look_local,NULL,NULL),*text=look?call(game.look_encode,look,NULL):NULL;
  char encoded[LOOK_MAX]={0};if(text&&il.string_length(text)>0)WideCharToMultiByte(CP_UTF8,0,il.string_chars(text),il.string_length(text),encoded,LOOK_MAX-1,NULL,NULL);
  if(*encoded&&strcmp(encoded,last_look)!=0){if(send_control(server_id,LOOK,encoded,(uint32_t)strlen(encoded)+1))snprintf(last_look,LOOK_MAX,"%s",encoded);}
 }
 if(client_timed_out(GetTickCount64()))return;
 if(client_connected&&now>=next_ping){next_ping=now+5000;send_control(server_id,PING,NULL,0);}
 begin_client_join();
}
void MoonNet_Tick(void){if(!installed||shutdown_requested)return;if(mode==MODE_SERVER)tick_server();else if(mode==MODE_CLIENT)tick_client();}
void MoonNet_KickAll(const char *reason){
 if(mode!=MODE_SERVER||!server_started)return;
 for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id&&peers[i].authenticated)drop_peer(&peers[i],reason);
 /* Give the reject messages a moment to leave before the Steam connection is torn down. */
 Sleep(750);
}
void MoonNet_Shutdown(void){
 shutdown_requested=true;
 if(mode==MODE_SERVER&&server_started){for(unsigned i=0;i<MAX_PEERS;i++)if(peers[i].id)sdk.end_auth(server_api,peers[i].id);sdk.shutdown();server_started=false;backend_ready=false;}
 else if(mode==MODE_CLIENT&&user_api){if(client_connected)send_control(server_id,GOODBYE,NULL,0);if(client_ticket)sdk.cancel_ticket(user_api,client_ticket);client_ticket=0;client_connected=false;}
 backend_ready=false;world_ready=false;
 client_connected=false;
 SecureZeroMemory(client_password,sizeof(client_password));SecureZeroMemory(ticket_bytes,sizeof(ticket_bytes));client_password_ready=false;
 memset(peers,0,sizeof(peers));
 if(mode==MODE_SERVER&&game.set_initialized){uint8_t no=0;void *args[]={&no};call(game.set_initialized,NULL,args);}
}
