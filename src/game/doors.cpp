#include "game/doors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/camera.h"
#include "game/door_policy.h"
#include "game/level.h"
#include "game/raycast.h"
#include "net/net.h"

namespace mecmp::game::doors {
namespace {
using Key=net::doors::Key;
using InitFn=uintptr_t(*)(void*);
using DestroyFn=uintptr_t(*)(void*,uint32_t);
using ReadFn=void(*)(void*,void*,int*,int*,void*);
using GetFn=uint8_t(*)(uintptr_t,uintptr_t);
using SetFn=void(*)(uintptr_t,uintptr_t,uint8_t);
constexpr uintptr_t kDriver=0x141c43850,kAnt=0x141b06770,kReader=0x141b06e50,kBool=0x141a435c8;
constexpr uintptr_t kInitSlot=0x141c438c8,kDestroySlot=0x141c43888,kReadSlot=0x141b7ad30;
constexpr uintptr_t kInit=0x143933cd0,kDestroy=0x14392b200,kRead=0x14342d510,kGet=0x142c34a70,kSet=0x142c3ea40;
constexpr uintptr_t kManager=0x1423dd0b8;
constexpr size_t kMaxDrivers=16384;
struct Driver {
    uintptr_t address=0,data=0,ant=0,mesh=0;
    float placement[16]{};
    Key key;
    door_policy::Observation observation;
    uint64_t sampled=0,applied=0;
    bool pending=false,closed=false;
};
std::mutex g_mutex;
std::map<uintptr_t,Driver> g_drivers; // Indexed by ANT identity, never sent over the network.
std::atomic<bool> g_installed{false},g_bankReady{false},g_request{false};
std::atomic<uint64_t> g_world{0},g_readyAt{0},g_targetAt{0};
door_policy::Lifecycle g_lifecycle;
Key g_target,g_command;
uint64_t g_commandUntil=0;
std::atomic<uintptr_t> g_manager{0},g_table{0},g_closed{0},g_pulse{0};
uint32_t g_closedId=0,g_pulseId=0;
InitFn g_init=nullptr;
DestroyFn g_destroy=nullptr;
ReadFn g_read=nullptr;
uintptr_t q(uintptr_t at) { return mem::readOr<uintptr_t>(at); }
bool snapshot(uintptr_t address,Driver& out) {
    if (q(address)!=mem::rva(kDriver)) return false;
    Driver d;d.address=address;d.data=q(address+0x28);d.mesh=q(address+0x40);
    const auto iface=q(address+0x38);
    if (iface<0x60||!d.data||!d.mesh) return false;
    d.ant=iface-0x60;
    if (q(d.ant)!=mem::rva(kAnt)||!mem::read(address+0x90,d.placement,sizeof(d.placement))) return false;
    Key key;
    if (!net::doors::makeKey(1,d.placement,key)) return false;
    out=d; return true;
}
uintptr_t initialized(void* self) {
    const auto result=g_init(self);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lifecycle.changed();
    Driver d;
    if (g_drivers.size()<kMaxDrivers&&snapshot(reinterpret_cast<uintptr_t>(self),d)) g_drivers[d.ant]=d;
    return result;
}
uintptr_t destroyed(void* self,uint32_t flags) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lifecycle.beginDestroy();
        const auto address=reinterpret_cast<uintptr_t>(self);
        for (auto it=g_drivers.begin();it!=g_drivers.end();) {
            if (it->second.address==address) it=g_drivers.erase(it); else ++it;
        }
    }
    const auto result=g_destroy(self,flags);
    {std::lock_guard<std::mutex> lock(g_mutex);g_lifecycle.endDestroy();}
    return result;
}
bool current(const Driver& d,uintptr_t actor) {
    if (q(d.address)!=mem::rva(kDriver)||q(d.address+0x28)!=d.data||q(d.address+0x38)!=d.ant+0x60||
        q(d.address+0x40)!=d.mesh||q(d.ant)!=mem::rva(kAnt)) return false;
    const auto parent=q(d.ant+0x158);
    return parent&&q(parent+0x10)+0x40==actor;
}
void readClosed(void* definition,void* bindings,int* assetIndex,int* propertyIndex,void* actorPtr) {
    int index=-1;
    const auto closed=g_closed.load();
    // These are the original callback's live arguments. Inspect before forwarding:
    // it advances assetIndex. Avoid a kernel read for every unrelated ANT bool.
    if (g_bankReady.load()&&closed) index=*assetIndex;
    const bool relevant=index>=0&&index<4096&&
        (*reinterpret_cast<uintptr_t**>(bindings))[index]==closed;
    g_read(definition,bindings,assetIndex,propertyIndex,actorPtr);
    if (!relevant||!g_installed.load()) return;
    const auto now=GetTickCount64();const auto world=g_world.load();
    if (!world||now<g_readyAt.load()) return;
    const auto reader=reinterpret_cast<uintptr_t>(bindings)-0x30;
    if (q(reader)!=mem::rva(kReader)) return;
    const auto holder=q(reader+0x88),antIface=q(holder);
    if (antIface<8) return;
    const auto ant=antIface-8,actor=reinterpret_cast<uintptr_t>(actorPtr);
    std::unique_lock<std::mutex> lock(g_mutex,std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (world!=g_world.load()||now<g_readyAt.load()) return;
    const auto found=g_drivers.find(ant);
    if (found==g_drivers.end()) return;
    Driver& d=found->second;
    if (now-d.sampled<50||!current(d,actor)) return;
    if (q(mem::rva(kManager))!=g_manager.load()||q(g_table.load()+8*(g_closedId&0x7ffff))!=closed-0x20||
        q(g_table.load()+8*(g_pulseId&0x7ffff))!=g_pulse.load()-0x20||q(closed)!=mem::rva(kBool)||
        q(g_pulse.load())!=mem::rva(kBool)) return;
    d.sampled=now;
    if (d.key.world!=world) {
        if (!net::doors::makeKey(world,d.placement,d.key)) return;
        d.observation.reset(); d.pending=false; d.applied=0;
    }
    const bool enabled=config::get()->syncDoors;
    d.closed=reinterpret_cast<GetFn>(mem::rva(kGet))(closed,actor)!=0;
    if (d.applied&&now-d.applied>3000) { d.observation.suppress=false; d.observation.publishApplied=false; }
    const bool opened=d.observation.observe(d.closed,enabled);
    const bool manual=now<=g_commandUntil&&g_command==d.key;
    const bool remote=d.closed&&enabled&&net::doorIsOpen(d.key);
    if (opened||d.pending||manual||remote) {
        // Reject ambiguous placement before publishing as well as before applying.
        for (const auto& entry:g_drivers) {
            Key other;
            if (entry.first!=ant&&net::doors::makeKey(world,entry.second.placement,other)&&other==d.key) {
                d.pending=false;return;
            }
        }
    }
    if (opened) d.pending=true;
    if (!enabled) d.pending=false;
    if (d.pending&&net::publishDoorOpened(d.key)) d.pending=false;
    if (!d.closed) return;
    if ((!manual&&!remote)||(d.applied&&now-d.applied<3000)) return;
    const auto pulse=g_pulse.load();
    if (reinterpret_cast<GetFn>(mem::rva(kGet))(pulse,actor)!=0) return;
    d.observation.applied(manual&&enabled);
    d.applied=now;
    if (manual) g_commandUntil=0;
    reinterpret_cast<SetFn>(mem::rva(kSet))(pulse,actor,1);
}
bool patch(uintptr_t slot,uintptr_t expected,uintptr_t replacement) {
    const auto address=mem::rva(slot);DWORD old=0;
    if (!VirtualProtect(reinterpret_cast<void*>(address),8,PAGE_READWRITE,&old)) return false;
    const auto previous=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(address),
        reinterpret_cast<void*>(replacement),reinterpret_cast<void*>(expected));
    DWORD ignored=0;VirtualProtect(reinterpret_cast<void*>(address),8,old,&ignored);
    return reinterpret_cast<uintptr_t>(previous)==expected;
}
bool code(uintptr_t va,const std::array<uint8_t,16>& expected) {
    std::array<uint8_t,16> bytes{};
    return mem::read(mem::rva(va),bytes.data(),bytes.size())&&bytes==expected;
}
void bank() {
    const auto manager=q(mem::rva(kManager));
    const auto count=mem::readOr<uint32_t>(manager+0x80),exports=mem::readOr<uint32_t>(manager+0xa0);
    if ((count!=33340&&count!=33341)||exports!=1357) {g_bankReady=false;return;}
    const auto table=q(manager+0xa8),map=q(manager+0xd0);
    std::vector<uint8_t> bytes(exports*20);
    if (!table||!mem::read(map,bytes.data(),bytes.size())) {g_bankReady=false;return;}
    uint64_t hash=14695981039346656037ULL;
    for (auto byte:bytes) hash=(hash^byte)*1099511628211ULL;
    uint32_t closedId=0,pulseId=0;
    if (count==33340&&hash==0xd70756f58c02cb00ULL) { closedId=0x85f3c;pulseId=0x86256; }
    else if (count==33341&&hash==0xe20f1f8ac2e10d23ULL) { closedId=0x85f77;pulseId=0x86292; }
    else {g_bankReady=false;return;}
    const auto closed=q(table+8*(closedId&0x7ffff))+0x20,pulse=q(table+8*(pulseId&0x7ffff))+0x20;
    if (q(closed)!=mem::rva(kBool)||q(pulse)!=mem::rva(kBool)) {g_bankReady=false;return;}
    std::lock_guard<std::mutex> lock(g_mutex);
    g_closedId=closedId;g_pulseId=pulseId;
    g_manager=manager;g_table=table;g_closed=closed;g_pulse=pulse;g_bankReady=true;
}
// One incremental late-injection pass. Keep the cursor between calls so a time
// slice or a lifecycle conflict cannot silently abandon the remaining heap.
std::atomic<int> g_scan{0}; // 0 waiting, 1 scanning, 2 complete, 3 limited
bool bootstrap() {
    static uintptr_t cursor=0x10000;
    static uint64_t started=0,bytesRead=0;
    static unsigned added=0,unreadable=0;
    static std::vector<uint8_t> buffer(1024*1024);
    if (!started) {started=GetTickCount64();g_scan=1;}
    const auto slice=GetTickCount64();
    SYSTEM_INFO system{};GetSystemInfo(&system);
    while (cursor<reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress)) {
        if (GetTickCount64()-started>120000||bytesRead>32ULL*1024*1024*1024) {
            g_scan=3;MLOG("doors: bootstrap limited after %u drivers; some loaded doors may need a level reload",added);return true;
        }
        if (GetTickCount64()-slice>=40) return false;
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor),&page,sizeof(page))||!page.RegionSize) {
            g_scan=3;MLOG("doors: bootstrap stopped at an unreadable memory map");return true;
        }
        const auto end=reinterpret_cast<uintptr_t>(page.BaseAddress)+page.RegionSize;
        if (end<=cursor) {g_scan=3;return true;}
        const bool readable=page.State==MEM_COMMIT&&page.Type==MEM_PRIVATE&&
            !(page.Protect&(PAGE_GUARD|PAGE_NOACCESS))&&(page.Protect&(PAGE_READWRITE|PAGE_WRITECOPY));
        if (!readable) {cursor=end;continue;}
        const size_t size=static_cast<size_t>(std::min<uintptr_t>(buffer.size(),end-cursor));
        uint64_t ticket;
        { std::lock_guard<std::mutex> lock(g_mutex);
          if (g_lifecycle.retiring) return false;
          ticket=g_lifecycle.ticket; }
        bool retry=false;
        if (mem::read(cursor,buffer.data(),size)) {
            for (size_t i=0;i+8<=size;i+=8) {
                uintptr_t vt=0;memcpy(&vt,buffer.data()+i,8);
                if (vt!=mem::rva(kDriver)) continue;
                Driver d;if (!snapshot(cursor+i,d)) continue;
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_lifecycle.accepts(ticket)) {retry=true;break;}
                if (g_drivers.size()<kMaxDrivers&&g_drivers.emplace(d.ant,d).second) ++added;
            }
        } else ++unreadable;
        bytesRead+=size;
        if (!retry) cursor+=size;
    }
    g_scan=2;
    MLOG("doors: bootstrap finished, %u drivers, %u unreadable chunks, %llu ms",added,unreadable,
         static_cast<unsigned long long>(GetTickCount64()-started));
    return true;
}
} // namespace

