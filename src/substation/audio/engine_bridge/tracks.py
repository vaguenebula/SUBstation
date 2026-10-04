"""Model -> engine for tracks: engine tracks for the project's tracks (and
groups, returns, the master), their mixers, where their outputs go, their
sends, their clips and notes, and the project's tempo, time signature and loop."""

from __future__ import annotations

from ... import _engine as ge
from ...model import automation
from ...model.automation import MASTER, MIXER_PAN, MIXER_VOLUME
from ...model.project import WARP_MODES, Clip, Send, Track
from ...model.timebase import db_to_gain
from ..settings import audio_threads
from .sources import _key

_WARP_MODES = {name: ge.WarpMode(index) for index, name in enumerate(WARP_MODES)}


def clip_desc(clip: Clip) -> ge.ClipDesc:
    """The engine's view of a clip. Positions stay in beats and seconds; the
    engine converts them to samples at the current tempo and sample rate."""
    return ge.ClipDesc(
        clip.path, clip.start_beat, clip.duration_sec, clip.offset_sec, db_to_gain(clip.gain_db),
        pan=clip.pan,
        warp=clip.is_warped,
        segment_bpm=clip.segment_bpm,
        warp_mode=_WARP_MODES.get(clip.warp_mode, ge.WarpMode.STANDARD),
        transpose=clip.transpose + clip.detune / 100.0,
        id=clip.id,
    )


def note_descs(track: Track) -> list[ge.NoteDesc]:
    """The notes a MIDI track plays, from all of its clips, in timeline beats."""
    return clip_note_descs(track.clips)


def clip_note_descs(clips) -> list[ge.NoteDesc]:
    """The notes these MIDI clips play, in timeline beats."""
    return [ge.NoteDesc(start, end - start, note.pitch, note.velocity)
            for clip in clips for start, end, note in clip.played_notes()]


