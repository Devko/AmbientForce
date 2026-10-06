#pragma once
// The process-wide table library. Every instance's slots read the same tables: they are built
// once per process, on one builder thread started by the first VSTPluginMain, and published one
// by one (in id order) for the audio thread to pick up with an atomic load. Until a table is
// published its slot plays the sine (TableSet::get). Tables live for the process: nothing is
// ever unpublished or freed, so no graveyard is needed.
#include "../dsp/lifetime.h"

namespace af {

TableSet& sharedTables();

// The first call builds the sine fallback and starts the builder thread; later calls return at
// once. If the thread can't be started, it says so in the trace and every slot keeps the sine
// (a later call tries again). Never throws. Not from the audio thread.
void ensureTablesBuilding();

} // namespace af
