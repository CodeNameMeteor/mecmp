#include "net/doors.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mecmp::net::doors {
namespace {
uint32_t next(uint32_t& id) { if(!++id)++id;return id; }
bool sane(const std::array<float,3>& pos) {
    for(float x:pos)if(!std::isfinite(x)||std::fabs(x)>100000.f)return false;
    return true;
}
float distance2(const std::array<float,3>& a,const std::array<float,3>& b) {
    float sum=0;for(size_t i=0;i<3;++i){float d=a[i]-b[i];sum+=d*d;}return sum;
}
bool newer(uint32_t a,uint32_t b) { const auto d=a-b;return d&&d<0x80000000u; }
bool valid(const Message& m) {
    if(!m.id||!m.key.world||!sane(m.position)||static_cast<unsigned>(m.op)>3||static_cast<unsigned>(m.ack)>2)return false;
    const bool zeroPlacement=m.key.placement==std::array<int32_t,12>{};
    const bool zeroPosition=m.position==std::array<float,3>{};
    if(m.op==Op::Interest)return m.ack==Op::Interest&&!m.rejected&&m.revision&&zeroPlacement;
    if(m.op==Op::Ack&&m.ack==Op::Interest)return !m.enabled&&!m.rejected&&m.revision&&zeroPlacement&&zeroPosition;
    if(!validKey(m.key)||!zeroPosition||m.enabled)return false;
    if(m.op==Op::Open)return m.ack==Op::Interest&&!m.rejected&&!m.revision;
    if(m.op==Op::State)return m.ack==Op::Interest&&!m.rejected&&m.revision;
    return m.ack!=Op::Interest&&(!m.rejected?m.revision!=0:m.ack==Op::Open&&m.revision==0);
}
Message acknowledgment(const Message& m) {
    Message result;result.op=Op::Ack;result.ack=m.op;result.id=m.id;result.revision=m.revision;result.key=m.key;return result;
}
void queue(std::deque<Message>& q,const Message& m) {
    for(const auto& old:q)if(old.id==m.id&&old.key==m.key&&old.ack==m.ack)return;
    if(q.size()<kMaxReports)q.push_back(m);
}
void refill(uint32_t now,uint32_t& at,unsigned& budget,bool& have) {
    if(!have||now-at>=kSendWindowMs){have=true;at=now;budget=kSendBudget;}
}
}

