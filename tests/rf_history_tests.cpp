#include "../devices/cardputer-adv/src/rf_history.h"
#include <cassert>
#include <limits>

int main() {
  cap::RfHistory first, second;
  assert(first.activityPercent(-80) == 0);
  assert(!first.connectedToPrevious(0));
  cap::RfHistory activity;
  activity.push(-90, 0xFFFFFFC0u);
  activity.push(-80, 36); // 100 ms across uptime rollover.
  activity.push(-70, 136);
  activity.push(-100, 1000); // Pause must break the trace.
  assert(activity.activityPercent(-80) == 50);
  assert(activity.activityPercent(-120) == 100);
  assert(activity.activityPercent(-30) == 0);
  assert(activity.connectedToPrevious(1));
  assert(activity.connectedToPrevious(2));
  assert(!activity.connectedToPrevious(3));
  assert(!activity.connectedToPrevious(4));
  for (size_t i = 0; i < cap::RfHistory::capacity; ++i) activity.push(-90);
  assert(activity.activityPercent(-80) == 0); // Expired bursts leave the window.
  assert(first.size() == 0);
  assert(first.at(0) == -120);
  // Invalid radio readings must not produce a spike or displace real history.
  assert(!first.push(std::numeric_limits<float>::quiet_NaN()));
  assert(!first.push(std::numeric_limits<float>::infinity()));
  assert(first.size() == 0);
  for (size_t i = 0; i < 450; ++i) first.push(static_cast<float>(i), static_cast<uint32_t>(i * 100));
  assert(first.size() == 200);
  assert(first.at(0) == 250);
  assert(first.at(199) == 449);
  assert(first.sampledAt(0) == 25000);
  assert(first.sampledAt(199) == 44900);
  assert(first.at(200) == -120);
  second.push(-80);
  assert(second.size() == 1 && second.at(0) == -80);
  assert(first.at(199) == 449);
  second.clear();
  assert(second.size() == 0 && second.sampledAt(0) == 0);
  second.push(-60, 123);
  assert(second.at(0) == -60 && second.sampledAt(0) == 123);
  // Strong and weak outliers clip at the chart boundary, never outside it.
  assert(cap::RfHistory::height(-150, 40) == 0);
  assert(cap::RfHistory::height(-120, 40) == 0);
  assert(cap::RfHistory::height(-75, 40) == 20);
  assert(cap::RfHistory::height(-30, 40) == 40);
  assert(cap::RfHistory::height(5, 40) == 40);
  assert(cap::RfHistory::height(-75, 0) == 0);
  assert(cap::RfHistory::height(std::numeric_limits<float>::quiet_NaN(), 40) == 0);
}
