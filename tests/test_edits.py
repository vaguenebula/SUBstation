from dataclasses import replace

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio.model import edits
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import Clip, Project, new_id

TEMPO = 120.0  # 1 beat = 0.5 s


def clip(start, beats, offset=0.0, source=100.0, cid=None):
    return Clip(id=cid or new_id(), path="a.wav", name="a", start_beat=start, duration_sec=beats * 0.5,
                offset_sec=offset, source_duration_sec=source)


def spans(clips):
    return [(round(c.start_beat, 6), round(c.end_beat(TEMPO), 6)) for c in clips]


# --- Pure functions -----------------------------------------------------------------


def test_overlap_trims_partially_covered_clips():
    old = clip(0, 4, cid="old")
    new = clip(3, 4, cid="new")
    result = edits.resolve_overlaps([old, new], {"new"}, TEMPO)
    assert spans(result) == [(0, 3), (3, 7)]
    assert result[0].id == "old"


def test_overlap_trims_start_and_shifts_offset():
    old = clip(2, 4, cid="old")
    new = clip(0, 3, cid="new")
    result = edits.resolve_overlaps([old, new], {"new"}, TEMPO)
    assert spans(result) == [(0, 3), (3, 6)]
    trimmed = result[1]
    assert trimmed.offset_sec == pytest.approx(0.5)  # one beat of audio skipped


def test_overlap_splits_enclosing_clip():
    old = clip(0, 8, cid="old")
    new = clip(2, 2, cid="new")
    result = edits.resolve_overlaps([old, new], {"new"}, TEMPO)
    assert spans(result) == [(0, 2), (2, 4), (4, 8)]
    left, _, right = result
    assert left.id == "old" and right.id not in ("old", "new")
    assert right.offset_sec == pytest.approx(2.0)


def test_overlap_removes_covered_clip_and_keeps_neighbours():
    covered = clip(1, 1, cid="covered")
    touching = clip(4, 1, cid="touching")
    new = clip(0, 4, cid="new")
    result = edits.resolve_overlaps([covered, touching, new], {"new"}, TEMPO)
    assert [c.id for c in result] == ["new", "touching"]


def test_remove_range_keeps_start_and_end_of_spanning_clip():
    long = clip(0.0, 8.0, cid="long")
    inside = clip(10.0, 1.0)
    across = clip(11.5, 2.0, cid="across")
    after = edits.remove_range([long, inside, across], 2.0, 12.0, TEMPO)
    after = edits.remove_range(after, 3.0, 5.0, TEMPO)
    assert spans(after) == [(0.0, 2.0), (12.0, 13.5)]
    # A middle cut leaves both ends, each still playing its own part of the audio.
    [a, b] = edits.remove_range([long], 3.0, 5.0, TEMPO)
    assert spans([a, b]) == [(0.0, 3.0), (5.0, 8.0)]
    assert a.id == "long" and b.id != "long"
    assert b.offset_sec == pytest.approx(2.5)
    assert edits.remove_range([long], 9.0, 10.0, TEMPO) == [long]  # untouched


def test_split_clip():
    left, right = edits.split_clip(clip(0, 4, offset=1.0), 1.0, TEMPO)
    assert spans([left, right]) == [(0, 1), (1, 4)]
    assert right.offset_sec == pytest.approx(1.5)
    assert edits.split_clip(clip(0, 4), 4.0, TEMPO) is None
    assert edits.split_clip(clip(0, 4), -1.0, TEMPO) is None


def test_trim_start_keeps_audio_in_place():
    c = clip(4, 4, offset=1.0)
    t = edits.trim_start(c, 5.0, TEMPO)
    assert (t.start_beat, t.offset_sec, t.duration_sec) == (5.0, 1.5, 1.5)
    # Cannot reveal audio before the start of the file.
    t = edits.trim_start(c, 0.0, TEMPO)
    assert t.start_beat == pytest.approx(2.0) and t.offset_sec == 0.0


def test_trim_end_limited_by_source():
    c = clip(0, 2, offset=1.0, source=3.0)
    assert edits.trim_end(c, 1.0, TEMPO).duration_sec == pytest.approx(0.5)
    assert edits.trim_end(c, 100.0, TEMPO).duration_sec == pytest.approx(2.0)
    assert edits.trim_end(c, -5.0, TEMPO).duration_sec == pytest.approx(edits.MIN_CLIP_SEC)


# --- Editor + undo -------------------------------------------------------------------


@pytest.fixture
def editor(qapp):
    return ProjectEditor(Project(), QUndoStack())


