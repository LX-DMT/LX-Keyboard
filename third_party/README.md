# Fixed keyboard and knob dependencies

These components are the exact local dependency snapshots used by the working
keyboard and knob projects before the ESP-IDF 5.5.6 migration. Upstream licenses
and metadata are preserved. This includes local modifications such as the
knob's LVGL colorwheel quality setting; it is not a claim of pristine upstream code.

See `../THIRD_PARTY.md` for versions and `../third_party.lock.json` for the current
complete SHA-256 inventory. Run `python scripts/verify_portability.py` from the
project root to verify it. Local relative path dependencies avoid fetching a
different component version or referring to another checkout.

`sources.json` is retained as historical provenance for the original keyboard
driver imports; it is not the current complete lockfile.
