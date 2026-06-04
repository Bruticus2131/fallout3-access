#pragma once
// Force-included on every TU. Combines the FOSE prefix (UInt32 typedefs +
// version macros) with <string>, which Tile::GetQualifiedName declares but
// FOSE's own headers don't pull in.
#include "fose_common/fose_prefix.h"
#include <string>
