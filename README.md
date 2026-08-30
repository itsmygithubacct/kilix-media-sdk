# kilix-media-sdk

The Kilix media domain parent. It owns aggregate CI, dependency ordering,
release manifests, the licence inventory and integration fixtures; each
component keeps its own version, changelog, licences, headers, tests and
namespaced release tag.

**State: BUILT — AWAITING INDEPENDENT ACCEPTANCE.** Nothing here is accepted,
tagged or released by this repository's own authors.

## Components present — 1/6

| Component | Version | State |
| --- | --- | --- |
| [`kilix-acoustic-link`](components/kilix-acoustic-link) | 0.1.0 | built; 0/6 physical profiles graduated |

## Components not yet migrated — 5/6

`kilix-mask`, `kilix-motion-detect`, `kilix-object-detect`,
`kilix-sound-detect` and `kilix-rtsp` are named in the F111 placement
decision as the media components that belong in this parent. **They have not
been migrated and this repository does not contain them.** Their migration is
a separate, reviewed, history-preserving move owned by their own streams; it
is not performed here and no history is rewritten by this repository.

## Building

Each component builds standalone and offline:

```sh
make -C components/kilix-acoustic-link
make -C components/kilix-acoustic-link test
```

The aggregate helpers build and test every present component:

```sh
make          # build all present components
make test     # test all present components
```

## Release

A component release uses a namespaced tag such as
`kilix-acoustic-link/v0.1.0`. **No tag is created by this repository's
authors.** Tags, pushes to `main`, release pins and publication are reserved
to the owner.

## Licence

MIT, see [`LICENSE`](LICENSE). Each component carries its own licence and
third-party notices.