bool install() {
    if (g_installed) return true;
    if (!code(kRead,{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x41,0x8b,0x08,0x48,0x89,0xd7})||
        !code(kInit,{0x48,0x89,0xe0,0x57,0x48,0x81,0xec,0xa0,0,0,0,0x48,0xc7,0x40,0x88,0xfe})||
        !code(kDestroy,{0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe})||
        !code(kGet,{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x0f,0xb7,0x41,0x28,0x48,0x89})||
        !code(kSet,{0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xd0,0x48,0x8d,0x51,0x18,0x41,0x0f,0xb6,0xd8})||
        q(mem::rva(kInitSlot))!=mem::rva(kInit)||q(mem::rva(kDestroySlot))!=mem::rva(kDestroy)||
        q(mem::rva(kReadSlot))!=mem::rva(kRead)) {
        MLOG("doors: unsupported code or an existing hook; door controls are off"); return false;
    }
    g_init=reinterpret_cast<InitFn>(mem::rva(kInit));g_destroy=reinterpret_cast<DestroyFn>(mem::rva(kDestroy));
    g_read=reinterpret_cast<ReadFn>(mem::rva(kRead));
    if (!patch(kDestroySlot,mem::rva(kDestroy),reinterpret_cast<uintptr_t>(&destroyed))) return false;
    if (!patch(kInitSlot,mem::rva(kInit),reinterpret_cast<uintptr_t>(&initialized))) {
        patch(kDestroySlot,reinterpret_cast<uintptr_t>(&destroyed),mem::rva(kDestroy));return false;
    }
    if (!patch(kReadSlot,mem::rva(kRead),reinterpret_cast<uintptr_t>(&readClosed))) {
        patch(kInitSlot,reinterpret_cast<uintptr_t>(&initialized),mem::rva(kInit));
        patch(kDestroySlot,reinterpret_cast<uintptr_t>(&destroyed),mem::rva(kDestroy));return false;
    }
    g_installed=true;MLOG("doors: lifecycle and animation callbacks installed");return true;
}

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_world=0;g_readyAt=GetTickCount64()+2000;g_targetAt=0;g_commandUntil=0;g_request=false;
    g_lifecycle.changed();
    for (auto& entry:g_drivers) { auto& d=entry.second;d.key={};d.pending=false;d.observation.reset();d.applied=0; }
}

