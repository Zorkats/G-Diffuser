# gnet_allowlist_tool

The owner's tool for the G-NET stable allowlist. It makes the signed list that public rooms check before a join, and it checks a list the same way a game copy does. The format and the gate are documented in `port/net/gdx_net_allowlist.h`.

The game ships only the public key. The seed stays with the owner, or in the release secret store, and never goes into the repository.

Build target: `gnet_allowlist_tool`. Link it with Monocypher. On Windows, also link `bcrypt`.

## Commands

```
gnet_allowlist_tool keygen <secret-out> <public-out>
gnet_allowlist_tool sign <secret-file> <in.txt> <out.bin>
gnet_allowlist_tool sign --secret-env NAME <in.txt> <out.bin>
gnet_allowlist_tool verify <public-hex> <in.bin>
```

Exit codes: `0` for success, `1` when signing or verification fails, `2` for a usage or input error. An existing file is never overwritten, so a bad run exits `2` and leaves the old file in place.

### keygen

Creates a new key pair. The seed, as 64 hex digits, goes to `<secret-out>`. The public key, as 64 hex digits, goes to `<public-out>`. The public key is also printed. The seed is never printed. On POSIX systems the secret file is owner-only (mode 600).

Run keygen once. A new run makes a new key, and the game would need a build with that public key.

### Input file

One directive per line. A line starting with `#` is a comment. Blank lines and trailing spaces are ignored.

| Line | Meaning |
| --- | --- |
| `protocol N` | The protocol id that public rooms require. Required. |
| `sequence N` | The list's number. Raise it with every new list. Required. |
| `issued N` | When the list was signed, in Unix seconds. Informational. Required. |
| `grace_until N` | Unix seconds after which older stable builds stop being admitted. `0` means no deadline. Optional, default `0`. |
| `current TAG` | The stable release that public rooms require. Must also appear as an `allow` line. Required. |
| `allow TAG` | A stable release that may still join. Repeat for each one. |
| `yank TAG REASON` | A pulled build and the reason players see. The reason is the rest of the line and may be empty. Repeat for each one. |

Rules the tool enforces:

- A tag is 1 to 48 printable characters with no space.
- A reason is 0 to 120 printable characters.
- At most 255 `allow` lines and 255 `yank` lines.
- No tag appears twice in `allow`, and none appears twice in `yank`.
- `protocol`, `sequence`, `issued`, `current` and `grace_until` each appear at most once.

Example:

```
# stable list, sequence 11
protocol 3
sequence 11
issued 1763000000
current 1.3.0
grace_until 1765600000
allow 1.3.0
allow 1.2.1
yank 1.2.0 Desyncs in the race intro
```

### sign

Reads the seed, checks the list, and signs it. Before writing anything, the tool decodes the signed bytes with the public key derived from the same seed, and writes nothing if that check fails. The tool prints the sequence, the counts and the public key.

`sign --secret-env NAME` reads the seed from an environment variable instead of a file. Use it in CI so the seed is never written to disk. Neither form prints the seed.

### verify

Checks the signature and the structure with the given public key, then prints the fields. The output starts with `verify: ok` on success. Otherwise it prints `verify: bad signature` or `verify: malformed` and exits `1`.

## Sequence and grace

A copy refuses any list whose sequence is below the highest one it has accepted. Within one run, it also refuses a different list with that same sequence. To correct a bad list, publish it again with a higher sequence, such as 12 after 11.

`grace_until` is for dropping a stable build from the allowed set without cutting off players at once. Before that time, older builds that are still listed may join. Admission ends at whichever comes first: the `grace_until` time, or removing the tag from `allow` in a new list.

## Release steps

1. Keep the seed in a secret store, never in the repository.
2. Write the list file, with a sequence higher than the last one published.
3. Sign it, then verify the output with the public key the game build expects.
4. Publish the `.bin` with the release.

For CI, store the seed as the environment secret `GNET_ALLOWLIST_SEED` in a GitHub environment that requires approval. Run the sign step only on manual dispatch or tag push, never on `pull_request`, and pass the seed with `--secret-env GNET_ALLOWLIST_SEED`.
