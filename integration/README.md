# Integration fixtures

Cross-component fixtures live here. Today the only component present is
`kilix-acoustic-link`, whose own fixtures — golden KAL1 frames and the fuzz
corpus — live with the component under
`components/kilix-acoustic-link/tests/vectors/`, because they are the
component's contract rather than a cross-component one.

**No recorded-room audio fixture exists.** Recording one requires the
two-device real-room matrix and sound authorization, neither of which has
been granted; see the component README's physical-qualification section.