@pytest.fixture
def qapp():
    from PySide6.QtWidgets import QApplication

    return QApplication.instance() or QApplication([])


def snapshot(project):
    return [(t.id, t.name, list(t.clips)) for t in project.tracks]


def test_add_clips_creates_track_and_places_sequentially(editor):
    refs = editor.add_clips(None, 2.0, [("C:/x/kick.wav", 1.0), ("C:/x/snare.wav", 0.5)])
    project = editor.project
    assert len(project.tracks) == 1 and project.tracks[0].name == "kick"
    assert spans(project.tracks[0].clips) == [(2, 4), (4, 5)]
    assert len(refs) == 2
    editor.undo_stack.undo()
    assert project.tracks == []


def test_move_clips_across_tracks_with_overlap(editor):
    t1 = editor.add_audio_track()
    t2 = editor.add_audio_track()
    [(_, a)] = editor.add_clips(t1.id, 0.0, [("a.wav", 2.0)])
    editor.add_clips(t2.id, 0.0, [("b.wav", 4.0)])
    before = snapshot(editor.project)

    [(dest, moved)] = editor.move_clips([(t1.id, a)], 2.0, track_delta=1)
    assert dest == t2.id
    assert editor.project.track(t1.id).clips == []
    assert spans(editor.project.track(t2.id).clips) == [(0, 2), (2, 6), (6, 8)]

    editor.undo_stack.undo()
    assert snapshot(editor.project) == before
    editor.undo_stack.redo()
    assert spans(editor.project.track(t2.id).clips) == [(0, 2), (2, 6), (6, 8)]


def test_move_is_clamped_to_timeline_and_tracks(editor):
    t1 = editor.add_audio_track()
    [(_, a)] = editor.add_clips(t1.id, 1.0, [("a.wav", 1.0)])
    [(dest, _)] = editor.move_clips([(t1.id, a)], -10.0, track_delta=5)
    assert dest == t1.id
    assert editor.project.track(t1.id).clips[0].start_beat == 0.0


def test_duplicate_places_copies_after_selection(editor):
    t = editor.add_audio_track()
    refs = editor.add_clips(t.id, 0.0, [("a.wav", 1.0), ("b.wav", 1.0)])
    new_refs = editor.duplicate_clips(refs)
    assert spans(editor.project.track(t.id).clips) == [(0, 2), (2, 4), (4, 6), (6, 8)]
    assert {cid for _, cid in new_refs}.isdisjoint({cid for _, cid in refs})


def test_split_and_delete(editor):
    t = editor.add_audio_track()
    refs = editor.add_clips(t.id, 0.0, [("a.wav", 2.0)])
    editor.split_clips(refs, 1.0)
    assert spans(editor.project.track(t.id).clips) == [(0, 1), (1, 4)]
    editor.delete_clips([(t.id, c.id) for c in editor.project.track(t.id).clips])
    assert editor.project.track(t.id).clips == []
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert spans(editor.project.track(t.id).clips) == [(0, 4)]


def test_track_param_changes_merge_during_a_gesture(editor):
    t = editor.add_audio_track()
    gesture = object()
    for db in (-1.0, -2.0, -3.0):
        editor.set_track_param(t.id, "volume_db", db, merge_key=gesture)
    assert editor.project.track(t.id).volume_db == -3.0
    editor.undo_stack.undo()
    assert editor.project.track(t.id).volume_db == 0.0
    editor.undo_stack.redo()
    editor.set_track_param(t.id, "volume_db", -6.0, merge_key=object())
    editor.undo_stack.undo()
    assert editor.project.track(t.id).volume_db == -3.0


def test_delete_track_undo_restores_everything(editor):
    t = editor.add_audio_track()
    editor.add_clips(t.id, 0.0, [("a.wav", 1.0)])
    editor.add_device(t.id, "utility")
    before = snapshot(editor.project)
    editor.delete_tracks([t.id])
    assert editor.project.tracks == []
    editor.undo_stack.undo()
    assert snapshot(editor.project) == before
    assert editor.project.track(t.id).devices[0].kind == "utility"


