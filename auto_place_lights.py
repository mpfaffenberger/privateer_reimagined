#!/usr/bin/env python3
"""Auto-place lights on all ship sprites using the Sprite Light Placer agent.

Usage:
    python auto_place_lights.py                    # Default: 8 parallel workers
    python auto_place_lights.py --parallel 4       # 4 parallel workers
    python auto_place_lights.py --resume           # Resume interrupted run
    python auto_place_lights.py --dry-run          # Show what would be done
"""

import argparse
import json
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Configuration
SKIP_SHIPS = {"steltek", "derelict", "drone", "scout", "paradigm"}  # Per user request
DEFAULT_PARALLEL = 8
ASSETS_DIR = Path(__file__).parent / "assets" / "ships"
SHOWROOM_FILE = Path(__file__).parent / "assets" / "systems" / "sprite_showroom.json"
PROGRESS_FILE = Path(__file__).parent / ".light_placement_progress.json"
MODEL = "claude-code-claude-opus-4-8"


@dataclass
class ShipAngle:
    """Represents a single sprite angle for a ship."""
    ship: str
    az: str  # e.g., "az000"
    el: str  # e.g., "el+000"
    sprite_path: str  # Full path to the sprite image
    lights_path: str  # Where to write the lights JSON
    angle_dir: str  # "sprites_3d" or "sprites"


@dataclass
class Progress:
    """Tracks progress of light placement."""
    completed: dict[str, list[str]] = field(default_factory=dict)  # ship -> list of completed angles
    failed: dict[str, list[str]] = field(default_factory=dict)  # ship -> list of failed angles
    start_time: Optional[float] = None

    def is_done(self, ship: str, angle_key: str) -> bool:
        """Check if a specific angle has been completed."""
        return angle_key in self.completed.get(ship, [])

    def mark_done(self, ship: str, angle_key: str):
        """Mark an angle as completed."""
        if ship not in self.completed:
            self.completed[ship] = []
        if angle_key not in self.completed[ship]:
            self.completed[ship].append(angle_key)

    def mark_failed(self, ship: str, angle_key: str):
        """Mark an angle as failed."""
        if ship not in self.failed:
            self.failed[ship] = []
        if angle_key not in self.failed[ship]:
            self.failed[ship].append(angle_key)

    def save(self):
        """Save progress to disk."""
        data = {
            "completed": self.completed,
            "failed": self.failed,
            "start_time": self.start_time
        }
        with open(PROGRESS_FILE, 'w') as f:
            json.dump(data, f, indent=2)

    @classmethod
    def load(cls, seed_from_existing: bool = False) -> "Progress":
        """Load progress from disk.
        
        If seed_from_existing is True, also scan for existing lights.json files
        and mark them as completed.
        """
        if not PROGRESS_FILE.exists():
            p = cls()
            if seed_from_existing:
                existing = seed_progress_from_existing_lights()
                p.completed = existing
                print(f"📁 Found {sum(len(v) for v in existing.values())} existing lights files")
            return p
        try:
            with open(PROGRESS_FILE, 'r') as f:
                data = json.load(f)
            p = cls()
            p.completed = data.get("completed", {})
            p.failed = data.get("failed", {})
            p.start_time = data.get("start_time")
            return p
        except (json.JSONDecodeError, IOError):
            p = cls()
            if seed_from_existing:
                existing = seed_progress_from_existing_lights()
                p.completed = existing
                print(f"📁 Found {sum(len(v) for v in existing.values())} existing lights files")
            return p


def discover_ships() -> list[str]:
    """Find ships from the showroom definition."""
    if not SHOWROOM_FILE.exists():
        # Fallback to scanning directory
        ships = []
        for item in sorted(ASSETS_DIR.iterdir()):
            if item.is_dir() and item.name not in SKIP_SHIPS:
                ships.append(item.name)
        return ships
    
    try:
        with open(SHOWROOM_FILE, 'r') as f:
            showroom = json.load(f)
        
        ships = []
        for sprite in showroom.get("placed_ship_sprites", []):
            atlas = sprite.get("atlas", "")  # e.g., "ships/broadsword/atlas_manifest_3d"
            if atlas.startswith("ships/"):
                ship_name = atlas.split("/")[1]
                if ship_name not in SKIP_SHIPS:
                    ships.append(ship_name)
        return sorted(set(ships))
    except (json.JSONDecodeError, IOError):
        # Fallback
        ships = []
        for item in sorted(ASSETS_DIR.iterdir()):
            if item.is_dir() and item.name not in SKIP_SHIPS:
                ships.append(item.name)
        return ships


