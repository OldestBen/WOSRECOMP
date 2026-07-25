# private/

Drop your own legally dumped Xbox 360 files here:

```
private/
├── default.xex     # required — the main game executable
└── default.xexp    # optional — title update patch, if you have one
```

**Nothing in this directory is ever committed.** It's excluded via
[`.gitignore`](../.gitignore). This repo ships no copyrighted game files —
you must dump your own from a disc/copy you legally own.

**Easiest way to get files here:** run `tools/import_dump.sh <path-to-your-extracted-folder-or-.iso>`
from the repo root — it finds `default.xex`/`default.xexp` wherever they are
in your dump, copies them here, and prints a safe (non-copyrighted) summary
you can share for help with the config. See
[`docs/01-getting-started.md`](../docs/01-getting-started.md) for details.
