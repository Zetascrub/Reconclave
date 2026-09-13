#include "hid_inject_module.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "ducky_script.h"
#include "node_service.h"
#include "usb_manager.h"

namespace reconclave {

void HidInjectModule::begin(Services& s) {
  // App::services_ outlives every module, so capturing the addresses of the
  // long-lived service objects is safe.
  TriggerPolicy* trigger = &s.trigger;
  UsbManager* usb = &s.usb;

  s.node.registerCapability(
      "input.hid.keystroke", "trusted",
      [trigger, usb](JsonVariantConst args, const RequestContext& ctx) -> CapabilityOutcome {
        CapabilityOutcome out;

        // Network trigger: authenticated by NodeService, but a verified signed
        // engagement scope is still required. scope_verified is false until
        // milestone 2, so this fails closed here.
        TriggerContext tctx;
        tctx.source = TriggerSource::Network;
        tctx.authenticated = ctx.authenticated;
        tctx.scope_verified = false;  // TODO(m2): verify args["_scope_delegation"]
        String reason;
        if (!trigger->authorize("input.hid.keystroke", tctx, reason)) {
          out.status = "rejected";
          out.error_code = "SCOPE_REQUIRED";
          out.error_message = reason;
          Serial.printf("hid.inject: fail-closed (%s)\n", reason.c_str());
          return out;
        }

        // Reachable only once scope verification is added and passes. Kept
        // compiled so the execution path is wired, not stubbed.
        const String script = args["script"] | "";
        const DuckyScriptResult r = runDuckyScript(script, usb->keyboard());
        JsonDocument res;
        res["steps_run"] = r.steps_run;
        res["line_number"] = r.line_number;
        String result_json;
        serializeJson(res, result_json);
        out.result_json = result_json;
        if (r.ok) {
          out.status = "ok";
        } else {
          out.status = "error";
          out.error_code = "SCRIPT_ERROR";
          out.error_message = r.error;
        }
        return out;
      });
}

}  // namespace reconclave
