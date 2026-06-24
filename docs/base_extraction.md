# Base Data Extraction (Privateer → JSON)

How each per-base attribute is sourced, what we can compute, and what
still needs hand-tuning.

## Source: BASES.IFF (1162 bytes, 60 bases)

`gog_extracted/extracted/priv/DATA/SECTORS/BASES.IFF` is a single
`FORM … BASE` container. Each base is a single `INFO` chunk with the
layout:

```
[0]      idx          base index (0..59), 1 byte
[1]      kind         gameplay-kind byte (0x01..0x06), 1 byte
[2..n]   name         null-terminated ASCII
```

That's all the file contains — no per-base services/market/etc. record.

## Source: QUADRANT.IFF (per-system base IDs)

`gog_extracted/extracted/priv/DATA/SECTORS/QUADRANT.IFF` is `FORM UNIV`.
Each quadrant contains `FORM QUAD` blocks, each containing a list of
`FORM SYST` blocks. Each SYST has:

```
INFO   first 4 bytes = coords, then NUL, then system name (NUL-terminated)
BASE   list of base IDs in this system, one byte per ID
```

The BASE chunks let us map base → system without needing the system JSONs.

## Kind values

| Kind  | Meaning        | Examples                       | Count |
|-------|----------------|--------------------------------|------:|
| 0x01  | civilian       | Jolson, Matahari, Erewhon     | 8     |
| 0x02  | refinery       | Anapolis, Beaconsfield        | 15    |
| 0x03  | mining_base    | Achilles, Hector, Charon      | 15    |
| 0x04  | agricultural   | Helen, Victoria, Heimdel      | 16    |
| 0x05  | pirate         | Drake, Oakham, Tuck's         | 5     |
| 0x06  | military       | New Constantinople, Perry      | 6     |

(0x04 is shared by agricultural AND industrial — the original game
treats them as the same class. Distinguishing requires the WCU XML
`file=` attribute or human review.)

## Per-base extraction: where each attribute comes from

| Per-base attribute    | Source                                                      |
|-----------------------|-------------------------------------------------------------|
| Display name          | BASES.IFF INFO chunk                                       |
| Gameplay kind         | BASES.IFF INFO byte 1                                      |
| Star system           | QUADRANT.IFF `BASE` chunks → system name in INFO           |
| Visual file           | WCU XML `<unit file="…">` (cross-ref)                      |
| Faction               | WCU XML `<unit faction="…">`                                |
| Has ship dealer       | WCU `bases/weapons_lib.norepair` list (NOT-listed ⇒ has)   |
| **Merchant Guild**    | **Deterministic hash** of system + base name first letters  |
| **Mercenary Guild**   | **Deterministic hash** of system + base name first letters  |

### Service formulas

Both guilds are computed at runtime by the original engine (per
`privateer_wcu/bases/guilds.py`):

```python
LETTERS = "123456789a0bcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

def hashLetter(c): return LETTERS.index(c) + 1

def CanMerchantGuild():
    # (hash(system_name[0]) + hash(base_name[0])) % 2 == 0
    return (hashLetter(sys[0]) + hashLetter(base[0])) % 2 == 0

def CanMercenaryGuild():
    # (hash(system_name[0]) + hash(base_name[0])) % 4 <= 1
    return (hashLetter(sys[0]) + hashLetter(base[0])) % 4 <= 1
```

That's a 50/50 for Merchant Guild and a ~50% chance for Mercenary Guild
(0 or 1 out of 4). Across 60 bases: **35 have Merchant Guild, 33 have
Mercenary Guild**.

### Bases WITHOUT a Ship Dealer

28 named bases — every base NOT in this list has a Ship Dealer / Repair:

Burton, Charon, Drake, Edom, Elysia, Erewhon, Gaea, Glasgow, Joplin,
Kronecker, Liverpool, Macabee, Matahari, Megiddo, Mjolnar, Munchen,
New Iberia, New Reno, Oakham, Oresville, Remus, Rilke, Romulus,
Saratov, Siva, Smallville, Speke, Trinsic, Tuck's, Valkyrie, Vincent
Moon, Wickerton.

## Base → {kind, faction, services} table

(All 60 bases with computed Merchant/Mercenary Guild and Has-Ship-Dealer.)