def discover_angles(ship: str, existing_only: bool = False) -> list[ShipAngle]:
    """Discover all sprite angles for a ship.
    
    Only discovers from sprites_3d/ directory since that's what the showroom uses.
    If existing_only=True, only returns angles that already have lights.json files.
    """
    angles = []
    ship_dir = ASSETS_DIR / ship

    # Only check sprites_3d directory (used by showroom)
    sprites_3d_dir = ship_dir / "sprites_3d"
    if sprites_3d_dir.exists():
        for png_file in sorted(sprites_3d_dir.glob("*_3d.png")):
            # Parse filename: ship_az000_el+000_3d.png
            # Format is: {ship}_{az}_{el}_3d
            stem = png_file.stem  # e.g., "centurion_az000_el+000_3d"
            
            # Find az and el in the stem
            import re
            match = re.search(r'(az\d+[p\d]*)_(el[+-]\d+)', stem)
            if match:
                az = match.group(1)  # e.g., "az000"
                el = match.group(2)  # e.g., "el+000"
                angle_key = f"{az}_{el}"
                lights_path = str(png_file.with_suffix(".lights.json"))
                
                # If existing_only, skip if lights file doesn't exist
                if existing_only and not Path(lights_path).exists():
                    continue
                    
                angles.append(ShipAngle(
                    ship=ship,
                    az=az,
                    el=el,
                    sprite_path=str(png_file),
                    lights_path=lights_path,
                    angle_dir="sprites_3d"
                ))

    return angles


def seed_progress_from_existing_lights() -> dict[str, list[str]]:
    """Scan for existing lights.json files and mark them as completed."""
    import re
    completed = {}
    
    # Fast glob for all lights.json files in sprites_3d
    for lights_file in ASSETS_DIR.glob("*/sprites_3d/*.lights.json"):
        ship = lights_file.parent.parent.name
        if ship in SKIP_SHIPS:
            continue
        
        # Parse angle from filename: ship_az000_el+000_3d.lights.json
        stem = lights_file.stem  # e.g., "centurion_az000_el+000_3d"
        match = re.search(r'(az\d+[p\d]*)_(el[+-]\d+)', stem)
        if match:
            angle_key = f"{match.group(1)}_{match.group(2)}"
            if ship not in completed:
                completed[ship] = []
            completed[ship].append(angle_key)
    
    return completed


def run_agent(sprite_path: str, output_path: str, ship: str, angle_key: str) -> bool:
    """Run the Sprite Light Placer agent on a single sprite."""
    prompt = f"""Place lights on {sprite_path} for the {ship} ship.

Output to: {output_path}

The angle is {angle_key}. Place appropriate lights based on the view:
- Engine exhausts should glow from the rear of the ship
- Cockpit windows from the front/top
- Weapon hardpoints as applicable

IMPORTANT: Write to {output_path} - this is the sprites_3d directory used by the showroom.
"""
    
    cmd = [
        "code-puppy",
        "-p", prompt,
        "--model", MODEL,
        "-a", "sprite-light-placer"
    ]
    
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=180,  # 3 minute timeout
            cwd=ASSETS_DIR.parent.parent  # Run from project root
        )
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"  ⏱️  Timeout on {ship}/{angle_key}")
        return False
    except Exception as e:
        print(f"  ❌ Error on {ship}/{angle_key}: {e}")
        return False


@dataclass
class WorkerState:
    """State for a worker thread."""
    index: int
    running: bool = True
    lock: threading.Lock = field(default_factory=threading.Lock)


