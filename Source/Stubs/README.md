# Compile-only Papyrus stubs

These are **not** MEO scripts. They are one-line-ish stubs of base-game types
(LocationRefType, GlobalVariable, Class, Light, EffectShader, SKI_ConfigBase,
...) that the SKSE64 source set references but does not include. Papyrus needs
every referenced type to resolve at compile time even when we never call it
(see MANUAL_MOD_CREATION_GUIDE "The stub technique").

Rules:
- They exist ONLY to satisfy the compiler. `tools/compile.sh` adds this folder
  to the import path AFTER `Source/Scripts` and BEFORE the SKSE sources.
- Never ship the `.pex` of a stub — the real implementations live in the game
  at runtime and Papyrus links by name at load.
- If a compile fails with "unknown type X" for a vanilla type, add `X.psc`
  here (copy the stub from MRO's `Source/Scripts` or write
  `Scriptname X extends Form Hidden`).
