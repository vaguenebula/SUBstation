# Licensing

SUBstation's own code is under the **MIT licence** ([LICENSE](../LICENSE)). It is built together with third-party
code under licences of its own, one of which, **Essentia** (the sound similarity's descriptors), is the **GNU Affero
General Public License v3** (AGPLv3). Every build includes Essentia, so **a SUBstation you distribute (an installer,
an executable, a zip) is, as a whole, distributed under the AGPLv3's terms**. Your own code stays MIT: anyone may take
it under MIT alone; only the combined program carries the AGPL's conditions.

This page says what that asks of you when you ship SUBstation, and which parts need a decision. It is a summary of
the licences, not legal advice: for a commercial release, have it checked.

## What is in a build

| Component | Where | Licence | How it is used | Compatible with distributing the whole under the AGPLv3? |
|---|---|---|---|---|
| SUBstation | everything not listed below | MIT | — | yes (MIT code can be part of an AGPL work) |
| [Essentia](../intelligence/third_party/essentia) 2.1-beta5 | intelligence/third_party/essentia | AGPL-3.0-or-later (or a proprietary licence from the Universitat Pompeu Fabra) | statically linked: the sound similarity's descriptors | — it is what makes the whole AGPL |
| KISS FFT 1.3.0 | inside Essentia's folder | BSD-3-Clause | Essentia's FFT | yes |
| TNT | inside Essentia's core | public domain | Essentia's matrices | yes |
| [miniaudio](../engine/third_party/miniaudio) | engine/third_party | Unlicense or MIT-0 | audio I/O, decoding | yes |
| [Signalsmith Stretch, Linear](../engine/third_party) | engine/third_party | MIT | time stretching, FFTs | yes |
| [VST 3 SDK](../engine/third_party/vst3sdk) 3.8.1 | engine/third_party | MIT | plug-in hosting | yes |
| [puff](../app/third_party/puff) 2.3 (zlib 1.3.1's contrib) | app/third_party/puff | zlib | inflate: reading Ableton Live Sets | yes |
| [LAME](../engine/third_party/lame) 3.100 (its encoding library only) | engine/third_party/lame | LGPL-2.0-or-later | statically linked: MP3 export | yes (the LGPL's relinking condition is met by shipping the whole program's source, as the AGPL asks anyway; ship its [COPYING](../engine/third_party/lame/COPYING)). MP3's patents have expired. |
| Qt 6 | not vendored; linked dynamically | LGPL-3.0 (or GPL, or commercial) | the application and its UI | yes (LGPLv3 is compatible; keep Qt dynamically linked and replaceable, and ship its licence) |
| [HUMANBRO](../intelligence/third_party/humanbro) runtime | intelligence/third_party/humanbro | the project's own (no licence file of its own) | Humanize › Velocity | yes, if it is under the MIT licence too: see [below](#to-decide) |
| [velocity.hbm](../intelligence/models) | intelligence/models | none stated; trained on MAESTRO v3, which is CC BY-NC-SA 4.0 | the velocity model | see [below](#to-decide) |
| Steinberg's ASIO SDK | not in the repository; used if present at build time | proprietary *or* GPLv3 (Steinberg's dual licence, since October 2025) | ASIO on Windows | only under its **GPLv3** option: see [below](#to-decide) |
| VST3 plug-ins | loaded at run time | their makers' | hosted | not part of SUBstation: they are separate programs loaded through the VST3 interface |

## Shipping SUBstation: what the AGPLv3 asks

When you give or sell someone a build (the AGPL's "convey"):

1. **Licence it as AGPLv3**: say so where you distribute it, and ship the licence texts: Essentia's
   [COPYING.txt](../intelligence/third_party/essentia/COPYING.txt) (the AGPLv3), [LICENSE](../LICENSE) (MIT), and the
   third-party notices (each `third_party` folder's licence files, and Qt's).
2. **Offer the complete corresponding source** of the build: SUBstation's source at the commit built, Essentia
   included (it is in the repository, with its local changes in
   [local-changes.patch](../intelligence/third_party/essentia/local-changes.patch)), and the build scripts. A tag of
   this public repository that matches the build, linked from where you ship it, is the simplest way; or ship the
   source archive with it. Keep that source available for as long as you offer the build.
3. **Add no restrictions**: no licence terms forbidding modifying, reverse engineering or passing it on. You may still
   charge for it.
4. **Network use (section 13)** applies to a modified SUBstation that users interact with remotely over a network
   (run as a service). A desktop DAW used on someone's own computer isn't; if you ever offer it as a service, its
   users must be offered its source too.
5. **Interactive notices (section 5d)**: Essentia shows none, so SUBstation needn't add them; an *About* dialog that
   names the licences is still good practice.

You **cannot** ship a closed-source SUBstation with Essentia in it. If you ever want to, either license Essentia
commercially from the Universitat Pompeu Fabra (Essentia's proprietary licence), or replace the extractor (the
similarity module takes any `FeatureExtractor`: [intelligence.md](intelligence.md#extending-it)), which leaves the rest
MIT.

## To decide

- **ASIO.** Steinberg's ASIO licence agreement (the proprietary one, the only one older SDKs such as 2.3.3 came with)
  adds conditions the AGPL doesn't allow, so a build with ASIO **and** Essentia mustn't be distributed under it.
  Since October 2025 Steinberg offers the ASIO SDK under GPLv3 as well; GPLv3 code may be combined with AGPLv3 code
  (section 13 of each). So for a build you distribute: use an ASIO SDK that Steinberg offers under GPLv3, take it under
  that licence, and follow Steinberg's trademark rules if you use the ASIO name or logo; or ship without ASIO (WASAPI
  only). Builds for your own use are unaffected: the AGPL's conditions apply to distributing.
- **HUMANBRO's runtime.** The repository calls it "the project's own". If you are its author, the root MIT licence
  covers it; say so in its folder (a `LICENSE` there, or a line in its README) to make it unambiguous.
- **The velocity model.** HUMANBRO's model was trained on MAESTRO v3, which Google publishes under
  [CC BY-NC-SA 4.0](https://magenta.tensorflow.org/datasets/maestro) (non-commercial, share-alike). Whether a model
  trained on a dataset is bound by the dataset's licence is legally unsettled. Shipping SUBstation for free is the
  least exposed; before selling it, either get advice, or ship it without `velocity.hbm` (Humanize › Velocity is then
  greyed out) or with a model trained on data you may use commercially. This is independent of Essentia.

## Building without Essentia

Not offered as a build option: Essentia makes the fingerprints, and the extractor it replaced is gone (its history is
in git). The extractor interface stays, so another extractor (a learned embedding, say) could take its place; a build
with it and without Essentia would then be MIT plus the permissive and LGPL components above (LAME: an LGPL library
linked statically, so such a build must let its users relink it with a changed LAME, e.g. by shipping its object files
or its source).
