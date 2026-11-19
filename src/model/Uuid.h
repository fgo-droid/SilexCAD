#pragma once
#include <string>

namespace silex {

/// Stable identifier (RFC 4122 v4, lowercase text). The UI only ever talks to the
/// kernel through these identifiers and persistent topological names.
using Uuid = std::string;

Uuid newUuid();

}  // namespace silex