uint64_t worldId(std::string_view name) {
    if(name.empty())return 0;
    uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:name){if(c>='A'&&c<='Z')c+=32;if(c=='\\')c='/';hash^=c;hash*=1099511628211ULL;}
    return hash?hash:1;
}
std::array<float,3> position(const Key& key) {
    return {key.placement[9]/100.f,key.placement[10]/100.f,key.placement[11]/100.f};
}
bool validKey(const Key& key) {
    if(!key.world||!sane(position(key)))return false;
    for(int row=0;row<3;++row) {
        double len=0;
        for(int col=0;col<3;++col){const double x=key.placement[row*3+col]/10000.0;len+=x*x;}
        if(std::fabs(len-1.0)>.02)return false;
        for(int other=0;other<row;++other) {
            double dot=0;for(int col=0;col<3;++col)dot+=(key.placement[row*3+col]/10000.0)*(key.placement[other*3+col]/10000.0);
            if(std::fabs(dot)>.02)return false;
        }
    }
    return true;
}
bool makeKey(uint64_t world,const float transform[16],Key& out) {
    Key key;key.world=world;
    for(int row=0;row<4;++row)for(int col=0;col<3;++col) {
        const double value=transform[row*4+col];
        if(!std::isfinite(value)||std::fabs(value)>(row==3?100000.0:2.0))return false;
        key.placement[row*3+col]=static_cast<int32_t>(std::llround(value*(row==3?100.0:10000.0)));
    }
    if(!validKey(key))return false;
    out=key;return true;
}
size_t encode(const Message& m,char* payload,size_t capacity) {
    if(!payload||capacity<kPayloadSize||!valid(m))return 0;
    auto* bytes=reinterpret_cast<unsigned char*>(payload);
    bytes[0]=1;bytes[1]=static_cast<uint8_t>(m.op);bytes[2]=(m.enabled?1:0)|(m.rejected?2:0);bytes[3]=static_cast<uint8_t>(m.ack);
    size_t at=4;
    auto put=[&](uint64_t value,unsigned n){for(unsigned i=0;i<n;++i)bytes[at++]=static_cast<unsigned char>(value>>(8*i));};
    put(m.id,4);put(m.revision,4);put(m.key.world,8);
    for(auto value:m.key.placement)put(static_cast<uint32_t>(value),4);
    for(float value:m.position){uint32_t bits;std::memcpy(&bits,&value,4);put(bits,4);}
    return at;
}
bool decode(const char* payload,size_t size,Message& out) {
    if(!payload||size!=kPayloadSize)return false;
    const auto* bytes=reinterpret_cast<const unsigned char*>(payload);
    if(bytes[0]!=1||(bytes[2]&~3u))return false;
    Message m;m.op=static_cast<Op>(bytes[1]);m.enabled=bytes[2]&1;m.rejected=bytes[2]&2;m.ack=static_cast<Op>(bytes[3]);
    size_t at=4;
    auto get=[&](unsigned n){uint64_t value=0;for(unsigned i=0;i<n;++i)value|=uint64_t(bytes[at++])<<(8*i);return value;};
    m.id=static_cast<uint32_t>(get(4));m.revision=static_cast<uint32_t>(get(4));m.key.world=get(8);
    for(auto& value:m.key.placement){const auto bits=static_cast<uint32_t>(get(4));std::memcpy(&value,&bits,4);}
    for(auto& value:m.position){const auto bits=static_cast<uint32_t>(get(4));std::memcpy(&value,&bits,4);}
    if(!valid(m))return false;
    out=m;return true;
}
void Authority::refresh(Peer& peer) {
    for(auto it=peer.watched.begin();it!=peer.watched.end();) {
        if(!peer.interest.enabled||it->first.world!=peer.interest.key.world||distance2(position(it->first),peer.interest.position)>kLeaveRadius*kLeaveRadius)it=peer.watched.erase(it);
        else {if(it->second.exhausted){it->second.exhausted=false;it->second.sent=false;it->second.message.id=next(nextId_);}++it;}
    }
    if(!peer.interest.enabled)return;
    for(const auto& item:doors_) {
        if(peer.watched.size()>=kMaxPeerDoors)break;
        if(item.first.world!=peer.interest.key.world||peer.watched.count(item.first)||distance2(position(item.first),peer.interest.position)>kEnterRadius*kEnterRadius)continue;
        Pending pending;pending.message.op=Op::State;pending.message.key=item.first;pending.message.id=next(nextId_);pending.message.revision=item.second;
        peer.watched.emplace(item.first,pending);
    }
}
void Authority::receive(uint32_t id,const Message& message,uint32_t now) {
    if(!id||!valid(message))return;
    if(message.op==Op::Interest) {
        if(!peers_.count(id)&&peers_.size()>=64)return;
        auto& peer=peers_[id];
        if(peer.interest.id) {
            if(message.id!=peer.interest.id&&!newer(message.id,peer.interest.id))return;
            if(message.id==peer.interest.id) {
                if(message.key.world!=peer.interest.key.world||message.enabled!=peer.interest.enabled)return;
                if(!newer(message.revision,peer.interest.revision)) {
                    if(message.revision==peer.interest.revision)queue(peer.control,acknowledgment(message));
                    return;
                }
            }
        }
        if(peer.interest.id!=message.id||peer.interest.key.world!=message.key.world||peer.interest.enabled!=message.enabled){peer.watched.clear();peer.control.clear();}
        peer.interest=message;peer.heard=now;refresh(peer);queue(peer.control,acknowledgment(message));return;
    }
    const auto found=peers_.find(id);if(found==peers_.end())return;
    auto& peer=found->second;
    if(!peer.interest.enabled||now-peer.heard>kPeerTimeoutMs||message.key.world!=peer.interest.key.world)return;
    if(message.op==Op::Ack&&message.ack==Op::State) {
        auto state=peer.watched.find(message.key);
        if(!message.rejected&&state!=peer.watched.end()&&state->second.message.id==message.id&&state->second.message.revision==message.revision)state->second.acked=true;
        return;
    }
    if(message.op!=Op::Open||distance2(position(message.key),peer.interest.position)>144.f)return;
    auto entry=doors_.find(message.key);Message reply=acknowledgment(message);
    if(entry==doors_.end()) {
        if(doors_.size()>=kMaxDoors){reply.rejected=true;queue(peer.control,reply);return;}
        entry=doors_.emplace(message.key,next(nextRevision_)).first;
        for(auto& item:peers_)refresh(item.second);
    }
    reply.revision=entry->second;queue(peer.control,reply);
}
std::vector<Delivery> Authority::tick(uint32_t now) {
    std::vector<Delivery> result;
    for(auto& item:peers_) {
        auto& p=item.second;refill(now,p.budgetAt,p.budget,p.haveBudget);
        if(now-p.heard>kPeerTimeoutMs){p.watched.clear();p.control.clear();continue;}
        while(p.budget&&!p.control.empty()){result.push_back({item.first,p.control.front()});p.control.pop_front();--p.budget;}
        for(auto& state:p.watched) {
            auto& pending=state.second;
            if(!p.budget)break;
            if(pending.acked||pending.exhausted)continue;
            if(pending.sent&&now-pending.first>kRetryWindowMs){pending.exhausted=true;continue;}
            if(pending.sent&&now-pending.last<kRetryMs)continue;
            if(!pending.sent)pending.first=now;
            pending.sent=true;pending.last=now;--p.budget;result.push_back({item.first,pending.message});
        }
    }
    return result;
}
void Authority::reset() {doors_.clear();peers_.clear();nextId_=nextRevision_=0;}

