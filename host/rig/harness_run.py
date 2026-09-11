"""
Phase 1 host harness: deploy bela/harness-passthrough/ to the Bela Gem, run it for a
fixed time box, fetch the recording, score it, and write a log record.

    guitar -> M4 (buffered direct out) -> Bela Gem in -> DSP -> Bela Gem out
                                                                       |
                                                        power amp (Dayton DTA120)
                                                                       |
                                                              exciter -> body -> pickup

This drives the exciter for real (bela/harness-passthrough/ is a passthrough, not a
silent diagnostic like io-check-input/watcher-check) -- ground rule 2 applies in full:
run this only with Abel present and the rig confirmed live. There is no in-line
hardware kill in this build (ground-rules-and-facts.md 4.5); the DTA120's power
switch is the manual backstop.

What this does NOT do yet: anything with host/rig/watcher_check.py's live streaming.
Watching in_peak/out_peak/muted while a run is in progress is available (the Bela
project already exposes them) but wiring a live monitor into this script is future
work -- see docs/phase-plan.md Phase 1. Right now this only reads back the recording
after the run stops, which is enough to score a take.

Usage:
    python3 -m rig.harness_run --seconds 20 --name first-take
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # host/ on sys.path
from harness import logrecord, metrics  # noqa: E402

BOARD_IP = "192.168.7.2"
BOARD_USER = "root"
PROJECT_NAME = "harness-passthrough"
REPO_ROOT = Path(__file__).resolve().parents[2]
LOCAL_PROJECT_DIR = REPO_ROOT / "bela" / PROJECT_NAME
REMOTE_SRC_DIR = f"/root/{PROJECT_NAME}"           # scp target
REMOTE_BUILD_DIR = f"/root/Bela/projects/{PROJECT_NAME}"  # where build_project.sh runs it
REMOTE_SCRIPTS_DIR = "/root/Bela/scripts"

# Extra time given to the SSH round trips around the run itself -- not part of the
# Bela-side time box (that's the watchdog inside render.cpp, unaffected by this).
_STARTUP_MARGIN_S = 3.0
_SHUTDOWN_MARGIN_S = 3.0

DEFAULT_SECONDS = 20.0  # short by design -- see the presence/safety note in main()


def _ssh(args: list[str], timeout: float | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["ssh", "-o", "BatchMode=yes", f"{BOARD_USER}@{BOARD_IP}", *args],
        capture_output=True, text=True, timeout=timeout,
    )


def _scp_to_board(local_paths: list[Path], remote_dir: str) -> None:
    subprocess.run(
        ["ssh", "-o", "BatchMode=yes", f"{BOARD_USER}@{BOARD_IP}", f"mkdir -p {remote_dir}"],
        check=True, capture_output=True, text=True,
    )
    subprocess.run(
        ["scp", "-o", "BatchMode=yes", *[str(p) for p in local_paths],
         f"{BOARD_USER}@{BOARD_IP}:{remote_dir}/"],
        check=True, capture_output=True, text=True,
    )


def _scp_from_board(remote_path: str, local_path: Path) -> bool:
    r = subprocess.run(
        ["scp", "-o", "BatchMode=yes", f"{BOARD_USER}@{BOARD_IP}:{remote_path}", str(local_path)],
        capture_output=True, text=True,
    )
    return r.returncode == 0


def deploy_and_run(seconds: float) -> dict:
    """Build+run on the board for `seconds`, then stop it cleanly. Returns build/run log text."""
    source_files = [
        LOCAL_PROJECT_DIR / "render.cpp",
        LOCAL_PROJECT_DIR / "Watcher.h",
        LOCAL_PROJECT_DIR / "Watcher.cpp",
    ]
    _scp_to_board(source_files, REMOTE_SRC_DIR)

    build = _ssh(
        [f"cd {REMOTE_SCRIPTS_DIR} && ./build_project.sh {REMOTE_SRC_DIR} "
         f"-p {PROJECT_NAME} --force -b"],
        timeout=60,
    )

    print(f"running for {seconds:.0f}s ...", flush=True)
    time.sleep(seconds + _SHUTDOWN_MARGIN_S)

    stop = _ssh([f"cd {REMOTE_SCRIPTS_DIR} && ./stop_running.sh"], timeout=30)

    return {"build_stdout": build.stdout, "stop_stdout": stop.stdout}


def fetch_recording(local_dir: Path) -> tuple[Path | None, Path | None]:
    local_dir.mkdir(parents=True, exist_ok=True)
    inputs_local = local_dir / "inputs.wav"
    outputs_local = local_dir / "outputs.wav"
    got_in = _scp_from_board(f"{REMOTE_BUILD_DIR}/inputs.wav", inputs_local)
    got_out = _scp_from_board(f"{REMOTE_BUILD_DIR}/outputs.wav", outputs_local)
    return (inputs_local if got_in else None, outputs_local if got_out else None)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="rig.harness_run",
        description="Phase 1: run bela/harness-passthrough/ on the Gem, score the take, log it.",
    )
    p.add_argument("--name", default="harness-take", help="run name, used in filenames")
    p.add_argument("--seconds", type=float, default=DEFAULT_SECONDS,
                   help=f"time box for the Bela-side run (default {DEFAULT_SECONDS:.0f}s)")
    p.add_argument("--logs", type=Path, default=None, help="where to write the fetched audio")
    p.add_argument("--i-confirm-abel-present", action="store_true",
                   help="required: this drives the exciter for real (ground rule 2)")
    args = p.parse_args(argv)

    if not args.i_confirm_abel_present:
        print(
            "error: this deploys a real passthrough that drives the exciter for "
            f"{args.seconds:.0f}s and refuses to run without --i-confirm-abel-present.\n"
            "Ground rule 2: Abel must have confirmed the rig is live and be present. "
            "This build has no in-line hardware kill (ground-rules 4.5) -- the DTA120's "
            "power switch is the manual backstop.",
            file=sys.stderr,
        )
        return 1

    logs_dir = args.logs or REPO_ROOT / "logs" / time.strftime("%Y-%m-%d")

    run_info = deploy_and_run(args.seconds)
    inputs_wav, outputs_wav = fetch_recording(logs_dir)

    if outputs_wav is None:
        print("error: could not fetch outputs.wav from the board -- no take to score.",
              file=sys.stderr)
        print(run_info["stop_stdout"], file=sys.stderr)
        return 1

    take_metrics = metrics.analyse(str(outputs_wav))
    print(take_metrics)

    rec_path = logrecord.write_record(
        run_name=args.name,
        params={
            "phase": "Phase 1 harness", "bela_project": PROJECT_NAME,
            "timebox_s": args.seconds,
            "output_ceiling": 0.5, "watchdog_timeout_s": 120.0, "fade_in_s": 0.2,
        },
        metrics=take_metrics.to_dict(),
        audio_path=outputs_wav,
        notes=(f"raw input recording: {inputs_wav}" if inputs_wav else
               "input recording was not fetched"),
        repo_root=REPO_ROOT,
    )
    print(f"log record: {rec_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