class LightPlacer:
    """Main orchestrator for light placement."""

    def __init__(self, parallel: int = DEFAULT_PARALLEL, dry_run: bool = False, 
                 resume: bool = False, seed_existing: bool = False,
                 filter_ship: str = None):
        self.parallel = parallel
        self.dry_run = dry_run
        self.filter_ship = filter_ship
        self.progress = Progress.load(seed_from_existing=seed_existing)
        
        if resume and self.progress.start_time is None:
            # If resuming but no start time, this is a fresh start
            self.progress.start_time = time.time()
        elif self.progress.start_time is None:
            self.progress.start_time = time.time()

    def get_pending_angles(self) -> list[tuple[str, ShipAngle]]:
        """Get all angles that still need lights placed."""
        pending = []
        ships = discover_ships()
        
        for ship in ships:
            # Filter by specific ship if requested
            if self.filter_ship and ship != self.filter_ship:
                continue
            angles = discover_angles(ship)
            for angle in angles:
                angle_key = f"{angle.az}_{angle.el}"
                if not self.progress.is_done(ship, angle_key):
                    pending.append((ship, angle))
        
        return pending

    def worker(self, work_queue, state: WorkerState):
        """Worker thread that processes items from the queue."""
        items_processed = 0
        while state.running:
            try:
                item = work_queue.get(timeout=0.5)
            except:
                # Check if queue is empty - if we've processed items and queue is empty, we're done
                if items_processed > 0 and work_queue.empty():
                    break
                continue
            
            items_processed += 1
            ship, angle = item
            angle_key = f"{angle.az}_{angle.el}"
            
            print(f"[{state.index}] 📍 {ship}/{angle_key} -> {angle.angle_dir}")
            
            if self.dry_run:
                print(f"[{state.index}]   (dry run - would place lights)")
                self.progress.mark_done(ship, angle_key)
            else:
                success = run_agent(angle.sprite_path, angle.lights_path, ship, angle_key)
                if success:
                    self.progress.mark_done(ship, angle_key)
                    print(f"[{state.index}] ✅ {ship}/{angle_key}")
                else:
                    self.progress.mark_failed(ship, angle_key)
                    print(f"[{state.index}] ❌ {ship}/{angle_key}")
            
            work_queue.task_done()

    def run(self):
        """Run the light placement process."""
        pending = self.get_pending_angles()
        
        if not pending:
            print("✨ All lights already placed! Nothing to do.")
            return

        print(f"🎯 Found {len(pending)} angles to process")
        print(f"📊 Ships: {len(set(s[0] for s in pending))}")
        print(f"⚡ Parallel workers: {self.parallel}")
        print(f"📁 Progress saved to: {PROGRESS_FILE}")
        print()

        if self.dry_run:
            print("🧪 DRY RUN MODE - no lights will actually be placed\n")

        # Create thread-safe work queue
        import queue
        work_queue = queue.Queue()
        for item in pending:
            work_queue.put(item)
        
        # Create worker states
        workers = []
        for i in range(self.parallel):
            state = WorkerState(index=i)
            workers.append(state)

        # Start workers
        threads = []
        for i, state in enumerate(workers):
            t = threading.Thread(target=self.worker, args=(work_queue, state))
            t.start()
            threads.append(t)

        # Wait for all work to complete or interrupt
        try:
            while not work_queue.empty():
                if self.dry_run:
                    # In dry-run, work is fast - just wait for queue to drain
                    pass
                time.sleep(0.1)
                if any(not t.is_alive() for t in threads):
                    break
        except KeyboardInterrupt:
            print("\n\n⚠️  Interrupted! Saving progress...")
            for state in workers:
                state.running = False
            for t in threads:
                t.join(timeout=2)
            self.progress.save()
            print(f"✅ Progress saved. Run with --resume to continue.")
            return

        # Wait for threads to finish
        for t in threads:
            t.join(timeout=1)

        # All done
        self.progress.save()
        
        total_completed = sum(len(v) for v in self.progress.completed.values())
        total_failed = sum(len(v) for v in self.progress.failed.values())
        elapsed = time.time() - self.progress.start_time
        
        print(f"\n✨ Done! Completed {total_completed} angles in {elapsed:.1f}s")
        if total_failed > 0:
            print(f"❌ Failed: {total_failed} angles")
            for ship, angles in self.progress.failed.items():
                print(f"   {ship}: {angles}")

    def show_status(self):
        """Show current status without doing work."""
        pending = self.get_pending_angles()
        completed = sum(len(v) for v in self.progress.completed.values())
        failed = sum(len(v) for v in self.progress.failed.values())
        
        print(f"📊 Light Placement Status")
        print(f"   Pending: {len(pending)} angles")
        print(f"   Completed: {completed} angles")
        print(f"   Failed: {failed} angles")
        
        if self.progress.completed:
            print(f"\n✅ Completed ships:")
            for ship, angles in sorted(self.progress.completed.items()):
                print(f"   {ship}: {len(angles)} angles")


def main():
    parser = argparse.ArgumentParser(description="Auto-place lights on ship sprites")
    parser.add_argument("--parallel", "-p", type=int, default=DEFAULT_PARALLEL,
                        help=f"Number of parallel workers (default: {DEFAULT_PARALLEL})")
    parser.add_argument("--resume", "-r", action="store_true",
                        help="Resume an interrupted run")
    parser.add_argument("--dry-run", "-n", action="store_true",
                        help="Show what would be done without doing it")
    parser.add_argument("--status", "-s", action="store_true",
                        help="Show status without doing work")
    parser.add_argument("--reset", action="store_true",
                        help="Reset progress and start fresh")
    parser.add_argument("--seed", action="store_true",
                        help="Seed progress from existing lights.json files")
    parser.add_argument("--ship", 
                        help="Only process this ship (e.g., 'centurion')")
    
    args = parser.parse_args()

    if args.reset:
        if PROGRESS_FILE.exists():
            PROGRESS_FILE.unlink()
            print("✅ Progress reset.")
        else:
            print("No progress file to reset.")
        return

    placer = LightPlacer(parallel=args.parallel, dry_run=args.dry_run, 
                         resume=args.resume, seed_existing=args.seed,
                         filter_ship=args.ship)

    if args.status:
        placer.show_status()
    else:
        placer.run()


if __name__ == "__main__":
    main()
