#include "evidence_store.h"

#include <fcntl.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "trust_crypto.h"

namespace reconclave {
namespace {

std::uint64_t nowMillis() {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

std::string safe(std::string value, const char* fallback) {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                return c < 0x20 || c == '=';
              }), value.end());
  if (value.size() > 64) value.resize(64);
  return value.empty() ? fallback : value;
}

bool writeAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t count = write(fd, data.data() + sent, data.size() - sent);
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

EvidenceStore::EvidenceStore(std::string directory, std::string node_id)
    : directory_(std::move(directory)), node_id_(std::move(node_id)) {}

std::string EvidenceStore::timelinePath() const { return directory_ + "/timeline.jsonl"; }

EvidenceVerification EvidenceStore::verify() const {
  EvidenceVerification result;
  std::ifstream input(timelinePath());
  if (!input) return result;

  std::string line;
  std::string previous;
  while (std::getline(input, line)) {
    ++result.records;
    json::Value record;
    std::string parse_error;
    if (!json::parse(line, record, parse_error) || !record.isObject()) {
      result.valid = false;
      result.first_invalid_record = result.records;
      result.error = "invalid JSON record";
      return result;
    }
    const auto* stored = record.find("record_hash");
    const auto* linked = record.find("previous_hash");
    if (!stored || !linked ||
        stored->asString() != sha256Hex(json::canonicalStringify(record, "record_hash")) ||
        linked->asString() != previous) {
      result.valid = false;
      result.first_invalid_record = result.records;
      result.error = "evidence hash chain mismatch";
      return result;
    }
    previous = stored->asString();
  }
  return result;
}

bool EvidenceStore::createManifest(const EvidenceContext& context, const std::string& firmware,
                                   json::Value& manifest, std::string& error) const {
  constexpr std::uint64_t kMaximumHashedFileBytes = 16U * 1024U * 1024U;
  constexpr std::size_t kMaximumFiles = 128;
  const auto verification = verify();
  if (!verification.valid) {
    error = "evidence timeline integrity check failed";
    return false;
  }

  DIR* directory = opendir(directory_.c_str());
  if (directory == nullptr) {
    error = std::strerror(errno);
    return false;
  }
  std::vector<std::string> names;
  while (dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || name == "manifest.json" ||
        name == "manifest.json.tmp") continue;
    struct stat candidate {};
    const std::string candidate_path = directory_ + "/" + name;
    if (lstat(candidate_path.c_str(), &candidate) != 0 || !S_ISREG(candidate.st_mode)) continue;
    names.push_back(name);
  }
  closedir(directory);
  std::sort(names.begin(), names.end());
  if (names.size() > kMaximumFiles) {
    error = "evidence store exceeds manifest file limit";
    return false;
  }

  manifest = json::Value::makeObject();
  manifest.set("schema", json::Value::makeString("reconclave-evidence-manifest/v1"));
  manifest.set("generated_at_ms", json::Value::makeNumber(static_cast<double>(nowMillis())));
  manifest.set("node_id", json::Value::makeString(node_id_));
  manifest.set("firmware", json::Value::makeString(firmware));
  manifest.set("project_id", json::Value::makeString(context.project_id));
  manifest.set("engagement_id", json::Value::makeString(context.engagement_id));
  manifest.set("operator_id", json::Value::makeString(context.operator_id));
  manifest.set("timeline_valid", json::Value::makeBool(true));
  manifest.set("timeline_records", json::Value::makeNumber(verification.records));

  json::Value files = json::Value::makeArray();
  for (const auto& name : names) {
    const std::string path = directory_ + "/" + name;
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    json::Value file = json::Value::makeObject();
    file.set("name", json::Value::makeString(name));
    file.set("size_bytes", json::Value::makeNumber(static_cast<double>(info.st_size)));
    if (static_cast<std::uint64_t>(info.st_size) > kMaximumHashedFileBytes) {
      file.set("hash_status", json::Value::makeString("skipped-too-large"));
    } else {
      std::ifstream input(path, std::ios::binary);
      std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (!input.good() && !input.eof()) {
        error = "unable to read evidence file: " + name;
        return false;
      }
      file.set("sha256", json::Value::makeString(sha256Hex(contents)));
      file.set("hash_status", json::Value::makeString("verified"));
    }
    files.push_back(file);
  }
  manifest.set("files", files);
  manifest.set("manifest_hash", json::Value::makeString(
      sha256Hex(json::canonicalStringify(manifest, "manifest_hash"))));

  const std::string temporary = directory_ + "/manifest.json.tmp";
  const std::string destination = directory_ + "/manifest.json";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) {
    error = "unable to create manifest";
    return false;
  }
  output << json::canonicalStringify(manifest) << '\n';
  output.close();
  if (!output.good() || std::rename(temporary.c_str(), destination.c_str()) != 0) {
    error = "unable to publish manifest";
    std::remove(temporary.c_str());
    return false;
  }
  error.clear();
  return true;
}

bool EvidenceStore::append(const std::string& kind, const json::Value& data,
                           const EvidenceContext& context, std::string& record_hash,
                           std::string& error) {
  if (kind.empty() || kind.size() > 64) {
    error = "invalid evidence kind";
    return false;
  }
  if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
    error = std::strerror(errno);
    return false;
  }
  const int fd = open(timelinePath().c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (flock(fd, LOCK_EX) != 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }

  const auto verification = verify();
  if (!verification.valid) {
    error = "refusing to append to invalid evidence chain";
    flock(fd, LOCK_UN);
    close(fd);
    return false;
  }

  std::string previous;
  std::ifstream input(timelinePath());
  std::string line;
  std::string last;
  while (std::getline(input, line)) {
    if (!line.empty()) last = line;
  }
  if (!last.empty()) {
    json::Value prior;
    std::string ignored;
    if (json::parse(last, prior, ignored)) {
      if (const auto* prior_hash = prior.find("record_hash")) previous = prior_hash->asString();
    }
  }

  json::Value record = json::Value::makeObject();
  record.set("schema", json::Value::makeString("reconclave-evidence/v1"));
  record.set("sequence", json::Value::makeNumber(verification.records + 1));
  record.set("timestamp_ms", json::Value::makeNumber(static_cast<double>(nowMillis())));
  record.set("node_id", json::Value::makeString(node_id_));
  record.set("project_id", json::Value::makeString(context.project_id));
  record.set("engagement_id", json::Value::makeString(context.engagement_id));
  record.set("operator_id", json::Value::makeString(context.operator_id));
  record.set("kind", json::Value::makeString(kind));
  record.set("previous_hash", json::Value::makeString(previous));
  record.set("data", data);
  record_hash = sha256Hex(json::canonicalStringify(record));
  record.set("record_hash", json::Value::makeString(record_hash));
  const std::string wire = json::canonicalStringify(record) + "\n";
  const bool ok = writeAll(fd, wire) && fsync(fd) == 0;
  error = ok ? "" : std::strerror(errno);
  flock(fd, LOCK_UN);
  close(fd);
  return ok;
}

EvidenceContext loadEvidenceContext(const std::string& path) {
  EvidenceContext result;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    const auto split = line.find('=');
    if (split == std::string::npos) continue;
    const auto key = line.substr(0, split);
    if (key == "project") result.project_id = safe(line.substr(split + 1), "UNASSIGNED");
    else if (key == "engagement") result.engagement_id = safe(line.substr(split + 1), "UNASSIGNED");
    else if (key == "operator") result.operator_id = safe(line.substr(split + 1), "LOCAL");
  }
  return result;
}

} // namespace reconclave
