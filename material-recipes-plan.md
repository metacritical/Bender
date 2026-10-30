# Standalone material recipes

1. Inspect existing atlas color conventions and Bender module patterns.
2. Add only `src/material-recipes.ben` with versioned recipe data, validation, deterministic fallback, and CPU-only sampling for grain/noise, Voronoi, wave, checker, and brick.
3. Run a narrow syntax/load check if Bender is available; report exact public API and path. Do not change atlas/main or commit; parent owns tests.