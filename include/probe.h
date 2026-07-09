#pragma once

#include <stdbool.h>

bool probe_init(void);
void probe_poll(void);

// (Re)detect the target; safe to call while running (used when no target
// was present at boot). Returns true if a target is attached afterwards.
bool probe_attach(void);

