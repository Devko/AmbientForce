#pragma once
// The process-wide table library. Every instance's slots read the same tables: they are built
// once per load of the plugin, on one builder thread that the first instance starts, and
// published one by one (in id order) for the audio thread to pick up with an atomic load. Until a
// table is published its slot plays the sine (TableSet::get). While any instance is alive nothing
// is unpublished or freed, so no graveyard is needed; when the module is unloaded (MPC does that
// when the last instance goes) the tables go with it.
//
// Precondition: an instance reports itself closed only once its renders are over for good (a
// host doesn't call effClose while processReplacing runs), so with no instance alive nothing reads
// a table. Counting, starting the builder and releasing the tables share one lock, so an instance
// created while another thread releases never finds its tables freed under it.
//
// The audio thread only calls TableSet::get; everything here is for the other threads.
#include "../dsp/lifetime.h"

namespace af {

TableSet& sharedTables();

// An instance was created (createPlugin): counts it, and the first one starts the builder thread
// (the sine fallback must exist by then: VSTPluginMain builds it). If the thread can't be
// started, it says so in the trace and every slot keeps the sine; the next instance tries again.
// After the module's teardown (unload, exit) it does nothing. Never throws.
void instanceOpened();
// An instance was closed (effClose), its renders over. Never throws.
void instanceClosed();
int liveInstances();

// Starts the builder without counting an instance (the tests); returns at once if it runs or has
// run. Never throws.
void ensureTablesBuilding();

// Stops the builder (it gives up within a pair of frames) and joins it. Then, if no instance is
// alive, unpublishes and frees every table: the slots read the sine again. With instances alive
// (a host exiting while it renders) the published tables stay where they are, for the OS to take
// back. Either way a later start builds what is missing. The module's teardown (unload, exit)
// does this too; the tests call it. Never throws.
void releaseTables();

} // namespace af
