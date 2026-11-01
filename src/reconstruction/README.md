# Game function reconstructions

Full Quest function bodies recorded in ReVault compile into the
`nevr_r15_reconstruction` static library. The library is not linked into the
runtime; it is available to downstream reconstruction consumers. Add a source
to `CMakeLists.txt` only after its complete function body has been reconstructed
in ReVault. Partial reference transcriptions remain outside the target.

Each compiled function records its Quest VA, ReVault binary and node, and
confidence. A PCVR address is included only when independently established.
Hook installation and platform address tables stay in their runtime homes.
