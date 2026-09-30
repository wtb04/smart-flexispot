#pragma once

#include "job_types.h"

// Work that comes round now and then, run by two shared workers instead of a
// task of its own each: a stack apiece was most of the internal RAM the radio
// and TLS need. Each says how often, what it waits for (the network, the
// screen) and how it backs off, and the rules live here once.
//
//     jobs::Spec spec;
//     spec.name      = "clock";
//     spec.period_ms = 1000;
//     spec.run       = [] { show_time(); return jobs::done(); };
//     s_job = jobs::add(std::move(spec));
namespace jobs {

/** Thread-safe; the first starts the workers. */
Job  add(Spec spec);
/** Runs it as soon as its worker is free, or again once it finishes. */
void poke(Job job);

int         count();
const char *name(Job job);
Status      status(Job job);

}  // namespace jobs
