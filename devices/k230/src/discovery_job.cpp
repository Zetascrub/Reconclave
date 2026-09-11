#include "discovery_job.h"

#include <arpa/inet.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "network_tools.h"

namespace reconclave { namespace {
constexpr unsigned kWorkers = 8;
constexpr int kConnectTimeoutMs = 120;
constexpr std::array<std::uint16_t, 8> kPorts = {22, 53, 80, 443, 445, 554, 3389, 8080};

std::string addressText(std::uint32_t host_order) {
  in_addr address{htonl(host_order)}; char text[INET_ADDRSTRLEN]{};
  return inet_ntop(AF_INET,&address,text,sizeof(text)) ? text : "";
}
std::string makeJobId() {
  timespec now{}; clock_gettime(CLOCK_MONOTONIC,&now);
  return "discovery-"+std::to_string(now.tv_sec)+"-"+std::to_string(now.tv_nsec);
}
}

bool boundedIpv4Range(const std::string& network,std::uint32_t&first,std::uint32_t&last,std::string&error) {
  const auto slash=network.find('/'); if(slash==std::string::npos){error="CIDR prefix is required";return false;}
  char*end=nullptr;const long prefix=std::strtol(network.c_str()+slash+1,&end,10);
  if(end==network.c_str()+slash+1||*end!='\0'||prefix<24||prefix>32){error="network must be IPv4 /24 or smaller";return false;}
  in_addr parsed{};if(inet_pton(AF_INET,network.substr(0,slash).c_str(),&parsed)!=1){error="invalid IPv4 network";return false;}
  const std::uint32_t mask=prefix==0?0:0xffffffffU<<(32-prefix),base=ntohl(parsed.s_addr)&mask,broadcast=base|~mask;
  first=base;last=broadcast;if(prefix<=30){++first;--last;}error.clear();return true;
}

DiscoveryJob::~DiscoveryJob(){cancel();if(thread_.joinable())thread_.join();}
bool DiscoveryJob::start(const std::string&network,std::string&error){
  std::uint32_t first=0,last=0;if(!boundedIpv4Range(network,first,last,error))return false;
  if(thread_.joinable()){DiscoverySnapshot current=snapshot();if(current.status=="running"){error="a discovery job is already running";return false;}thread_.join();}
  cancel_=false;{std::lock_guard<std::mutex>lock(mutex_);state_={};state_.job_id=makeJobId();state_.status="running";state_.total=last-first+1;}
  thread_=std::thread(&DiscoveryJob::run,this,first,last);return true;
}
void DiscoveryJob::cancel(){cancel_=true;}
DiscoverySnapshot DiscoveryJob::snapshot()const{std::lock_guard<std::mutex>lock(mutex_);return state_;}
void DiscoveryJob::run(std::uint32_t first,std::uint32_t last){
  std::atomic<std::uint32_t>next{first};std::vector<std::thread>workers;
  for(unsigned worker=0;worker<kWorkers;++worker)workers.emplace_back([&,last]{
    while(!cancel_){const std::uint32_t value=next.fetch_add(1);if(value>last)break;DiscoveredHost host;host.address=addressText(value);
      for(auto port:kPorts){if(cancel_)break;TcpObservation observation;if(tcpConnect(host.address,port,kConnectTimeoutMs,observation))host.open_ports.push_back(port);}
      std::lock_guard<std::mutex>lock(mutex_);++state_.checked;if(!host.open_ports.empty())state_.hosts.push_back(std::move(host));
    }});
  for (auto& worker : workers) worker.join();
  std::lock_guard<std::mutex> lock(mutex_);
  state_.status = cancel_ ? "cancelled" : "complete";
}
} // namespace reconclave
