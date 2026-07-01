#!/usr/bin/env python3
"""Rewrite each base's concourse hotspots to match the CANONICAL facilities
from the Privateer Playtester's Guide (OCR'd from the PDF).

Each base in the guide's system pages lists its premium facilities:
Mercenaries' Guild, Merchants' Guild, and/or Ship Dealer. Bar, Commodity
Exchange, Mission Computer (+ our Cargo Hold + Launch) are universal. Our
Equipment screen (guns/shields/engines refit) is the base's outfitter, so it
tracks Ship-Dealer presence.

FAC codes:  M=Mercenaries' Guild  T=Merchants' (Trade) Guild  S=Ship Dealer
A handful of bases the OCR couldn't read adjacently fall back to the
guide's per-TYPE concourse default (pirate=M+S, mining/refinery=M+T).
Special bases (Perry Naval / New Constantinople / New Detroit / Oxford /
derelict) are set from their one-of-a-kind concourse pages.
"""
import os, re, glob

BASES = os.path.join(os.path.dirname(__file__), "..", "assets", "bases")

# base_id -> premium facilities (from the guide). '' = nothing premium.
FAC = {
    "achilles": "MTS", "anapolis": "MS",  "basque": "MTS", "basra": "MS",
    "beaconsfield": "T", "bodensee": "TS", "burton": "TS", "charon": "MT",
    "derelict_base": "", "drake": "MS", "edinburgh": "MTS", "edom": "T",
    "elysia": "MT", "erewhon": "T", "glasgow": "MT", "gracchus": "TS",
    "hector": "MTS", "heimdel": "TS", "helen": "TS", "jolson": "MTS",
    "joplin": "MT", "kronecker": "MT", "lisacc": "MS", "liverpool": "MTS",
    "macabee": "MT", "magdaline": "TS", "matahari": "MT", "meadow": "TS",
    "megiddo": "MS", "mjolnar": "MT", "munchen": "MT", "n1912_1": "MTS",
    "new_constantinople": "MTS", "new_detroit": "MTS", "new_iberia": "T",
    "new_reno": "MT", "nitir": "TS", "oakham": "MS", "olympus": "MTS",
    "oresville": "MT", "oxford": "T", "palan": "TS", "perry_naval": "MS",
    "remus": "MT", "rilke": "MT", "rodin": "MTS", "romulus": "MT",
    "rygannon": "MS", "saratov": "MT", "siva": "T", "smallville": "MS",
    "speke": "MT", "surtur": "TS", "thisbury": "TS", "trinsic": "MT",
    "tucks": "MS", "valkyrie": "MT", "victoria": "MT", "vishnu": "MS",
    "wickerton": "MT",
}

META = {
    "MercenariesGuild": ("MERCENARIES' GUILD", "Combat contracts (5000 cr to join)"),
    "MerchantsGuild":   ("MERCHANTS' GUILD",   "Trade contracts (1000 cr to join)"),
    "ShipDealer":       ("SHIP DEALER",        "Trade up your hull"),
    "Equipment":        ("EQUIPMENT",          "Guns, shields, engines"),
    "CommodityExchange":("COMMODITY EXCHANGE", "Buy and sell cargo"),
    "MissionComputer":  ("MISSION COMPUTER",   "Find work"),
    "CargoHold":        ("CARGO HOLD",         "Sell loot and salvage"),
    "Bar":              ("BAR",                "Drinks, rumours, and fixers"),
    "Launch":           ("LAUNCH",             "Return to flight"),
}

COLS_X = [0.04, 0.37, 0.70]
ROWS_Y = [0.28, 0.50, 0.72]
W, H = 0.26, 0.16

def services_for(code):
    s = []
    if "M" in code: s.append("MercenariesGuild")
    if "T" in code: s.append("MerchantsGuild")
    if "S" in code: s += ["ShipDealer", "Equipment"]   # outfitter pair
    s += ["CommodityExchange", "MissionComputer", "CargoHold", "Bar", "Launch"]
    return s

def build_hotspots(code):
    items = services_for(code)
    lines = []
    for i, tgt in enumerate(items):
        label, tip = META[tgt]
        x, y = COLS_X[i % 3], ROWS_Y[i // 3]
        lines.append(
            f'    {{ "target": "{tgt}", "label": "{label}",\n'
            f'      "rect": [{x:.2f}, {y:.2f}, {W:.2f}, {H:.2f}], "tooltip": "{tip}" }}'
        )
    return '"hotspots": [\n' + ",\n".join(lines) + "\n  ]"

changed = 0
for j in sorted(glob.glob(os.path.join(BASES, "*", "base.json"))):
    bid = os.path.basename(os.path.dirname(j))
    if bid not in FAC:
        print(f"!! {bid}: no facility entry — skipped"); continue
    text = open(j).read()
    new = build_hotspots(FAC[bid])
    out, n = re.subn(r'"hotspots"\s*:\s*\[.*\]', new, text, flags=re.DOTALL)
    if n != 1:
        print(f"!! {bid}: hotspots block not found/ambiguous"); continue
    if out != text:
        open(j, "w").write(out); changed += 1
        guilds = "".join(c for c in FAC[bid])
        print(f"   {bid:<20} {FAC[bid] or '(none)'}")
print(f"\nRewrote {changed} base(s).")