| IDX | Name                | Kind    | Has ship dealer | Has merch guild | Has merc guild | System             | Faction  |
|----:|---------------------|---------|:---------------:|:---------------:|:--------------:|-------------------|----------|
|  0  | Achilles            | 0x03    | yes             | no              | yes            | Troy               | merchant |
|  1  | Anapolis            | 0x02    | yes             | no              | yes            | Perry              | confed   |
|  2  | Basque              | 0x03    | yes             | yes             | no             | Pyrenees           | militia  |
|  3  | Basra               | 0x02    | yes             | yes             | no             | Palan              | merchant |
|  4  | Beaconsfield        | 0x02    | yes             | no              | no             | Auriga             | militia  |
|  5  | Bodensee            | 0x04    | yes             | yes             | no             | Tingerhoff         | —        |
|  6  | Burton              | 0x04    | no              | yes             | yes            | Junction           | —        |
|  7  | Charon              | 0x03    | no              | no              | no             | Hyades             | confed   |
|  8  | Drake               | 0x05    | no              | no              | no             | Capella            | pirates  |
|  9  | Edinburgh           | 0x02    | yes             | no              | no             | New Caledonia      | confed   |
| 10  | Edom                | 0x04    | no              | no              | no             | New Constantinople | —        |
| 11  | Elysia              | 0x04    | no              | yes             | no             | Auriga             | —        |
| 12  | Erewhon             | 0x01    | no              | yes             | yes            | Shangri La         | —        |
| 13  | Glasgow             | 0x02    | no              | no              | yes            | New Caledonia      | confed   |
| 14  | Gracchus            | 0x02    | yes             | no              | yes            | Raxis              | confed   |
| 15  | Hector              | 0x03    | yes             | yes             | yes            | Troy               | merchant |
| 16  | Heimdel             | 0x04    | yes             | no              | yes            | Midgard            | —        |
| 17  | Helen               | 0x04    | yes             | yes             | yes            | Troy               | —        |
| 18  | Jolson              | 0x01    | yes             | yes             | no             | XXN-1927           | —        |
| 19  | Joplin              | 0x02    | no              | yes             | no             | XXN-1927           | militia  |
| 20  | Kronecker           | 0x03    | no              | no              | yes            | Regallis           | merchant |
| 21  | Lisacc              | 0x03    | yes             | yes             | yes            | Lisacc             | confed   |
| 22  | Liverpool           | 0x02    | no              | yes             | no             | Newcastle          | confed   |
| 23  | Macabee             | 0x03    | no              | no              | no             | Nexus              | militia  |
| 24  | Magdaline           | 0x01    | yes             | no              | yes            | Padre              | —        |
| 25  | Matahari            | 0x01    | no              | yes             | no             | Aldebran           | —        |
| 26  | Meadow              | 0x02    | yes             | no              | yes            | Hind's Variable N. | militia  |
| 27  | Megiddo             | 0x05    | no              | no              | yes            | Telar              | pirates  |
| 28  | Mjolnar             | 0x04    | no              | no              | no             | Ragnarok           | —        |
| 29  | Munchen             | 0x02    | no              | no              | yes            | Tingerhoff         | confed   |
| 30  | N1912-1              | 0x01    | yes             | yes             | no             | DN-N1912           | —        |
| 31  | New Constantinople  | 0x06    | yes             | yes             | yes            | New Constantinople | —        |
| 32  | New Detroit         | 0x06    | yes             | yes             | yes            | New Detroit        | —        |
| 33  | New Iberia          | 0x04    | yes             | yes             | no             | Pyrenees           | —        |
| 34  | New Reno            | 0x01    | yes             | yes             | yes            | ND-57              | —        |
| 35  | Nitir               | 0x04    | yes             | yes             | yes            | Nitir              | —        |
| 36  | Oakham              | 0x05    | no              | no              | no             | Pentonville        | pirates  |
| 37  | Olympus             | 0x01    | yes             | yes             | no             | Saxtogue           | —        |
| 38  | Oresville           | 0x04    | no              | no              | no             | Hind's Variable N. | —        |
| 39  | Oxford              | 0x06    | yes             | yes             | no             | Oxford             | —        |
| 40  | Palan               | 0x04    | yes             | yes             | yes            | Palan              | —        |
| 41  | Perry Naval Base    | 0x06    | yes             | yes             | yes            | Perry              | —        |
| 42  | Remus               | 0x02    | no              | yes             | no             | Pollux             | militia  |
| 43  | Rilke               | 0x02    | no              | yes             | yes            | Varnus             | merchant |
| 44  | Rodin               | 0x04    | yes             | yes             | yes            | Varnus             | —        |
| 45  | Romulus             | 0x03    | no              | no              | yes            | Castor             | confed   |
| 46  | Rygannon            | 0x03    | yes             | yes             | yes            | Rygannon           | militia  |
| 47  | Saratov             | 0x03    | no              | no              | no             | Prasepe            | militia  |
| 48  | Siva                | 0x04    | no              | no              | yes            | Rikel              | —        |
| 49  | Smallville          | 0x05    | no              | yes             | no             | KM-252             | pirates  |
| 50  | Speke               | 0x01    | no              | no              | yes            | Junction           | —        |
| 51  | Surtur              | 0x04    | yes             | yes             | no             | Surtur             | —        |
| 52  | Thisbury            | 0x02    | yes             | no              | yes            | Manchester         | confed   |
| 53  | Trinsic             | 0x04    | no              | yes             | no             | Raxis              | —        |
| 54  | Tuck's              | 0x05    | no              | no              | no             | Sherwood           | pirates  |
| 55  | Valkyrie            | 0x03    | no              | yes             | no             | Valhalla           | merchant |
| 56  | Victoria            | 0x04    | yes             | yes             | yes            | Junction           | —        |
| 57  | Vishnu              | 0x03    | yes             | yes             | yes            | Rikel              | confed   |
| 58  | Wickerton           | 0x02    | no              | yes             | yes            | Manchester         | confed   |
| 59  | Derelict Base       | 0x06    | yes             | yes             | yes            | Delta Prime        | —        |

## Coverage status vs. assets/bases/<id>/base.json

5 of 60 bases have authored JSON (Achilles, Hector, Helen, Perry Naval,
Victoria). 55 are still missing.
