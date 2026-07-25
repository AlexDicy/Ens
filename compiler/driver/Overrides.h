#pragma once

// `ens override add|remove|list`: manage the ens.overrides file next to the workspace's
// ens.package discovered from the current folder. Returns the process exit code.
int runOverrideCommand(int argc, char* argv[]);
