#pragma once
// The process-wide table library. Every instance's slots read the same tables: they are built
// once per load of the plugin, on one builder thread started by the first VSTPluginMain, and
// published one by one (in id order) for the audio thread to pick up with an atomic load. Until a
// table is published its slot plays the sine (TableSet::get). While any instance exists nothing
// is unpublished or freed, so no graveyard is needed; when the module is unloaded (MPC does that
// when the last instance goes) the tables go with it.
//
// Not from the audio thread, any of these: the audio thread only calls TableSet::get.
#include "../dsp/lifetime.h"

namespace af {

TableSet& sharedTables();

// The first call builds the sine fallback and starts the builder thread; later calls return at
// once. If the thread can't be started, it says so in the trace and every slot keeps the sine
// (a later call tries again). Never throws. Not from the audio thread.
void ensureTablesBuilding();

// Stops the builder (it gives up within a pair of frames) and joins it. Then, if no instance is
// alive, unpublishes and frees every table: the slots read the sine again. With instances alive
// (a host exiting while it renders) the published tables stay where they are, for the OS to take
// back. Either way a later ensureTablesBuilding() builds what is missing. The static that owns
// the builder calls it when the module is unloaded and at process exit; the tests call it.
// Never throws.
void releaseTables();

// The live plugin instances (createPlugin and effClose report them): the tables are freed only
// when there are none.
void instanceOpened();
void instanceClosed();
int liveInstances();

} // namespace af
