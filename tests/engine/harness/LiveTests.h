#pragma once
// Tests that play live through the fake ASIO driver, which test_parallel_live.cpp
// runs again with the engine rendering on workers (RecordingTest(true)): live,
// held, recorded and preview notes and recorded audio behave as on one thread.
// Each is defined beside its own TEST_CASE, in the file named.

#include <string>

#include "harness/Recording.h"

namespace subtest::live {

// test_recording.cpp
void monitoringThroughALatentPlugin(RecordingTest& t);
void loopbackTakeLinesUp(RecordingTest& t, const std::string& latency);  // "none", "track" or "master"
void autoMonitoringWhileRecording(RecordingTest& t);
void countIn(RecordingTest& t);
void whatEndsARecording(RecordingTest& t);

// test_midi_input.cpp
void liveNotesReachTheInstrument(RecordingTest& t);
void aNoteReleasedAsItIsPlayedStillSounds(RecordingTest& t);
void whichTracksHearWhichInput(RecordingTest& t);
void midiMonitoring(RecordingTest& t);
void heldKeysAreReleased(RecordingTest& t);
void recordedNotesLandOnTheirBeats(RecordingTest& t, const std::string& latency);  // "none", "track" or "master"
void midiAndAudioRecordTogether(RecordingTest& t);

// test_resampling_engine.cpp
void aResampledTakeEqualsItsSourcesRender(RecordingTest& t, const std::string& source);  // "track", "latent track", "group" or "master"
void monitoringATrackIsNotDelayed(RecordingTest& t);
void soloAcrossAMonitoredInput(RecordingTest& t);

}  // namespace subtest::live
