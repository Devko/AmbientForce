#pragma once
// The process-wide table library. Every instance's slots read the same tables: they are built
// once per load of the plugin, on one builder thread started by the first VSTPluginMain, and
// published one by one (in id order) for the audio thread to pick up with an atomic load. Until a
// table is published its slot plays the sine (TableSet::get). While any instance exists nothing
// is unpublished or freed, so no graveyard is needed; when the module is unloaded (MPC does that
// when the last instance goes) the tables go with it.
#include "../dsp/lifetime.h"

namespace af {

TableSet& sharedTables();

// The first call builds the sine fallback and starts the builder thread; later calls return at
// once. If the thread can't be started, it says so in the trace and every slot keeps the sine
// (a later call tries again). Never throws. Not from the audio thread.
void ensureTablesBuilding();

// Stops the builder (it gives up within a pair of frames), joins it, then unpublishes and frees
// every table: the slots read the sine again, and a later ensureTablesBuilding() starts over.
// Only when no instance can be rendering. The static that owns the builder calls it when the
// module is unloaded (and at process exit); the tests call it. Never throws.
void releaseTables();

} // namespace af