def test_faster_tempo_trims_clips_instead_of_overlapping(editor):
    p = editor.project
    track = editor.add_audio_track()
    editor.add_clips(track.id, 0.0, [("a.wav", 2.0), ("b.wav", 1.0)])  # 4 beats, then 2 at 120 BPM
    second = p.track(track.id).clips[1]
    assert second.start_beat == 4.0

    def ends():
        return [(round(c.start_beat, 6), round(c.end_beat(p.tempo), 6)) for c in p.track(track.id).clips]

    # Dragging the tempo up trims the first clip at the second one's start...
    drag = object()
    editor.set_tempo(150.0, drag)
    assert ends() == [(0.0, 4.0), (4.0, 6.5)]
    assert p.track(track.id).clips[0].duration_sec == pytest.approx(1.6)
    # ...and back down within the same drag restores it: steps fit from the start of the drag.
    editor.set_tempo(130.0, drag)
    editor.set_tempo(100.0, drag)
    assert p.track(track.id).clips[0].duration_sec == pytest.approx(2.0)
    editor.set_tempo(180.0, drag)
    assert ends() == [(0.0, 4.0), (4.0, 7.0)]
    # The whole drag is one undo step that restores tempo and clips.
    editor.undo_stack.undo()
    assert p.tempo == 120.0
    assert [c.duration_sec for c in p.track(track.id).clips] == [2.0, 1.0]
    editor.undo_stack.redo()
    assert p.tempo == 180.0 and ends()[0] == (0.0, 4.0)


def test_fit_to_tempo_leaves_non_overlapping_clips_alone():
    clips = [clip(0.0, 2.0), clip(4.0, 2.0)]
    assert edits.fit_to_tempo(clips, TEMPO) is clips
    for factor, end in ((2, 8.0), (3, 10.0)):  # the first clip would grow to 4 or 6 beats
        fitted = edits.fit_to_tempo(clips, TEMPO * factor)
        assert [(c.start_beat, round(c.end_beat(TEMPO * factor), 6)) for c in fitted] == [(0.0, 4.0), (4.0, end)]


def test_settings(editor):
    editor.set_tempo(140.0)
    editor.set_loop(True, 4.0, 8.0)
    assert (editor.project.tempo, editor.project.loop_enabled, editor.project.loop_start) == (140.0, True, 4.0)
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert (editor.project.tempo, editor.project.loop_enabled) == (120.0, False)


# --- Warping --------------------------------------------------------------------------


def warped(start, beats, segment_bpm=60.0, cid=None, source=100.0):
    """A warped clip `beats` long; at 60 BPM a beat is one second of audio."""
    return Clip(id=cid or new_id(), path="a.wav", name="a", start_beat=start,
                duration_sec=beats * 60.0 / segment_bpm, source_duration_sec=source,
                warp=True, segment_bpm=segment_bpm)


def test_warped_clip_length_is_fixed_in_beats():
    c = warped(0.0, 4.0)
    for tempo in (60.0, 120.0, 175.0):
        assert c.length_beats(tempo) == pytest.approx(4.0)
    # Unwarped (or warped without a segment BPM) it follows the tempo again.
    assert replace(c, warp=False).length_beats(120.0) == pytest.approx(8.0)
    assert replace(c, segment_bpm=0.0).length_beats(120.0) == pytest.approx(8.0)


def test_editing_warped_clips_measures_source_at_segment_bpm():
    c = warped(2.0, 4.0, cid="w")  # 4 s of audio over beats 2..6
    left, right = edits.split_clip(c, 3.0, TEMPO)
    assert (left.duration_sec, right.offset_sec, right.duration_sec) == pytest.approx((1.0, 1.0, 3.0))
    trimmed = edits.trim_start(replace(c, offset_sec=5.0), 1.0, TEMPO)
    assert (trimmed.start_beat, trimmed.offset_sec, trimmed.duration_sec) == pytest.approx((1.0, 4.0, 5.0))
    assert edits.trim_end(c, 5.0, TEMPO).duration_sec == pytest.approx(3.0)
    pieces = edits.remove_range([c], 3.0, 4.0, TEMPO)
    assert spans(pieces) == [(2.0, 3.0), (4.0, 6.0)]
    assert pieces[1].offset_sec == pytest.approx(2.0)


def test_tempo_change_leaves_warped_clips_alone(editor):
    t = editor.add_audio_track()
    editor.project.set_clips(t.id, [warped(0.0, 4.0, cid="w"), clip(4.0, 2.0, cid="u")])
    editor.set_tempo(60.0)  # the unwarped clip grows, the warped one doesn't
    w, u = editor.project.track(t.id).clips
    assert w.length_beats(60.0) == pytest.approx(4.0)
    assert u.length_beats(60.0) == pytest.approx(1.0)


