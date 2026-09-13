#include "ducky_script.h"

#include <vector>

namespace reconclave {
namespace {

// Splits on runs of spaces/tabs. Good enough for DuckyScript's line grammar
// (no quoting, STRING/STRINGLN take the literal rest-of-line separately).
std::vector<String> tokenize(const String& line) {
  std::vector<String> tokens;
  int i = 0;
  const int len = line.length();
  while (i < len) {
    while (i < len && isspace(static_cast<unsigned char>(line[i]))) i++;
    int start = i;
    while (i < len && !isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i > start) tokens.push_back(line.substring(start, i));
  }
  return tokens;
}

// Rest-of-line after the first token, for STRING/STRINGLN - preserves
// internal spacing/case, only strips the single separating space.
String restOfLine(const String& line) {
  int i = 0;
  const int len = line.length();
  while (i < len && !isspace(static_cast<unsigned char>(line[i]))) i++;
  while (i < len && isspace(static_cast<unsigned char>(line[i]))) i++;
  return line.substring(i);
}

String upper(const String& s) {
  String out = s;
  out.toUpperCase();
  return out;
}

// Named non-printable keys, matching the constants USBHIDKeyboard.h defines
// (which the Arduino Keyboard API already overloads onto its "ASCII" write()/
// press() - see that header for why these look like odd byte values).
bool namedKeyToCode(const String& name, uint8_t& code_out) {
  const String n = upper(name);
  struct Entry { const char* name; uint8_t code; };
  static const Entry table[] = {
      {"ENTER", KEY_RETURN},     {"RETURN", KEY_RETURN},
      {"TAB", KEY_TAB},          {"ESC", KEY_ESC},
      {"ESCAPE", KEY_ESC},       {"SPACE", ' '},
      {"BACKSPACE", KEY_BACKSPACE}, {"DELETE", KEY_DELETE},
      {"DEL", KEY_DELETE},       {"HOME", KEY_HOME},
      {"END", KEY_END},          {"INSERT", KEY_INSERT},
      {"PAGEUP", KEY_PAGE_UP},   {"PAGEDOWN", KEY_PAGE_DOWN},
      {"UP", KEY_UP_ARROW},      {"UPARROW", KEY_UP_ARROW},
      {"DOWN", KEY_DOWN_ARROW},  {"DOWNARROW", KEY_DOWN_ARROW},
      {"LEFT", KEY_LEFT_ARROW},  {"LEFTARROW", KEY_LEFT_ARROW},
      {"RIGHT", KEY_RIGHT_ARROW}, {"RIGHTARROW", KEY_RIGHT_ARROW},
      {"CAPSLOCK", KEY_CAPS_LOCK},
      {"F1", KEY_F1}, {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4},
      {"F5", KEY_F5}, {"F6", KEY_F6}, {"F7", KEY_F7}, {"F8", KEY_F8},
      {"F9", KEY_F9}, {"F10", KEY_F10}, {"F11", KEY_F11}, {"F12", KEY_F12},
  };
  for (const Entry& e : table) {
    if (n == e.name) {
      code_out = e.code;
      return true;
    }
  }
  return false;
}

bool modifierToCode(const String& name, uint8_t& code_out) {
  const String n = upper(name);
  if (n == "CTRL" || n == "CONTROL") { code_out = KEY_LEFT_CTRL; return true; }
  if (n == "SHIFT") { code_out = KEY_LEFT_SHIFT; return true; }
  if (n == "ALT") { code_out = KEY_LEFT_ALT; return true; }
  if (n == "GUI" || n == "WINDOWS" || n == "CMD") { code_out = KEY_LEFT_GUI; return true; }
  return false;
}

// Parses "123" (optionally with trailing junk ducky scripts sometimes carry,
// e.g. "100ms") into a non-negative long; -1 on no leading digits at all.
long parseNonNegative(const String& s) {
  String trimmed = s;
  trimmed.trim();
  if (trimmed.length() == 0 || !isdigit(static_cast<unsigned char>(trimmed[0]))) return -1;
  return trimmed.toInt();
}

// A single key-combo line: zero or more modifiers followed by exactly one
// named key or single printable character, e.g. "CTRL ALT DELETE", "GUI r".
bool pressComboLine(const std::vector<String>& tokens, USBHIDKeyboard& keyboard,
                     String& error_out) {
  if (tokens.empty()) return true;  // Blank line, nothing to do.
  std::vector<uint8_t> modifiers;
  uint8_t main_key = 0;
  bool have_main_key = false;
  for (size_t i = 0; i < tokens.size(); i++) {
    uint8_t code;
    const bool is_last = (i + 1 == tokens.size());
    if (!is_last && modifierToCode(tokens[i], code)) {
      modifiers.push_back(code);
      continue;
    }
    if (namedKeyToCode(tokens[i], code)) {
      main_key = code;
      have_main_key = true;
    } else if (tokens[i].length() == 1) {
      main_key = static_cast<uint8_t>(tokens[i][0]);
      have_main_key = true;
    } else if (modifierToCode(tokens[i], code)) {
      // A modifier alone on the last token (e.g. just "GUI") - treat the
      // modifier itself as the pressed key so it's not silently dropped.
      main_key = code;
      have_main_key = true;
    } else {
      error_out = "unrecognized key/command: " + tokens[i];
      return false;
    }
  }
  if (!have_main_key) {
    error_out = "line has modifiers but no key";
    return false;
  }
  for (uint8_t m : modifiers) keyboard.press(m);
  keyboard.press(main_key);
  delay(5);  // Let the host register the combo before it gets released again.
  keyboard.releaseAll();
  return true;
}

struct Executor {
  USBHIDKeyboard& keyboard;
  unsigned long default_delay_ms = 0;
  unsigned long started_at_ms = 0;
  int steps_run = 0;

