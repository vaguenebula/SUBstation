#pragma once
// Labels for a project's tracks: a few words saying what each is ("Washed Out
// Serum Pluck", "Orchestral Trumpet", "Sad Piano Line", "Drum Group"), for
// people (the track name's tooltip) and for agents (the MCP server's context).
//
// What a track is comes from everything that names or shapes it, each weighed by
// how much it says: the user's own name for it; the preset its instrument has
// loaded (Serum's "PL - Electric Flow" is a pluck; Spitfire's "Core - Trumpet" a
// trumpet), a rack's name, the instrument itself (Serum is a synth; "Cinematic
// Soft Piano" a piano), a sampler's sample; its audio files' names (by how much
// each plays); its notes (low and one at a time: a bass; struck together:
// chords; fast and wide: an arp); its effects and how much they do (a reverb
// half wet washes it out), its sends to returns, its fader and pan. Groups are
// named by what they hold, returns by their effects.
//
// Then the project as a whole: a trait most tracks share tells little (a reverb
// on every track), so it weighs less; and tracks that come out alike are told
// apart by what differs (another trait, their register, their presets' or
// files' own names), numbered only if nothing does. So the same facts always
// give the same labels: rules, no model (docs/intelligence.md#track-labels).

#include <vector>

#include "labels/Facts.h"

namespace sub::intelligence::labels {

// Every track's label, in the order given. Returns and groups are labelled from
// the tracks around them (a group by those whose parent it is), so a project's
// tracks go in together: its tracks, groups, returns and master.
std::vector<TrackLabel> labelTracks(const std::vector<TrackFacts>& tracks);

}  // namespace sub::intelligence::labels
