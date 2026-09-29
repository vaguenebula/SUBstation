import pytest

from gilstudio.model.timebase import (
    TimeSignature,
    beats_to_seconds,
    format_bar_label,
    format_db,
    format_pan,
    format_position,
    parse_position,
    seconds_to_beats,
)

FOUR_FOUR = TimeSignature(4, 4)
SIX_EIGHT = TimeSignature(6, 8)


def test_time_signature_lengths():
    assert FOUR_FOUR.beats_per_bar == 4.0
    assert SIX_EIGHT.beats_per_bar == 3.0
    assert SIX_EIGHT.beat_length == 0.5


def test_seconds_and_beats():
    assert beats_to_seconds(4.0, 120.0) == 2.0
    assert seconds_to_beats(2.0, 120.0) == 4.0


@pytest.mark.parametrize(
    "beats, ts, expected",
    [
        (0.0, FOUR_FOUR, "1.1.1"),
        (1.0, FOUR_FOUR, "1.2.1"),
        (4.25, FOUR_FOUR, "2.1.2"),
        (15.99, FOUR_FOUR, "4.4.4"),
        (3.0, SIX_EIGHT, "2.1.1"),
        (3.5, SIX_EIGHT, "2.2.1"),
    ],
)
def test_format_position(beats, ts, expected):
    assert format_position(beats, ts) == expected


def test_parse_position_roundtrip():
    for beats in (0.0, 1.0, 4.25, 13.75):
        assert parse_position(format_position(beats, FOUR_FOUR), FOUR_FOUR) == beats
    assert parse_position("3", FOUR_FOUR) == 8.0
    assert parse_position("nonsense", FOUR_FOUR) is None
    assert parse_position("0.1.1", FOUR_FOUR) is None


def test_bar_labels():
    assert format_bar_label(8.0, FOUR_FOUR) == "3"
    assert format_bar_label(9.0, FOUR_FOUR) == "3.2"
    assert format_bar_label(9.25, FOUR_FOUR) == "3.2.2"


def test_value_formatting():
    assert format_db(-80.0) == "-inf dB"
    assert format_db(-6.02) == "-6.0 dB"
    assert format_pan(0.0) == "C"
    assert format_pan(-1.0) == "50L"
    assert format_pan(0.5) == "25R"
