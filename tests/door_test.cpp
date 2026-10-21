#include "net/doors.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>

using namespace mecmp::net::doors;
static int failures=0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED %s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while(0)

static Key door(float x=0, uint64_t world=1) {
    const float transform[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,0,0,1};
    Key key;CHECK(makeKey(world,transform,key));return key;
}
static Message interest(uint32_t generation=1,float x=0,bool enabled=true,uint64_t world=1) {
    Message m;m.op=Op::Interest;m.id=generation;m.revision=1;m.key.world=world;m.position[0]=x;m.enabled=enabled;return m;
}
static Message opened(Key key,uint32_t id=1) {
    Message m;m.op=Op::Open;m.id=id;m.key=key;return m;
}
static Message ack(const Message& in) {
    Message m;m.op=Op::Ack;m.ack=in.op;m.id=in.id;m.revision=in.revision;m.key=in.key;return m;
}
static void identityAndCodec() {
    Key key=door(20);
    CHECK(worldId("Levels/SP/SP_MainCity")==worldId("levels/sp/sp_maincity"));
    CHECK(worldId("city")!=worldId("menu"));
    float transform[16]={1,0,0,0,0,1,0,0,0,0,1,0,20.00001f,0,0,1};
    Key copy;CHECK(makeKey(1,transform,copy));CHECK(copy==key);
    transform[12]=std::numeric_limits<float>::quiet_NaN();CHECK(!makeKey(1,transform,copy));
    transform[12]=0;transform[0]=100;CHECK(!makeKey(1,transform,copy));
    CHECK(!makeKey(0,transform,copy));
    Message source=opened(key,7),decoded;char buf[kPayloadSize];
    CHECK(encode(source,buf,sizeof(buf))==kPayloadSize);
    CHECK(decode(buf,sizeof(buf),decoded));CHECK(decoded.key==key);CHECK(decoded.id==7);
    CHECK(!decode(buf,sizeof(buf)-1,decoded));
    buf[0]=99;CHECK(!decode(buf,sizeof(buf),decoded));
    encode(source,buf,sizeof(buf));buf[2]=64;CHECK(!decode(buf,sizeof(buf),decoded));
    source.id=0;CHECK(encode(source,buf,sizeof(buf))==0);
    source=interest();source.position[1]=std::numeric_limits<float>::infinity();CHECK(encode(source,buf,sizeof(buf))==0);
}
static void authorityAndHysteresis() {
    Authority a;Key key=door();
    a.receive(1,interest(),0);a.receive(2,interest(1,199),0);a.tick(0);
    a.receive(1,opened(key),1);
    auto messages=a.tick(100);Message state;bool found=false;
    for (const auto& out:messages) if(out.peer==2&&out.message.op==Op::State) {state=out.message;found=true;}
    CHECK(found);CHECK(state.revision!=0);CHECK(a.size()==1);
    a.receive(2,ack(state),101);
    bool unacknowledgedRetried=false;
    for(const auto& out:a.tick(400)) {
        CHECK(out.peer!=2);
        if(out.peer==1&&out.message.op==Op::State)unacknowledgedRetried=true;
    }
    CHECK(unacknowledgedRetried);
    a.receive(2,interest(1,220),500);a.tick(500);
    a.receive(2,interest(2,220),600);
    bool outside=false;for(const auto& out:a.tick(600))if(out.peer==2&&out.message.op==Op::State)outside=true;
    CHECK(!outside);
    Message moved=interest(2,199);moved.revision=2;a.receive(2,moved,700);
    found=false;for(const auto& out:a.tick(700))if(out.peer==2&&out.message.op==Op::State)found=true;
    CHECK(found);
    a.remove(2);CHECK(a.peers()==1);
    a.reset();CHECK(a.size()==0);CHECK(a.peers()==0);
}
static void lostReportAndLateArrival() {
    Authority a;Client c,d;const auto key=door();const float pos[3]={0,0,0};
    c.interest(1,pos,true,0);CHECK(c.opened(key,0));
    for(const auto& m:c.tick(0))if(m.op==Op::Open)a.receive(1,m,0);
    CHECK(a.size()==0);
    for(const auto& m:c.tick(1000))a.receive(1,m,1000);
    for(const auto& out:a.tick(1000))if(out.peer==1)c.receive(out.message,1000);
    for(const auto& m:c.tick(1250))a.receive(1,m,1250);
    for(const auto& out:a.tick(1250))if(out.peer==1)c.receive(out.message,1250);
    CHECK(a.size()==1);CHECK(c.isOpen(key));CHECK(c.available(1250));
    d.interest(1,pos,true,2000);
    for(const auto& m:d.tick(2000))a.receive(2,m,2000);
    auto batch=a.tick(2000);
    for(const auto& out:batch)if(out.peer==2&&out.message.op==Op::State) {d.receive(out.message,2000);d.receive(out.message,2000);}
    CHECK(d.isOpen(key));
    d.tick(2050);CHECK(d.isOpen(key)); // state remains available for an object that loads later
    const float far[3]={1000,0,0};d.interest(1,far,false,2100);CHECK(!d.isOpen(key));
    CHECK(!d.opened(key,2100));
    d.interest(1,pos,true,3000);
    for(const auto& m:d.tick(3000))a.receive(2,m,3000);
    for(const auto& out:a.tick(3000))if(out.peer==2)d.receive(out.message,3000);
    CHECK(d.isOpen(key));
    d.interest(2,pos,true,4000);CHECK(!d.isOpen(key));
}
static void repeatsLimitsAndWrap() {
    Authority a;Key key=door();a.receive(1,interest(),0);a.tick(0);
    a.receive(1,opened(key,10),1);a.receive(1,opened(key,10),1);a.receive(1,opened(key,11),1);
    CHECK(a.size()==1);
    a.receive(3,interest(1,1000),2);a.tick(100);
    bool remote=false;for(const auto& out:a.tick(400))if(out.peer==3&&out.message.op==Op::State)remote=true;
    CHECK(!remote);
    a.receive(3,opened(key,100),500);CHECK(a.size()==1);
    a.receive(3,opened(door(1000,2),101),500);CHECK(a.size()==1);
    Client c;const float p[3]={0,0,0};c.interest(1,p,true,0xffffff00u);CHECK(c.opened(key,0xffffff00u));
    c.tick(0xffffff00u);bool retried=false;for(const auto& m:c.tick(0x100u))if(m.op==Op::Open)retried=true;
    CHECK(retried);
    Client legacy;legacy.interest(1,p,true,0);legacy.tick(0);CHECK(!legacy.available(10000));
    Client limited;limited.interest(1,p,true,0);
    for(unsigned i=0;i<kMaxReports;++i)CHECK(limited.opened(door(static_cast<float>(i)),0));
    CHECK(!limited.opened(door(1000),0));
    CHECK(limited.tick(0).size()<=kSendBudget);
    limited.tick(11000);CHECK(limited.failedReports()!=0);
}
static void reorderedInterestDoesNotReenable() {
    Authority a;const auto key=door();
    a.receive(1,interest(1),0);a.tick(0);
    a.receive(1,interest(2,0,false),10);a.tick(100);
    a.receive(1,interest(1),20);a.receive(1,opened(key),21);
    CHECK(a.size()==0);
    a.receive(1,interest(3),30);a.receive(1,opened(key,2),31);CHECK(a.size()==1);
    a.tick(200);a.receive(2,interest(1,1000),201);a.tick(300);
    Message nearby=interest(1);nearby.revision=2;
    a.receive(2,nearby,301);a.tick(400);
    a.receive(2,interest(1,1000),401);
    bool retry=false;
    for(const auto& out:a.tick(700))if(out.peer==2&&out.message.op==Op::State)retry=true;
    CHECK(retry);
}
static void moreDoorsThanPendingSlots() {
    Authority a;Client c;const float point[3]={0,0,0};
    a.receive(1,interest(),0);std::vector<Key> keys;
    for(unsigned i=0;i<kMaxPeerDoors+1;++i) {
        auto key=door((i%64)*.1f);key.placement[11]=static_cast<int32_t>(i/64)*10;
        keys.push_back(key);a.receive(1,opened(key,i+10),0);
    }
    CHECK(a.size()==keys.size());c.interest(1,point,true,0);
    for(uint32_t now=0;now<60000;now+=100) {
        for(const auto& m:c.tick(now))a.receive(2,m,now);
        for(const auto& out:a.tick(now))if(out.peer==2)c.receive(out.message,now);
    }
    for(const auto& key:keys)CHECK(c.isOpen(key));
    c.interest(1,point,false,60000);
    for(const auto& m:c.tick(60000))a.receive(2,m,60000);
    for(const auto& out:a.tick(60000))if(out.peer==2)c.receive(out.message,60000);
    CHECK(c.tick(62000).empty());
}
static void pendingStateKeepsLeaveRadius() {
    Authority a;const auto key=door();a.receive(1,interest(),0);a.receive(1,opened(key),0);a.tick(0);
    a.receive(2,interest(1,199),100);a.tick(100);
    auto moved=interest(1,220);moved.revision=2;a.receive(2,moved,110);a.tick(200);
    bool kept=false;for(const auto& out:a.tick(400))if(out.peer==2&&out.message.op==Op::State)kept=true;
    CHECK(kept);
    moved.position[0]=251;moved.revision=3;a.receive(2,moved,410);a.tick(500);
    for(const auto& out:a.tick(800))CHECK(out.peer!=2);
    moved.position[0]=220;moved.revision=4;a.receive(2,moved,810);
    for(const auto& out:a.tick(900))if(out.peer==2)CHECK(out.message.op!=Op::State);
    moved.position[0]=199;moved.revision=5;a.receive(2,moved,910);
    bool entered=false;for(const auto& out:a.tick(1000))if(out.peer==2&&out.message.op==Op::State)entered=true;
    CHECK(entered);
}
int main() {
    identityAndCodec();authorityAndHysteresis();lostReportAndLateArrival();repeatsLimitsAndWrap();reorderedInterestDoesNotReenable();moreDoorsThanPendingSlots();pendingStateKeepsLeaveRadius();
    std::printf("door tests: %d failure(s)\n",failures);return failures?1:0;
}
