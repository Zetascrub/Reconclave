#pragma once

#include <cstdint>
#include <string>

#include "json.h"

namespace reconclave {

struct EvidenceContext {
  std::string project_id{"UNASSIGNED"};
  std::string engagement_id{"UNASSIGNED"};
  std::string operator_id{"LOCAL"};
};

struct EvidenceVerification {
  bool valid = true;
  std::uint64_t records = 0;
  std::uint64_t first_invalid_record = 0;
  std::string error;
};

class EvidenceStore {
 public:
  EvidenceStore(std::string directory, std::string node_id);
  bool append(const std::string& kind, const json::Value& data,
              const EvidenceContext& context, std::string& record_hash,
              std::string& error);
  EvidenceVerification verify() const;
  bool createManifest(const EvidenceContext& context, const std::string& firmware,
                      json::Value& manifest, std::string& error) const;
  std::string timelinePath() const;

 private:
  std::string directory_;
  std::string node_id_;
};

EvidenceContext loadEvidenceContext(const std::string& path);

}  // namespace reconclave
