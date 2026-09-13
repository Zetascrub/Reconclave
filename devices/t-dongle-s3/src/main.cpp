// Reconclave T-Dongle-S3 FLEET NODE entry point.
//
// A trust-governed Reconclave node on the T-Dongle hardware, distinct from the
// standalone ZetaDongle firmware. It joins the fleet network (STA), advertises
// _reconclave._tcp, serves the reconclave/1 announce + message endpoints, and
// authenticates every request with the provisioned HMAC scheme.
//
// Registered capabilities:
//   input.hid.keystroke - run a DuckyScript payload on the USB HID keyboard,
//                         invoked by the coordinator. AUTHENTICATED + requires a
//                         verified signed engagement scope; scope verification
//                         is milestone 2, so today it fails closed
//                         (SCOPE_REQUIRED) and cannot type unscoped.
//
// Mass storage (MSC) deliberately lives on the standalone ZetaDongle, not here.
#include "app.h"
#include "hid_inject_module.h"

namespace {
reconclave::App g_app;
reconclave::HidInjectModule g_hid_inject;
}  // namespace

void setup() {
  g_app.addModule(g_hid_inject);
  g_app.begin();
}

void loop() { g_app.loop(); }
