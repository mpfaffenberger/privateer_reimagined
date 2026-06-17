#!/usr/bin/env python3
"""Triple the size of all sprite lights in talon sidecar files."""

import json
import glob
import os

SPRITES_DIR = "/Users/mpfaffenberger/code/new_privateer/assets/ships/talon/sprites"
SPRITES_3D_DIR = "/Users/mpfaffenberger/code/new_privateer/assets/ships/talon/sprites_3d"

def triple_lights_in_file(filepath):
    """Read JSON, triple 'size' in each light, write back."""
    with open(filepath, 'r') as f:
        lights = json.load(f)
    
    modified = False
    for light in lights:
        if 'size' in light:
            old_size = light['size']
            light['size'] = int(old_size * 3)
            modified = True
    
    if modified:
        with open(filepath, 'w') as f:
            json.dump(lights, f, indent=2)
        print(f"✓ {os.path.basename(filepath)}: size 5 → {lights[0]['size'] if lights else '?'}")
    else:
        print(f"  {os.path.basename(filepath)}: no sizes to modify")

def main():
    # Process both sprites and sprites_3d directories
    for directory in [SPRITES_DIR, SPRITES_3D_DIR]:
        print(f"\n📁 Processing: {directory}")
        light_files = glob.glob(os.path.join(directory, "*.lights.json"))
        
        if not light_files:
            print("  No .lights.json files found!")
            continue
        
        for filepath in sorted(light_files):
            triple_lights_in_file(filepath)
    
    print("\n🐶 Done! All talon sprite lights tripled!")

if __name__ == "__main__":
    main()
