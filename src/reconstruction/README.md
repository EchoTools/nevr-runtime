# Game function reconstructions

These files are reference-only C++ reconstructions of game functions from ReVault evidence.
Each function lives once under its game module, shared across PC echovr.exe and Quest libr15.so.
Each header names binary RVAs, ReVault binary/node/tier, and who assessed the grade.
Files under this directory are not added to any CMake target.
A hook change can include one of these functions when it needs the stock behavior.
Keep hook installation and platform address tables in their existing runtime homes.
Reconstruct each stock function from evidence before relying on its source.
Reference reconstructions are not linked, tested, or executed by this repository.
