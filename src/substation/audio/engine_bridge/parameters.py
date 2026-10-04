"""Automation and parameters: envelopes pushed to the engine, overriding them
by hand and re-enabling them, and every kind of parameter described to the UI
as ParamSpecs, with its value now."""

from __future__ import annotations

from ... import _engine as ge
from ...model import automation
from ...model.automation import MASTER, MIXER_PAN, MIXER_VOLUME
from ...model.devices import device_name
from ...model.params import ParamSpec, chain_specs, format_value, mixer_specs
from ...model.project import Device, Send, find_device, iter_chains, iter_devices


class ParameterSync:
    """Automation, and parameters described to the UI. Part of EngineBridge (engine_bridge/__init__.py)."""

    def _on_automation_changed(self, owner: str, key: str) -> None:
        if automation.key_send(key) is not None:
            self._push_sends(owner)  # a send automated before it was set is made
        self._push_automation(owner)

    def _push_automation(self, owner: str) -> None:
        """The owner's envelopes to the engine, but those overridden. Targets whose
        envelope no longer plays go back to their own value."""
        engine_id = self._track_ids.get(owner)
        if engine_id is None:
            return
        lanes, playing = [], set()
        for key, points in self.project.automation(owner).items():
            if not points or (owner, key) in self._overridden:
                continue
            lane = self._engine_lane(owner, key, points)
            if lane is not None:
                lanes.append(lane)
                playing.add(key)
        self.engine.set_track_automation(engine_id, lanes)
        stopped = self._automating.get(owner, set()) - playing
        self._automating[owner] = playing
        for key in stopped:
            self._push_own_value(owner, key)
        self.automation_state_changed.emit(owner)

    def _engine_lane(self, owner: str, key: str, points) -> ge.AutomationLane | None:
        try:
            target = automation.parse_key(key)
        except ValueError:
            return None
        engine_points = [ge.AutomationPoint(p.beat, p.value, p.curve) for p in points]
        if target[0] == "mixer":
            return ge.AutomationLane(0, target[1], engine_points)
        if target[0] == "send":
            return_id = self._track_ids.get(target[1])
            return None if return_id is None else ge.AutomationLane(0, f"send:{return_id}", engine_points)
        processor_id = self.engine_device_id(owner, target[1])
        if (control := automation.key_chain_control(key)) is not None:  # a rack chain's fader
            chain = self.engine_chain_id(control[0])
            if processor_id is None or chain is None:
                return None
            return ge.AutomationLane(processor_id, f"chain:{chain}:{control[1]}", engine_points)
        return None if processor_id is None else ge.AutomationLane(processor_id, target[2], engine_points)

    def _push_own_value(self, owner: str, key: str) -> None:
        """A target no longer automated: back to the value it has in the model."""
        if key in automation.MIXER_KEYS:
            if self.project.has_owner(owner):
                self._push_mixer(owner)
            return
        if automation.key_send(key) is not None or automation.key_chain(key) is not None:
            return  # the engine kept the send's (or the chain's fader's) own level
        device_id = automation.key_device(key)
        if self.project.has_device(owner, device_id):
            self._push_device_param(owner, device_id, automation.parse_key(key)[2])

    def is_automated(self, owner: str, key: str) -> bool:
        """Whether the engine plays this target's envelope (it has one, not overridden)."""
        return key in self._automating.get(owner, ())

    def is_overridden(self, owner: str, key: str) -> bool:
        return (owner, key) in self._overridden

    @property
    def has_overrides(self) -> bool:
        return bool(self._overridden)

    def override_automation(self, owner: str, key: str) -> None:
        """The target was changed by hand: its automation stops until re-enabled."""
        if self.is_automated(owner, key):
            self._overridden.add((owner, key))
            self._push_automation(owner)

    def re_enable_automation(self, owner: str | None = None) -> None:
        """Automation plays again where it was overridden (everywhere, or for one owner)."""
        owners = {o for o, _ in self._overridden if owner is None or o == owner}
        self._overridden = {(o, k) for o, k in self._overridden if o not in owners}
        for o in owners:
            self._push_automation(o)

    def param_infos(self, processor_id: int) -> list:
        infos = self._param_infos.get(processor_id)
        if infos is None:
            infos = self._param_infos[processor_id] = list(self.engine.processor_params(processor_id))
        return infos

    def plugin_param_text(self, processor_id: int, index: int, value: float) -> str:
        """A plug-in's text for a value of its parameter, with its unit."""
        info = self.param_infos(processor_id)[index]
        text = self.engine.processor_param_text(processor_id, index, value)
        if not text:
            return f"{value:.2f}"
        return text if not info.unit or info.unit in text else f"{text} {info.unit}"

    def device_param_specs(self, track_id: str, device: Device) -> list[ParamSpec]:
        """The parameters of a device that can be automated (none if it isn't
        loaded); a rack's: its chains' faders."""
        if device.is_rack:
            return chain_specs(device.id, [(c.id, c.name) for c in device.chains], device_name(device))
        processor_id = self.engine_device_id(track_id, device.id)
        if processor_id is None:
            return []
        specs = self._param_specs.get(processor_id)
        if specs is None:
            name = device_name(device)
            is_plugin = processor_id in self._plugin_ids
            specs = []
            for index, info in enumerate(self.param_infos(processor_id)):
                if not info.automatable or info.hidden or info.read_only:
                    continue
                text = (lambda v, i=index: self.plugin_param_text(processor_id, i, v)) if is_plugin else None
                specs.append(ParamSpec.from_info(info, automation.device_key(device.id, info.id), name, text))
            self._param_specs[processor_id] = specs
        return specs

    def mixer_specs(self, owner: str) -> list[ParamSpec]:
        """An owner's mixer controls: volume, pan, and its sends (to the returns it can send to)."""
        sends = tuple((r.id, self.project.return_letter(r.id)) for r in self.project.send_targets(owner)) \
            if self.project.has_owner(owner) else ()
        return mixer_specs(master=owner == MASTER, sends=sends)

    def param_groups(self, owner: str) -> list[tuple[str, str, list[ParamSpec]]]:
        """What an owner has that can be automated, as (group id, name, specs): its
        mixer ("mixer", with its sends), then each device (by id)."""
        groups = [("mixer", "Mixer", self.mixer_specs(owner))]
        for device in iter_devices(self.project.track(owner).devices):
            groups.append((device.id, device_name(device), self.device_param_specs(owner, device)))
        return groups

    def can_automate(self, owner: str, key: str) -> bool:
        return any(spec.key == key for _, _, specs in self.param_groups(owner) for spec in specs)

    def param_spec(self, owner: str, key: str) -> ParamSpec | None:
        """A target's description; None if it doesn't exist (a device that is gone)."""
        if key in automation.MIXER_KEYS:  # every owner has these: no need to work out its sends
            return mixer_specs(master=owner == MASTER)[automation.MIXER_KEYS.index(key)]
        if automation.is_mixer_key(key):
            return next((s for s in self.mixer_specs(owner) if s.key == key), None)  # (a return that is gone)
        if not self.project.has_owner(owner):
            return None
        device_id = automation.key_device(key)
        device = find_device(self.project.track(owner).devices, device_id)
        if device is None:
            return None
        spec = next((s for s in self.device_param_specs(owner, device) if s.key == key), None)
        if spec is None:  # not loaded: its values are still worth showing
            param_id = automation.parse_key(key)[2]
            spec = ParamSpec(key, param_id, device_name(device), text=lambda v: format_value(v, ""))
        return spec

    def own_value(self, owner: str, key: str) -> float | None:
        """A target's value as set by hand (plain; None if not known)."""
        if not self.project.has_owner(owner):
            return None
        track = self.project.track(owner)
        if key == MIXER_VOLUME:
            return track.volume_db
        if key == MIXER_PAN:
            return track.pan
        if (return_id := automation.key_send(key)) is not None:
            return track.sends.get(return_id, Send()).level_db
        if (control := automation.key_chain_control(key)) is not None:
            chain = next((c for _, c in iter_chains(track.devices) if c.id == control[0]), None)
            return None if chain is None else chain.volume_db if control[1] == automation.CHAIN_VOLUME else chain.pan
        target = automation.parse_key(key)
        device = find_device(track.devices, target[1])
        if device is None:
            return None
        processor_id = self.engine_device_id(owner, device.id)
        if processor_id in self._plugin_ids:  # its values live in the plug-in
            index = self.engine.processor_param_index(processor_id, target[2])
            if index >= 0:
                return self.engine.processor_param(processor_id, index)
        value = device.params.get(target[2])
        if value is None and processor_id is not None:
            index = self.engine.processor_param_index(processor_id, target[2])
            if index >= 0:
                value = self.param_infos(processor_id)[index].default_value
        return value

    def current_value(self, owner: str, key: str, beat: float | None = None) -> float | None:
        """What a target is at `beat` (default: the playhead): its envelope's value
        while that plays, else its own (plain; None if not known)."""
        spec = self.param_spec(owner, key)
        if spec is not None and self.is_automated(owner, key):
            value = automation.value_at(self.project.envelope(owner, key), self.position if beat is None else beat)
            if value is not None:
                return spec.from_normalized(spec.quantize(value))
        return self.own_value(owner, key)