def test_segment_bpm_changes_trim_at_the_next_clip_and_drags_recover(editor):
    t = editor.add_audio_track()
    original = [warped(0.0, 2.0, cid="w"), clip(3.0, 2.0, cid="next")]
    editor.project.set_clips(t.id, original)
    refs = [(t.id, "w")]
    key = object()
    depth = editor.undo_stack.count()
    # A higher segment BPM plays the audio slower, so the clip grows (2 s at
    # 180 BPM is 6 beats) until it meets the next clip, which keeps its place.
    editor.update_clips(refs, lambda c: replace(c, segment_bpm=180.0), "Change Segment BPM", key)
    w, nxt = editor.project.track(t.id).clips
    assert w.end_beat(TEMPO) == pytest.approx(3.0)
    assert w.duration_sec == pytest.approx(1.0)  # 3 beats at 180 BPM
    assert nxt == original[1]
    # ...and dragging back in the same gesture restores it untrimmed.
    editor.update_clips(refs, lambda c: replace(c, segment_bpm=60.0), "Change Segment BPM", key)
    assert editor.project.track(t.id).clips == original
    assert editor.undo_stack.count() == depth + 1
    # Separate edits don't merge: a lower BPM (faster) leaves the trimmed clip short.
    editor.update_clips(refs, lambda c: replace(c, segment_bpm=180.0), "Change Segment BPM")
    editor.update_clips(refs, lambda c: replace(c, segment_bpm=90.0), "Change Segment BPM")
    assert editor.project.clip(t.id, "w").length_beats(TEMPO) == pytest.approx(1.5)
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert editor.project.track(t.id).clips == original


def test_turning_warp_off_trims_like_a_tempo_change(editor):
    t = editor.add_audio_track()
    editor.project.set_clips(t.id, [warped(0.0, 2.0, cid="w"), clip(3.0, 2.0, cid="next")])
    # Unwarped, 2 s of audio is 4 beats at 120 BPM: cut where the next clip starts.
    editor.update_clips([(t.id, "w")], lambda c: replace(c, warp=False), "Toggle Warp")
    assert editor.project.clip(t.id, "w").end_beat(TEMPO) == pytest.approx(3.0)


def test_track_input_and_monitoring_are_undoable_arming_is_not(editor):
    a, b = editor.add_audio_track(), editor.add_audio_track()
    midi = editor.add_midi_track()
    steps = editor.undo_stack.count()
    editor.set_track_input(a.id, (2, 3))
    editor.set_track_monitor(a.id, "in")
    assert (a.input, a.monitor) == ((2, 3), "in") and editor.undo_stack.count() == steps + 2
    with pytest.raises(ValueError):
        editor.set_track_monitor(a.id, "loud")
    with pytest.raises(ValueError):
        editor.set_track_input(a.id, (0, 1, 2))
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert (a.input, a.monitor) == ((), "auto")

    editor.arm_tracks([a.id], True)
    editor.arm_tracks([b.id, midi.id], True, exclusive=True)  # the others are disarmed
    assert (a.armed, b.armed, midi.armed) == (False, True, True)
    editor.arm_tracks([a.id], True, exclusive=False)
    assert (a.armed, b.armed, midi.armed) == (True, True, True)
    editor.arm_tracks([a.id, b.id, midi.id], False)
    assert (a.armed, b.armed, midi.armed) == (False, False, False)
    assert editor.undo_stack.index() == steps and editor.undo_stack.count() == steps + 2  # still redoable


def test_recorded_takes_become_clips_replacing_what_was_under_them(editor):
    from gilstudio.model.editor import RecordedTake

    project = editor.project
    track = editor.add_audio_track()
    editor.add_clips(track.id, 0.0, [("C:/x/loop.wav", 4.0)])  # 8 beats at 120 BPM
    before = list(track.clips)
    takes = [RecordedTake(track.id, "C:/rec/take 1.wav", start_sec=1.0, duration_sec=1.5),  # beats 2..5
             RecordedTake("gone", "C:/rec/other.wav", 0.0, 1.0)]  # its track was deleted meanwhile
    refs = editor.add_recordings(takes)
    assert editor.undo_stack.undoText() == "Record"
    assert len(refs) == 1 and refs[0][0] == track.id
    take = project.clip(*refs[0])
    assert (take.path, take.start_beat, take.duration_sec, take.offset_sec) == ("C:/rec/take 1.wav", 2.0, 1.5, 0.0)
    assert spans(track.clips) == [(0, 2), (2, 5), (5, 8)]  # overdubbed: the take wins
    editor.undo_stack.undo()  # the take goes, and with it the project's reference to its file
    assert track.clips == before
    assert not any(c.path == "C:/rec/take 1.wav" for t in project.tracks for c in t.clips)
    editor.undo_stack.redo()
    assert spans(track.clips) == [(0, 2), (2, 5), (5, 8)]

    # A take that began before the timeline (recorded from the start, heard late) starts at 0.
    refs = editor.add_recordings([RecordedTake(track.id, "C:/rec/take 2.wav", -0.25, 1.0)])
    take = project.clip(*refs[0])
    assert (take.start_beat, take.offset_sec, take.duration_sec, take.source_duration_sec) == (0.0, 0.25, 0.75, 1.0)
    assert editor.add_recordings([RecordedTake(track.id, "C:/rec/empty.wav", -2.0, 1.0)]) == []


