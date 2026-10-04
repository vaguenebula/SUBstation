"""Group tracks in the window: Ctrl+G, folding, the group's header and summary
lane, dragging headers into and out of groups, and the device view."""

from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtTest import QTest

from substation.model.project import Clip


def press_key(window, key, modifiers=Qt.KeyboardModifier.NoModifier) -> None:
    QTest.keyClick(window.arrangement.lanes, key, modifiers)
    QTest.qWait(10)


def drag_header(window, header, end_y: int) -> None:
    """Drag a track's header (by its name) to `end_y` in the header column."""
    column = window.arrangement.headers
    start = QPoint(header.width() // 2, 8)
    end = header.mapFrom(column, QPoint(header.width() // 2, end_y))
    QTest.mousePress(header, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(header, start + QPoint(0, 12))
    QTest.mouseMove(header, end)
    QTest.mouseRelease(header, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)
    QTest.qWait(10)


def make_tracks(window, count=3):
    editor = window.editor
    ids = [editor.add_audio_track(name=name).id for name in "ABC"[:count]]
    for i, track_id in enumerate(ids):
        editor._commit("Add", {track_id: [Clip(id=f"c{i}", path="missing.wav", name="x", start_beat=i * 4.0,
                                               duration_sec=2.0, source_duration_sec=2.0)]})
    QTest.qWait(10)
    return ids


def test_ctrl_g_groups_the_selected_tracks_and_folding_hides_them(window):
    a, b, c = make_tracks(window)
    selection = window.selection
    headers = window.arrangement.headers.headers
    selection.select_track(a, focus_track=True)
    selection.select_track(b, focus_track=True, mode="toggle")
    window.activateWindow()
    press_key(window, Qt.Key.Key_G, Qt.KeyboardModifier.ControlModifier)
    project = window.project
    group = project.tracks[0]
    assert group.is_group and [t.parent for t in project.tracks] == [None, group.id, group.id, None]
    assert selection.track_id == group.id
    # The group's header: no arm or input; its tracks indented under it.
    assert headers[group.id].arm.isHidden() and headers[group.id].input.isHidden()
    assert headers[a].indent > headers[group.id].indent
    window.grab()  # the summary lane paints

    # Folding hides its tracks; the tracks below move up.
    top_of_c = window.arrangement.layout_model.row_for(c).top
    QTest.mouseClick(headers[group.id], Qt.MouseButton.LeftButton, pos=headers[group.id]._fold_rect().center())
    assert project.track(group.id).folded
    rows = window.arrangement.layout_model
    assert rows.row_for(a).hidden and rows.row_for(b).hidden and not headers[a].isVisible()
    assert rows.row_for(c).top < top_of_c
    assert window.undo_stack.undoText() == "Group Tracks"  # folding isn't undone
    window.grab()

    # Clicking in the lanes where the hidden tracks were finds the track below.
    lanes = window.arrangement.lanes
    y = rows.row_for(c).top + 5 - window.arrangement.view.scroll_y
    assert rows.rows[lanes.row_index_at(y)].track_id == c

    QTest.mouseClick(headers[group.id], Qt.MouseButton.LeftButton, pos=headers[group.id]._fold_rect().center())
    assert headers[a].isVisible()

    # Ctrl+Shift+G ungroups.
    selection.select_track(group.id, focus_track=True)
    press_key(window, Qt.Key.Key_G, Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier)
    assert [t.id for t in project.tracks] == [a, b, c] and all(t.parent is None for t in project.tracks)
    window.undo_stack.undo()
    assert project.tracks[0].is_group


def test_dragging_headers_moves_tracks_into_and_out_of_groups(window):
    a, b, c = make_tracks(window)
    project = window.project
    group = window.editor.group_tracks([a])
    QTest.qWait(10)
    column = window.arrangement.headers
    layout = window.arrangement.layout_model
    scroll = window.arrangement.view.scroll_y

    # Onto the middle of the group's header: into the group, last.
    row = layout.row_for(group.id)
    window.selection.select_track(c, focus_track=True)
    drag_header(window, column.headers[c], row.top + row.main_height // 2 - scroll)
    assert [t.id for t in project.tracks] == [group.id, a, c, b]
    assert project.track(c).parent == group.id
    assert window.undo_stack.undoText() == "Move Track"

    # Below the last track: out of the group, last.
    drag_header(window, column.headers[a], layout.total_height + 10 - scroll)
    assert [t.id for t in project.tracks] == [group.id, c, b, a] and project.track(a).parent is None

    # Dropped amid its own tracks, a group stays where it is.
    row = layout.row_for(c)
    drag_header(window, column.headers[group.id], row.top + row.main_height - 4 - scroll)
    assert [t.id for t in project.tracks] == [group.id, c, b, a]

    window.undo_stack.undo()
    window.undo_stack.undo()
    assert [t.id for t in project.tracks] == [group.id, a, b, c]


def test_a_groups_devices_show_in_the_device_view(window):
    a, = make_tracks(window, 1)
    group = window.editor.group_tracks([a])
    window.selection.select_track(group.id, focus_track=True)
    assert window.editor.add_device(group.id, "utility") is not None
    assert window.editor.add_device(group.id, "synth") is None  # instruments go on MIDI tracks
    QTest.qWait(10)
    assert window.bridge.engine_device_id(group.id, window.project.track(group.id).devices[0].id) is not None
    window.grab()


def test_folding_a_track_or_group_hides_its_automation(window):
    a, b = make_tracks(window, 2)
    group = window.editor.group_tracks([b])
    QTest.qWait(10)
    arrangement = window.arrangement
    headers = arrangement.headers.headers
    layout = arrangement.layout_model
    for track_id in (a, group.id):
        window.editor.show_automation(track_id)
        window.editor.add_automation_lane(track_id)
    QTest.qWait(10)
    assert layout.row_for(a).lanes and headers[a].automation.main.device.isVisible()
    height = layout.row_for(a).main_height

    # A folded track is its name row: no automation lanes or choosers, and it doesn't resize.
    QTest.mouseClick(headers[a], Qt.MouseButton.LeftButton, pos=headers[a]._fold_rect().center())
    row = layout.row_for(a)
    assert window.project.track(a).folded and row.folded and not row.lanes and not row.automation
    assert row.main_height < height and headers[a].height() == row.height
    assert not headers[a].automation.main.device.isVisible() and not headers[a].volume.isVisible()
    assert not headers[a]._in_resize_zone(row.main_height - 2)
    assert not [area for area in arrangement.lanes.envelope_areas() if area.owner == a]
    window.grab()

    # A folded group is its name row too (a little taller than a track's), and hides its tracks.
    QTest.mouseClick(headers[group.id], Qt.MouseButton.LeftButton, pos=headers[group.id]._fold_rect().center())
    row = layout.row_for(group.id)
    assert layout.row_for(a).main_height < row.main_height < height and not row.lanes and not row.automation
    assert not headers[group.id].automation.main.device.isVisible() and layout.row_for(b).hidden
    window.grab()

    # Unfolded, both show their automation as before.
    for track_id in (a, group.id):
        QTest.mouseClick(headers[track_id], Qt.MouseButton.LeftButton, pos=headers[track_id]._fold_rect().center())
    assert layout.row_for(a).lanes and layout.row_for(a).main_height == height
    assert layout.row_for(group.id).lanes and headers[group.id].automation.main.device.isVisible()


def drag_lanes(window, start: QPoint, end: QPoint) -> None:
    lanes = window.arrangement.lanes
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(lanes, start + QPoint(10, 0))
    QTest.mouseMove(lanes, end)
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)
    QTest.qWait(10)


def test_a_folded_tracks_clips_are_bars_to_click_and_drag(window):
    """As in Ableton: a folded track shows its clips as bars with their names, a
    click selects one and a drag moves it; its lane isn't a grid to select time on."""
    a, b = make_tracks(window, 2)
    window.editor.set_folded(a, True)
    QTest.qWait(10)
    lanes = window.arrangement.lanes
    view = window.arrangement.view
    project = window.project
    layout = window.arrangement.layout_model
    row = layout.row_for(a)
    assert row.bars and row.main_height < layout.row_for(b).main_height
    low_y = row.top - view.scroll_y + row.main_height - 5  # low in the bar: still the clip's
    x = int(view.beat_to_x(1.0))  # inside clip c0 (beats 0-4)
    assert lanes.hit_clip(QPointF(x, low_y))[2] == "title"

    # A click on a bar selects its clip (and draws it selected).
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(x, low_y))
    assert window.selection.clips == {(a, "c0")} and window.selection.time_range == (0.0, 4.0, (a,))
    window.grab()
    # A drag moves it.
    drag_lanes(window, QPoint(x, low_y), QPoint(int(view.beat_to_x(3.0)), low_y))
    assert project.track(a).clips[0].start_beat == view.snap_beat(2.0)
    assert window.undo_stack.undoText() == "Move Time Selection"
    window.undo_stack.undo()

    # Beside its clips, a drag selects nothing: a click there only moves the insert marker.
    window.selection.clear()
    drag_lanes(window, QPoint(int(view.beat_to_x(5.0)), low_y), QPoint(int(view.beat_to_x(7.0)), low_y))
    assert window.selection.time_range is None and not window.selection.clips
    assert window.selection.insert_beat == view.snap_beat(5.0) and window.selection.track_id == a
    assert lanes.cursor().shape() == Qt.CursorShape.ArrowCursor

    # A selection made on the grid of other tracks takes in the folded one, and its clips.
    other = layout.row_for(b)
    other_y = other.top - view.scroll_y + other.main_height // 2
    drag_lanes(window, QPoint(int(view.beat_to_x(1.0)), other_y), QPoint(int(view.beat_to_x(6.0)), low_y))
    assert window.selection.time_range == (1.0, 6.0, (a, b))
    assert window.selection.clips == {(a, "c0"), (b, "c1")}
    window.grab()


def test_folding_leaves_the_name_and_buttons_in_place(window):
    a, = make_tracks(window, 1)
    group = window.editor.group_tracks([a])
    QTest.qWait(10)
    headers = window.arrangement.headers.headers
    for track_id in (a, group.id):
        header = headers[track_id]
        before = header._fold_rect(), header.solo.geometry(), header.activator.geometry()
        QTest.mouseClick(header, Qt.MouseButton.LeftButton, pos=header._fold_rect().center())
        assert window.project.track(track_id).folded
        assert (header._fold_rect(), header.solo.geometry(), header.activator.geometry()) == before
        QTest.mouseClick(header, Qt.MouseButton.LeftButton, pos=header._fold_rect().center())


def test_folding_one_of_the_selected_tracks_folds_them_all(window):
    a, b, c = make_tracks(window, 3)
    group = window.editor.group_tracks([c])
    QTest.qWait(10)
    project = window.project
    headers = window.arrangement.headers.headers
    selection = window.selection
    selection.select_track(a, focus_track=True)
    selection.select_track(group.id, focus_track=True, mode="toggle")
    window.editor.set_folded(group.id, True)  # one folded already: they take the clicked one's new state

    QTest.mouseClick(headers[a], Qt.MouseButton.LeftButton, pos=headers[a]._fold_rect().center())
    assert project.track(a).folded and project.track(group.id).folded and not project.track(b).folded
    assert set(selection.track_ids) == {a, group.id}  # the click doesn't change the selection
    QTest.mouseClick(headers[group.id], Qt.MouseButton.LeftButton, pos=headers[group.id]._fold_rect().center())
    assert not project.track(a).folded and not project.track(group.id).folded

    # An unselected track's fold button folds just it.
    QTest.mouseClick(headers[b], Qt.MouseButton.LeftButton, pos=headers[b]._fold_rect().center())
    assert project.track(b).folded and not project.track(a).folded


def test_groups_can_be_cut_copied_and_pasted(window):
    a, b, c = make_tracks(window)
    project, selection = window.project, window.selection
    group = window.editor.group_tracks([a, b])
    selection.select_track(group.id, focus_track=True)
    press_key(window, Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier)
    selection.select_track(c, focus_track=True)
    press_key(window, Qt.Key.Key_V, Qt.KeyboardModifier.ControlModifier)  # after C
    assert len(project.tracks) == 7  # the group, A, B, C, then the copies
    pasted = project.tracks[4]
    assert pasted.is_group and pasted.parent is None and selection.track_ids == (pasted.id,)
    inside = project.descendants(pasted.id)
    assert [len(t.clips) for t in inside] == [1, 1] and {t.id for t in inside}.isdisjoint({a, b})
    assert window.undo_stack.undoText() == "Paste Track"

    selection.select_track(group.id, focus_track=True)
    press_key(window, Qt.Key.Key_X, Qt.KeyboardModifier.ControlModifier)
    assert not project.has_track(group.id) and not project.has_track(a) and len(project.tracks) == 4
    selection.select_track(c, focus_track=True)
    press_key(window, Qt.Key.Key_V, Qt.KeyboardModifier.ControlModifier)
    assert len(project.tracks) == 7 and project.tracks[1].is_group  # after C, before the first copy
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert project.has_track(group.id) and [t.id for t in project.descendants(group.id)] == [a, b]


def test_a_refused_cut_leaves_the_clipboard_and_the_header_menu_acts_on_tracks(window, monkeypatch):
    from PySide6.QtGui import QContextMenuEvent
    from PySide6.QtWidgets import QMenu

    a, b, c = make_tracks(window)
    project, selection, lanes = window.project, window.selection, window.arrangement.lanes
    group = window.editor.group_tracks([a, b])
    # Cutting a track in a frozen group is refused: nothing is cut, the clipboard keeps what it had.
    lanes.clipboard = before = object()
    monkeypatch.setattr(project, "frozen_by", lambda t: group.id if t in (a, b) else None)
    selection.select_track(a, focus_track=True)
    press_key(window, Qt.Key.Key_X, Qt.KeyboardModifier.ControlModifier)
    assert project.has_track(a) and lanes.clipboard is before
    monkeypatch.undo()

    # Right-clicking a selected track's header: its Cut and Copy act on the selected
    # tracks, not on a clip range selected since.
    selection.select_track(c, focus_track=True)
    selection.set_time_range(0.0, 4.0, [c], clips=window.editor.clips_in_range(0.0, 4.0, [c]))
    assert selection.track_id == c and selection.focus == "clips"
    from substation.ui.arrangement import track_headers

    class Menu(QMenu):
        def exec(self, *_args):  # (not shown)
            return None

    monkeypatch.setattr(track_headers, "QMenu", Menu)
    header = window.arrangement.headers.headers[c]
    header.contextMenuEvent(QContextMenuEvent(QContextMenuEvent.Reason.Mouse, QPoint(20, 8), header.mapToGlobal(QPoint(20, 8))))
    assert selection.focus == "track" and selection.time_range is None and selection.track_ids == (c,)
    assert window._what_is_copied("copied") == "tracks"

    # Ctrl+R doesn't rename a track hidden in a folded group.
    window.editor.set_folded(group.id, True)
    QTest.qWait(10)
    assert not window.arrangement.rename_track(a)
    assert window.arrangement.rename_track(group.id)
