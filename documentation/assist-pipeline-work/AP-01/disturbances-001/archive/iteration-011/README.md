# Dowody iteracji 011 (przed poprawką REWORK-003)

Zachowane zgodnie z kartą TASK-EVD-AP-01-DISTURBANCES-REWORK-003 ("Zachowaj dowody iteracji 011
przed podmianą wyników"). To jest stan ODEBRANY przez REVIEW-EVD-AP-01-011 jako odtwarzalny:
Master niezależnie powtórzył 74 przypadki x 2 i uzyskał 74/74 identyczne hashe CSV.

Zawartość:
- `results/disturbances-results.json`  — manifest przebiegu iteracji 011
- `results/disturbances-metrics.json`  — analiza iteracji 011
- `results/CSV-SHA256.txt`             — hashe wszystkich 74 CSV iteracji 011
- `plots/SVG-SHA256.txt`               — hashe wszystkich 31 SVG iteracji 011
- `plots/index.pl.md`                  — indeks wykresow iteracji 011
- `PROPOSED_CRITERIA.md`               — kryteria w brzmieniu z iteracji 011 (P-2 bledne, patrz 012)
- `*.log`                              — logi build/tests/runner/analyze/plots iteracji 011

Same pliki CSV/SVG iteracji 011 NIE sa tu kopiowane (ok. 190 MB); ich hashe wystarczaja do
stwierdzenia, ktore wyniki pochodza z ktorej iteracji. Hashe iteracji 012 sa w nowym manifescie
i nie wolno ich mieszac z powyzszymi.