def test_copy_paste_takes_just_the_range_and_pastes_at_a_track(editor):
    t1 = editor.add_audio_track()
    t2 = editor.add_audio_track()
    t3 = editor.add_audio_track()
    editor.add_clips(t1.id, 0.0, [("a.wav", 2.0)])  # beats 0-4
    editor.add_clips(t2.id, 2.0, [("b.wav", 1.0)])  # beats 2-4
    content = editor.copy_range(1.0, 3.0, [t1.id, t2.id])
    assert content.length == 2.0 and [c.row for c in content.tracks] == [0, 1]
    assert spans(content.tracks[0].clips) == [(0, 2)] and spans(content.tracks[1].clips) == [(1, 2)]
    assert editor.copy_range(10.0, 12.0, [t1.id]) is None  # nothing there

    # Onto the second track: the content's second row lands on the third.
    area = editor.paste(content, 8.0, t2.id)
    assert area == (8.0, 10.0, [t2.id, t3.id])
    assert spans(editor.project.track(t2.id).clips) == [(2, 4), (8, 10)]
    assert spans(editor.project.track(t3.id).clips) == [(9, 10)]
    # Pasting again makes new clips; one undo step each.
    editor.paste(content, 10.0, t2.id)
    ids = [c.id for c in editor.project.track(t2.id).clips]
    assert len(set(ids)) == 3
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert spans(editor.project.track(t3.id).clips) == []


def test_paste_replaces_what_is_there_and_cut_takes_it_out(editor):
    t = editor.add_audio_track()
    editor.add_clips(t.id, 0.0, [("a.wav", 4.0)])  # beats 0-8
    content = editor.cut_range(2.0, 4.0, [t.id])
    assert spans(editor.project.track(t.id).clips) == [(0, 2), (4, 8)]
    assert editor.undo_stack.undoText() == "Cut"
    editor.paste(content, 5.0, t.id)
    assert spans(editor.project.track(t.id).clips) == [(0, 2), (4, 5), (5, 7), (7, 8)]
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert spans(editor.project.track(t.id).clips) == [(0, 8)]


def test_paste_onto_another_kind_of_track_goes_back_where_it_came_from(editor):
    audio = editor.add_audio_track()
    midi = editor.add_midi_track()
    editor.add_clips(audio.id, 0.0, [("a.wav", 1.0)])
    content = editor.copy_range(0.0, 2.0, [audio.id])
    assert editor.paste_targets(content, midi.id) == [audio.id]
    assert editor.paste(content, 4.0, midi.id) == (4.0, 6.0, [audio.id])
    assert editor.project.track(midi.id).clips == []
    editor.delete_tracks([audio.id])
    assert editor.paste(content, 4.0, midi.id) is None


def test_copied_automation_comes_along_unless_locked(editor):
    from gilstudio.model.automation import MIXER_VOLUME, AutomationPoint

    t1 = editor.add_audio_track()
    t2 = editor.add_audio_track()
    editor.add_clips(t1.id, 0.0, [("a.wav", 1.0)])
    editor.set_envelope(t1.id, MIXER_VOLUME, (AutomationPoint(0.0, 0.2), AutomationPoint(2.0, 0.6)))
    content = editor.copy_range(0.0, 2.0, [t1.id])
    editor.paste(content, 4.0, t2.id)
    assert [(p.beat, p.value) for p in editor.project.envelope(t2.id, MIXER_VOLUME)] == [(4.0, 0.2), (6.0, 0.6)]

    editor.set_automation_locked(True)
    assert editor.copy_range(0.0, 2.0, [t1.id]).tracks[0].automation == ()
    editor.cut_range(0.0, 2.0, [t1.id])
    assert len(editor.project.envelope(t1.id, MIXER_VOLUME)) == 2  # locked: it stays
