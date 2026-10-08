#pragma once
// The engine's view of the model's clips and notes (model -> engine
// descriptions). An application-layer header that includes the engine's: for
// the bridge and its tests, never the UI.

#include "model/Clip.h"
#include "model/Track.h"

#include "Engine.h"

#include <vector>

namespace sub::app {

// The engine's view of a clip. Positions stay in beats and seconds; the engine
// converts them to samples at the current tempo and sample rate.
sub::ClipDesc clipDesc(const Clip& clip);
// The engine's view of the audio clips that play: all but the deactivated ones.
std::vector<sub::ClipDesc> clipDescs(const std::vector<Clip>& clips);
// The notes a MIDI track plays, from all of its clips (but deactivated ones), in timeline beats.
std::vector<sub::NoteDesc> noteDescs(const Track& track);
// The notes these MIDI clips play (a deactivated one none), in timeline beats.
std::vector<sub::NoteDesc> clipNoteDescs(const std::vector<Clip>& clips);

}  // namespace sub::app
