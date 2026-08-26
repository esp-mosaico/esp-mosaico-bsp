#!/usr/bin/env python3
"""Deterministic loss/jitter model for dual-screen Pong snapshot delivery."""

from __future__ import annotations

import argparse
import heapq
import math
import random
from dataclasses import dataclass, field

SIM_HZ = 120
SNAPSHOT_HZ = 30
INPUT_HZ = 50
DURATION_S = 30
RENDER_DELAY_S = 0.067
MAX_EXTRAPOLATION_S = 0.100


@dataclass(order=True)
class Packet:
    delivery_time: float
    sequence: int = field(compare=False)
    sent_time: float = field(compare=False)
    position: float = field(compare=False)
    velocity: float = field(compare=False)
    event_id: int = field(compare=False)


def truth(t: float) -> tuple[float, float]:
    """A bounded path with acceleration and Pong-like direction changes."""
    phase = 2.0 * math.pi * t / 2.6
    return 480.0 + 390.0 * math.sin(phase), 390.0 * (2.0 * math.pi / 2.6) * math.cos(phase)


def run(loss: float, seed: int) -> dict[str, float]:
    rng = random.Random(seed)
    in_flight: list[Packet] = []
    received: list[Packet] = []
    sequence = 0
    old_packets = 0
    delivered_events: set[int] = set()
    duplicate_events = 0
    duplicate_events_suppressed = 0
    squared_error = 0.0
    max_error = 0.0
    rendered = 0

    for tick in range(DURATION_S * SIM_HZ):
        now = tick / SIM_HZ
        if tick % (SIM_HZ // SNAPSHOT_HZ) == 0:
            sequence += 1
            if rng.random() >= loss:
                position, velocity = truth(now)
                jitter = max(0.001, rng.gauss(0.025, 0.014))
                if rng.random() < 0.04:
                    jitter += rng.uniform(0.03, 0.09)
                heapq.heappush(
                    in_flight,
                    Packet(now + jitter, sequence, now, position, velocity, tick // SIM_HZ),
                )

        while in_flight and in_flight[0].delivery_time <= now:
            packet = heapq.heappop(in_flight)
            if received and packet.sequence <= received[-1].sequence:
                old_packets += 1
                continue
            received.append(packet)
            received = received[-4:]
            if packet.event_id in delivered_events:
                duplicate_events_suppressed += 1
            else:
                delivered_events.add(packet.event_id)

        render_time = now - RENDER_DELAY_S
        if render_time < 0 or not received:
            continue
        before = None
        after = None
        for packet in received:
            if packet.sent_time <= render_time:
                before = packet
            elif after is None:
                after = packet
        if before and after:
            span = after.sent_time - before.sent_time
            alpha = (render_time - before.sent_time) / span
            estimate = before.position + (after.position - before.position) * alpha
        else:
            newest = received[-1]
            dt = min(max(0.0, render_time - newest.sent_time), MAX_EXTRAPOLATION_S)
            estimate = newest.position + newest.velocity * dt
        actual, _ = truth(render_time)
        error = abs(estimate - actual)
        squared_error += error * error
        max_error = max(max_error, error)
        rendered += 1

    return {
        "loss": loss,
        "rms_error_px": math.sqrt(squared_error / rendered),
        "max_error_px": max_error,
        "old_packets_dropped": float(old_packets),
        "duplicate_events": float(duplicate_events),
        "duplicate_events_suppressed": float(duplicate_events_suppressed),
        "input_packets": float(DURATION_S * INPUT_HZ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=0x51A31)
    args = parser.parse_args()

    for loss in (0.05, 0.10):
        result = run(loss, args.seed)
        print(
            f"loss={loss:.0%} rms={result['rms_error_px']:.1f}px "
            f"max={result['max_error_px']:.1f}px old={int(result['old_packets_dropped'])}"
        )
        assert result["rms_error_px"] < 28.0
        assert result["max_error_px"] < 150.0
        assert result["duplicate_events"] == 0
    print("PASS: interpolation remains bounded at 5-10% packet loss")


if __name__ == "__main__":
    main()
