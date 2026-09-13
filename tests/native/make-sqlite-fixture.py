#!/usr/bin/env python3
"""Create known-count test data; this is not a production language corpus."""
from pathlib import Path
import sqlite3
import sys

path = Path(sys.argv[1])
if path.exists():
    raise SystemExit("Refusing to replace existing data")
with sqlite3.connect(path) as db:
    for n in range(1, 4):
        columns = [f"word_{i} TEXT" for i in range(n - 1, 0, -1)] + ["word TEXT", "count INTEGER"]
        db.execute(f"CREATE TABLE _{n}_gram ({', '.join(columns)})")
    db.executemany("INSERT INTO _1_gram VALUES (?,?)", [
        ("see", 100), ("you", 100), ("later", 5), ("large", 500),
        ("last", 400), ("hello", 100), ("help", 50),
    ])
    db.executemany("INSERT INTO _2_gram VALUES (?,?,?)", [
        ("see", "you", 90), ("hello", "you", 90), ("you", "later", 90),
    ])
    db.executemany("INSERT INTO _3_gram VALUES (?,?,?,?)", [
        ("see", "you", "later", 85), ("hello", "you", "later", 85),
    ])