class TrackSync:
    """Engine tracks, mixers, outputs, sends and clips. Part of EngineBridge (engine_bridge/__init__.py)."""

    def _on_reset(self) -> None:
        self._remove_engine_tracks()
        self._devices.clear()
        self._pids.clear()
        self._where.clear()
        self._chain_owner = {MASTER: MASTER}
        self._rack_of_chain.clear()
        self._rack_orders.clear()
        self._chain_mixer.clear()
        self.chain_meters.clear()
        self._enabled.clear()
        self._plugin_ids.clear()
        self._plugin_states.clear()
        self._param_ids.clear()
        self._param_infos.clear()
        self._param_specs.clear()
        self._automating.clear()
        self._overridden.clear()
        self._mixer.clear()
        self._inputs.clear()
        self._outputs.clear()
        self._sends.clear()
        self._send_levels.clear()
        self._frozen.clear()
        self._sidechains.clear()
        self.plugin_errors.clear()
        self.meters.clear()
        self._editors_wanted.clear()
        self._hidden_editors.clear()
        self._previewing.clear()
        self._reversed.clear()
        self._stop_loading_plugins()
        self._deferring = True  # its plug-ins load after it shows (loading.py)
        try:
            for track in self.project.all_tracks():
                self._add_engine_track(track)
        finally:
            self._deferring = False
        self._push_outputs()
        self._push_all_sends()  # (into returns added after the tracks sending to them)
        self._push_all_inputs()  # (from tracks added after the tracks taking them)
        self._push_sidechains()
        self._push_settings()
        # Forget decoded audio the new project doesn't use.
        used = {_key(c.path) for t in self.project.tracks if not t.is_midi for c in t.clips}
        used |= {_key(t.frozen.path) for t in self.project.all_tracks() if t.frozen is not None}
        self._sources = {k: s for k, s in self._sources.items() if k in used}
        self.engine.release_unused_sources()
        self._start_loading_plugins()

    def _remove_engine_tracks(self) -> None:
        """Every track goes from the engine, with its devices; the master stays, without its devices."""
        self._forget_chain_devices(MASTER, remove=True)
        for track_id, engine_id in list(self._track_ids.items()):
            if track_id != MASTER:
                self._forget_chain_devices(track_id, remove=False)
                self.engine.remove_track(engine_id)
                del self._track_ids[track_id]
                self._drop_chain(track_id)
        self._chain_owner.setdefault(MASTER, MASTER)

    def _add_engine_track(self, track: Track) -> None:
        if not track.is_master:  # the engine always has the master
            self._track_ids[track.id] = self.engine.add_track()
            self._chains[track.id] = self.engine.track_chain(self._track_ids[track.id])
            self._chain_owner[track.id] = track.id
        self._push_mixer(track.id)
        self._push_input(track.id)
        self._push_frozen(track.id)
        self._push_clips(track.id)
        self._sync_devices(track.id)
        self._push_automation(track.id)
        if not track.is_master:  # into its group, and what is in it (back) into it
            self._push_outputs()
        if track.is_return:  # its sends, and the sends into it
            self._push_all_sends()
        elif not track.is_master:
            self._push_sends(track.id)
        if not track.is_master:  # the inputs taken from it (back)
            self._push_all_inputs()
        self._push_sidechains()  # its devices', and those it is the source of

    def _on_track_removed(self, track_id: str, _index: int) -> None:
        self._forget_chain_devices(track_id, remove=False)
        engine_id = self._track_ids.pop(track_id, None)
        self._drop_chain(track_id)
        if engine_id is not None:
            self.engine.remove_track(engine_id)  # also removes its devices
        self.meters.pop(track_id, None)
        self._automating.pop(track_id, None)
        self._mixer.pop(track_id, None)
        self._inputs.pop(track_id, None)
        self._outputs.pop(track_id, None)
        self._sends.pop(track_id, None)
        self._send_levels.pop(track_id, None)
        self._frozen.discard(track_id)
        # What went into it goes to the engine's master now, and the sends into it
        # and the inputs from it are gone (the model has its say next).
        self._outputs = {t: ge.MASTER if out == engine_id else out for t, out in self._outputs.items()}
        for sends in self._sends.values():
            sends.pop(engine_id, None)
        for track, state in list(self._inputs.items()):
            if state[1] == engine_id:
                self._inputs[track] = (state[0], None, *state[2:])
        self._sidechains = {p: state for p, state in self._sidechains.items() if state[0] != engine_id}
        self._overridden = {(o, k) for o, k in self._overridden if o != track_id}

    def _on_track_changed(self, track_id: str) -> None:
        if track_id not in self._track_ids:
            return
        track = self.project.track(track_id)
        self._override_changed_mixer(track_id, track.volume_db, track.pan)
        self._override_changed_sends(track)
        self._push_mixer(track_id)
        self._push_input(track_id)
        self._push_sends(track_id)
        self._push_sidechains()  # (a route that stood in a sidechain's way may have gone)

    def _override_changed_sends(self, track: Track) -> None:
        """A send level changed by hand while automated: its automation stops."""
        old = self._send_levels.get(track.id)
        new = {return_id: send.level_db for return_id, send in track.sends.items()}
        self._send_levels[track.id] = new
        if old is None:
            return
        for return_id, level in new.items():
            if old.get(return_id) != level:
                self.override_automation(track.id, automation.send_key(return_id))

    def _override_changed_mixer(self, owner: str, volume_db: float, pan: float) -> None:
        """A mixer control changed by hand while automated: its automation stops."""
        old = self._mixer.get(owner)
        if old is None:
            return
        if volume_db != old[0]:
            self.override_automation(owner, MIXER_VOLUME)
        if pan != old[1]:
            self.override_automation(owner, MIXER_PAN)

    def _push_mixer(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None:
            return
        track = self.project.track(track_id)
        self.engine.set_track_gain(engine_id, db_to_gain(track.volume_db))
        self.engine.set_track_pan(engine_id, track.pan)
        if not track.is_master:
            self.engine.set_track_mute(engine_id, track.mute)
            self.engine.set_track_solo(engine_id, track.solo)
        self._mixer[track_id] = (track.volume_db, track.pan)

    def _push_outputs(self) -> None:
        """Every track's output into its group's engine track (or the master).
        Changed routes go to the master first, so that no step closes a cycle
        (a group moving into what was in it)."""
        wanted = {}
        for track in self.project.tracks:
            if track.id in self._track_ids:
                wanted[track.id] = self._track_ids.get(track.parent, ge.MASTER) if track.parent else ge.MASTER
        changed = {t: out for t, out in wanted.items() if self._outputs.get(t, ge.MASTER) != out}
        for track_id in changed:
            if self._outputs.get(track_id, ge.MASTER) != ge.MASTER:
                self.engine.set_track_output(self._track_ids[track_id], ge.MASTER)
                self._outputs[track_id] = ge.MASTER
        for track_id, out in changed.items():
            if out != ge.MASTER:
                self.engine.set_track_output(self._track_ids[track_id], out)
            self._outputs[track_id] = out
        if changed:
            self._push_sidechains()

    def _wanted_sends(self, track: Track) -> dict[int, tuple[float, bool]]:
        """The engine sends a track should have: its sends, and silent ones for
        those automated without having been set (so that the automation plays)."""
        sends = dict(track.sends)
        for key in track.automation:
            return_id = automation.key_send(key)
            if return_id is not None and return_id not in sends and self.project.has_return(return_id) \
                    and not self.project.would_cycle(track.id, return_id):
                sends[return_id] = Send()
        return {self._track_ids[r]: (db_to_gain(send.level_db), send.pre_fader) for r, send in sends.items()
                if r in self._track_ids and self.project.has_return(r)}

    def _push_sends(self, track_id: str) -> None:
        """A track's sends to the engine: those going away first (no step closes a cycle)."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        wanted = self._wanted_sends(self.project.track(track_id))
        current = self._sends.setdefault(track_id, {})
        for return_engine_id in [r for r in current if r not in wanted]:
            self.engine.remove_track_send(engine_id, return_engine_id)
            del current[return_engine_id]
        for return_engine_id, (gain, pre_fader) in wanted.items():
            if current.get(return_engine_id) == (gain, pre_fader):
                continue
            try:
                self.engine.set_track_send(engine_id, return_engine_id, gain, pre_fader)
            except ValueError:
                continue  # a cycle with a send another track hasn't given up yet: it comes with that track's turn
            current[return_engine_id] = (gain, pre_fader)
        self._send_levels.setdefault(track_id, {r: s.level_db for r, s in self.project.track(track_id).sends.items()})

    def _push_all_sends(self) -> None:
        for track in self.project.senders():
            self._push_sends(track.id)

    def _push_clips(self, track_id: str) -> None:
        """A track's clips (a MIDI track's notes) to the engine; a frozen track's
        frozen audio instead."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        track = self.project.track(track_id)
        if track.frozen is not None:
            clip = track.frozen.clip(track_id, track.name)
            self.request_source(clip.path)
            if track.is_midi:
                self.engine.set_track_notes(engine_id, [])
            self.engine.set_track_clips(engine_id, [clip_desc(clip)])
            return
        if track.is_midi:
            self.engine.set_track_notes(engine_id, note_descs(track))
            return
        for clip in track.clips:
            self.request_source(clip.path)
        self.engine.set_track_clips(engine_id, [clip_desc(c) for c in track.clips])

    def preview_clips(self, clips_by_track: dict[str, list]) -> None:
        """Play these clips on these tracks instead of the model's, until
        end_clip_preview() (or the next change to their clips): what a drag would
        make of them, heard while it goes on. Frozen tracks play on as they are."""
        for track_id, clips in clips_by_track.items():
            engine_id = self._track_ids.get(track_id)
            if engine_id is None or not self.project.has_track(track_id) or self.project.is_frozen(track_id):
                continue
            self._previewing.add(track_id)
            if self.project.track(track_id).is_midi:
                self.engine.set_track_notes(engine_id, clip_note_descs(clips))
            else:
                self.engine.set_track_clips(engine_id, [clip_desc(c) for c in clips])
        # Tracks previewed before but not now play their own clips again.
        for track_id in [t for t in self._previewing if t not in clips_by_track]:
            self._previewing.discard(track_id)
            if self.project.has_track(track_id):
                self._push_clips(track_id)

    def end_clip_preview(self) -> None:
        """The previewed tracks play the model's clips again."""
        previewed, self._previewing = self._previewing, set()
        for track_id in previewed:
            if self.project.has_track(track_id):
                self._push_clips(track_id)

    def _push_settings(self) -> None:
        p = self.project
        self.engine.tempo = p.tempo
        self.engine.set_time_signature(p.time_signature.numerator, p.time_signature.denominator)
        self.engine.set_loop(p.loop_enabled, p.loop_start, p.loop_end)

    def apply_audio_threads(self) -> None:
        """Renders on as many threads as the preferences say (the engine's default unless chosen)."""
        self.engine.audio_threads = audio_threads() or ge.Engine.default_audio_threads()
