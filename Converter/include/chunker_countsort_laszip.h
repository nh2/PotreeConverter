
#pragma once

#include <string>
#include <vector>

#include "Vector3.h"
#include "Attributes.h"
#include "Monitor.h"

using std::string;
using std::vector;

class Source;
class State;

namespace chunker_countsort_laszip {

	void doChunking(vector<Source> sources, string targetDir, Vector3 min, Vector3 max, State& state, Attributes outputAttributes, Monitor* monitor);

	// The chunker lays a `chunkingGridSize(totalPoints)^3` counting grid
	// over the global bounding cube and produces one chunk per occupied cell.
	// A chunk holding more than `chunkingMaxPointsPerChunk(totalPoints)` points
	// is further subdivided by the indexer.
	// These two functions are the single source of truth for those choices.
	int64_t chunkingGridSize(int64_t totalPoints);
	int64_t chunkingMaxPointsPerChunk(int64_t totalPoints);

}