  bool timeBudgetExceeded() const {
    return millis() - started_at_ms > kDuckyMaxTotalRuntimeMs;
  }

  // Runs one already-tokenized, already-classified line. Returns false with
  // error_out set on a hard parse/runtime error; REPEAT is handled by the
  // caller (it needs the *previous* line's tokens, which live one level up).
  bool runLine(const String& raw_line, const std::vector<String>& tokens,
               String& error_out) {
    const String cmd = upper(tokens[0]);
    if (cmd == "REM") {
      return true;
    }
    if (cmd == "STRING" || cmd == "STRINGLN") {
      keyboard.print(restOfLine(raw_line));
      if (cmd == "STRINGLN") keyboard.write(KEY_RETURN);
      steps_run++;
      return true;
    }
    if (cmd == "DELAY") {
      long ms = parseNonNegative(restOfLine(raw_line));
      if (ms < 0) { error_out = "DELAY needs a non-negative number"; return false; }
      if (static_cast<unsigned long>(ms) > kDuckyMaxSingleDelayMs) {
        ms = kDuckyMaxSingleDelayMs;
      }
      delay(ms);
      steps_run++;
      return true;
    }
    if (cmd == "DEFAULTDELAY" || cmd == "DEFAULT_DELAY") {
      long ms = parseNonNegative(restOfLine(raw_line));
      if (ms < 0) { error_out = "DEFAULTDELAY needs a non-negative number"; return false; }
      default_delay_ms = (static_cast<unsigned long>(ms) > kDuckyMaxSingleDelayMs)
                              ? kDuckyMaxSingleDelayMs
                              : static_cast<unsigned long>(ms);
      return true;
    }
    // Anything else is a key-combo line: re-tokenize the whole line (tokens
    // already holds this - cmd here is just tokens[0]'s upper-cased form).
    if (!pressComboLine(tokens, keyboard, error_out)) return false;
    steps_run++;
    return true;
  }
};

}  // namespace

DuckyScriptResult runDuckyScript(const String& body, USBHIDKeyboard& keyboard) {
  DuckyScriptResult result;
  Executor exec{keyboard};
  exec.started_at_ms = millis();

  std::vector<String> lines;
  int start = 0;
  for (int i = 0; i <= body.length(); i++) {
    if (i == body.length() || body[i] == '\n') {
      String line = body.substring(start, i);
      line.trim();
      // A trailing \r from CRLF-authored scripts (the web textarea can save
      // either) would otherwise become a stray, unrecognized last "token".
      while (line.length() > 0 && line[line.length() - 1] == '\r') {
        line.remove(line.length() - 1);
      }
      lines.push_back(line);
      start = i + 1;
    }
  }
  if (static_cast<int>(lines.size()) > kDuckyMaxLines) {
    result.error = "script has " + String(lines.size()) + " lines, max is " +
                    String(kDuckyMaxLines);
    return result;
  }

  std::vector<String> previous_tokens;
  String previous_raw;
  for (size_t i = 0; i < lines.size(); i++) {
    if (exec.timeBudgetExceeded()) {
      result.error = "script exceeded the " + String(kDuckyMaxTotalRuntimeMs) +
                      "ms runtime budget";
      result.line_number = static_cast<int>(i) + 1;
      result.steps_run = exec.steps_run;
      return result;
    }
    const String& line = lines[i];
    if (line.length() == 0) continue;
    std::vector<String> tokens = tokenize(line);
    if (tokens.empty()) continue;

    if (upper(tokens[0]) == "REPEAT") {
      long count = parseNonNegative(restOfLine(line));
      if (count < 0) {
        result.error = "REPEAT needs a non-negative number";
        result.line_number = static_cast<int>(i) + 1;
        return result;
      }
      if (count > kDuckyMaxRepeatCount) count = kDuckyMaxRepeatCount;
      if (previous_tokens.empty()) {
        result.error = "REPEAT with no preceding command";
        result.line_number = static_cast<int>(i) + 1;
        return result;
      }
      for (long r = 0; r < count; r++) {
        if (exec.timeBudgetExceeded()) {
          result.error = "script exceeded the " + String(kDuckyMaxTotalRuntimeMs) +
                          "ms runtime budget";
          result.line_number = static_cast<int>(i) + 1;
          result.steps_run = exec.steps_run;
          return result;
        }
        String error;
        if (!exec.runLine(previous_raw, previous_tokens, error)) {
          result.error = error;
          result.line_number = static_cast<int>(i) + 1;
          result.steps_run = exec.steps_run;
          return result;
        }
        if (exec.default_delay_ms > 0) delay(exec.default_delay_ms);
      }
      continue;
    }

    String error;
    if (!exec.runLine(line, tokens, error)) {
      result.error = error;
      result.line_number = static_cast<int>(i) + 1;
      result.steps_run = exec.steps_run;
      return result;
    }
    if (upper(tokens[0]) != "REM") {
      previous_tokens = tokens;
      previous_raw = line;
    }
    if (exec.default_delay_ms > 0) delay(exec.default_delay_ms);
  }

  result.ok = true;
  result.steps_run = exec.steps_run;
  return result;
}

}  // namespace reconclave
