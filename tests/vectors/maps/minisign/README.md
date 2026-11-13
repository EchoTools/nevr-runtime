# minisign test vector

`test_key.pub` is a minisign key made for this test (`minisign -G -W`, key id `12CF26D38A5D6FFB`). It signs nothing
else and is trusted by nothing: the secret half was not kept. `test_manifest.json` is a channel manifest in the
`nevr-maps-manifest` v1 shape, and `test_manifest.json.minisig` is its signature, made by the minisign tool itself
(`minisign -S`, the prehashed `ED` algorithm). Its trusted comment carries no date. `map_manifest_test` checks that
nevr's verifier accepts the tool's signature, that a flipped byte is refused, and that the signed object built from
the two files parses.
