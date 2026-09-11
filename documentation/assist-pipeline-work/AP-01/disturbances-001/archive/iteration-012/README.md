# Dowody iteracji 012 (przed poprawka REWORK-004)

Zachowane zgodnie z karta TASK-EVD-AP-01-DISTURBANCES-REWORK-004 ("Zachowaj dowody 012").
Stan ODEBRANY czesciowo przez REVIEW-EVD-AP-01-012: D011-01/03/04 RESOLVED, D011-02 PARTIAL
(P-2 FAIL przyjete, P-1 otwarte jako D012-01).

- results/disturbances-results.json  - manifest przebiegu 012 (82 przypadki x 2)
- results/disturbances-metrics.json  - analiza 012 (P-1 z BLEDNA latencja 0.00 ms, patrz 013)
- results/CSV-SHA256.txt             - hashe wszystkich 82 CSV iteracji 012
- plots/SVG-SHA256.txt               - hashe wszystkich 36 SVG iteracji 012
- PROPOSED_CRITERIA.md               - kryteria v2 (P-1 bledne, sprostowane w v3)
- *.log                              - logi build/tests/runner/analyze/plots iteracji 012

Pliki CSV/SVG nie sa duplikowane (~220 MB); hashe identyfikuja plik, ale NIE zastepuja jego
tresci (REVIEW-EVD-AP-01-012). Pelne dane iteracji 011 i 012 zachowal dodatkowo Master we
wlasnych katalogach review.