void Client::interest(uint64_t world,const float pos[3],bool enabled,uint32_t) {
    const std::array<float,3> point={pos[0],pos[1],pos[2]};
    if(!sane(point)||!world)enabled=false;
    if(!haveInterest_||interest_.key.world!=world||interest_.enabled!=enabled) {
        states_.clear();reports_.clear();acknowledgments_.clear();haveReply_=false;
        interest_.id=next(nextId_);interestDue_=true;
        interest_.revision=0;
    }
    haveInterest_=true;interest_.op=Op::Interest;interest_.key.world=world;interest_.position=point;interest_.enabled=enabled;
}
bool Client::opened(const Key& key,uint32_t now) {
    if(!interest_.enabled||key.world!=interest_.key.world||!validKey(key)||reports_.size()>=kMaxReports)return false;
    if(states_.count(key)||reports_.count(key))return true;
    Report report;report.message.op=Op::Open;report.message.id=next(nextId_);report.message.key=key;report.created=now;
    reports_.emplace(key,report);return true;
}
void Client::receive(const Message& message,uint32_t now) {
    if(!valid(message)||!interest_.enabled||message.key.world!=interest_.key.world)return;
    if(message.op==Op::Ack&&message.ack==Op::Interest&&message.id==interest_.id){haveReply_=true;lastReply_=now;return;}
    if(message.op==Op::Ack&&message.ack==Op::Open) {
        const auto pending=reports_.find(message.key);
        if(pending==reports_.end()||pending->second.message.id!=message.id)return;
        reports_.erase(pending);
        if(message.rejected){++failed_;return;}
    } else if(message.op==Op::State) {
        const auto old=states_.find(message.key);
        if(old==states_.end()&&states_.size()>=kMaxDoors)return;
        queue(acknowledgments_,acknowledgment(message));
    } else return;
    auto found=states_.find(message.key);
    if(found==states_.end()) {if(states_.size()<kMaxDoors)states_.emplace(message.key,message.revision);}
    else if(newer(message.revision,found->second))found->second=message.revision;
}
std::vector<Message> Client::tick(uint32_t now) {
    std::vector<Message> result;refill(now,budgetAt_,budget_,haveBudget_);
    for(auto it=reports_.begin();it!=reports_.end();) {
        if(now-it->second.created>kRetryWindowMs){++failed_;it=reports_.erase(it);}else ++it;
    }
    if(haveInterest_&&interest_.key.world&&budget_&&(interestDue_||now-lastInterest_>=kInterestMs)) {
        next(interest_.revision);result.push_back(interest_);lastInterest_=now;interestDue_=false;--budget_;
    }
    while(budget_&&!acknowledgments_.empty()){result.push_back(acknowledgments_.front());acknowledgments_.pop_front();--budget_;}
    for(auto& entry:reports_) {
        auto& report=entry.second;if(!budget_)break;
        if(report.sent&&now-report.last<kRetryMs)continue;
        report.sent=true;report.last=now;--budget_;result.push_back(report.message);
    }
    return result;
}
void Client::reset() {*this=Client{};}

} // namespace mecmp::net::doors
