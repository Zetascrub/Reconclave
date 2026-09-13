// Capability: input.hid.keystroke — run a DuckyScript payload on the USB HID
// keyboard, invoked by the coordinator over the authenticated protocol.
//
// It is registered as a "trusted" capability, so NodeService authenticates the
// request (HMAC) before the handler runs. The handler then requires a verified
// signed engagement scope via TriggerPolicy. Until signed-scope verification
// exists (milestone 2) scope_verified is false, so this fails closed with
// SCOPE_REQUIRED and never types anything — the node advertises the capability
// but cannot fire it unscoped. This is the "no keystroke without verified scope
// + evidence" rule the project has always required for a remote trigger.
#pragma once

#include "framework.h"

namespace reconclave {

class HidInjectModule : public Module {
 public:
  const char* capabilityId() const override { return "input.hid.keystroke"; }
  void begin(Services& services) override;
};

}  // namespace reconclave