void tick() {
    if (!g_installed) return;
    const auto now=GetTickCount64();
    const auto world=net::doors::worldId(level::levelName());
    g_world=world;
    const bool sync=config::get()->syncDoors;
    static bool previousSync=sync;
    if (sync!=previousSync) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry:g_drivers) {entry.second.pending=false;entry.second.observation.reset();}
        previousSync=sync;
    }
    CameraView camera{};
    if (!world||level::inFrontEnd()||!camera::read(camera)) { g_targetAt=0;g_request=false;return; }
    net::publishDoorInterest(world,camera.pos,sync&&g_bankReady&&now>=g_readyAt.load());
    static uint64_t checked=0;
    if (now-checked<100&&!g_request.load()) return;
    checked=now;
    const bool requested=g_request.exchange(false);
    float to[3];for (int i=0;i<3;++i) to[i]=camera.pos[i]-3.f*camera.forward[i];
    raycast::Hit hit{};
    if (!g_bankReady||now<g_readyAt.load()||!raycast::cast(camera.pos,to,hit)) {g_targetAt=0;return;}
    std::unique_lock<std::mutex> lock(g_mutex,std::try_to_lock);
    if (!lock.owns_lock()) {g_targetAt=0;return;}
    const Driver* candidate=nullptr;
    for (const auto& entry:g_drivers) {
        const auto& d=entry.second;
        if (!d.closed||!d.observation.seen||d.key.world!=world||now-d.sampled>500||
            !door_policy::matchesHit(d.placement,camera.pos,hit.pos,hit.normal)) continue;
        if (candidate) {g_targetAt=0;return;}
        candidate=&d;
    }
    if (!candidate) {g_targetAt=0;return;}
    g_target=candidate->key;g_targetAt=now;
    if (requested) {g_command=g_target;g_commandUntil=now+300;}
}

void maintain() {
    if (!g_installed) return;
    static uint64_t checked=0;static bool scanned=false;
    const auto now=GetTickCount64();
    if (now-checked>=1000) {checked=now;bank();}
    if (!scanned&&g_world.load()&&now>=g_readyAt.load()&&g_bankReady) scanned=bootstrap();
}
bool canOpen() {const auto at=g_targetAt.load();return at&&GetTickCount64()-at<250;}
void requestOpen() {if (canOpen()) g_request=true;}
std::string status() {
    if (!g_installed) return "Door controls are unavailable for this game build.";
    if (!g_bankReady) return "Waiting for a supported animation bank.";
    if (g_scan==3) return "Door discovery was incomplete. Reload the level to register remaining doors.";
    if (g_scan==1) return "Registering doors already loaded in this level.";
    if (!config::get()->syncDoors) return "Local only. Other players cannot change your doors.";
    return net::doorSyncAvailable()?"Syncing nearby doors.":"Local controls ready. Waiting for door sync from the host.";
}
} // namespace mecmp::game::doors
