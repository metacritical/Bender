# Standalone material recipes

1. Inspect existing atlas color conventions and Janet module patterns.
2. Add only `src/material-recipes.janet` with versioned recipe data, validation, deterministic fallback, and CPU-only sampling for grain/noise, Voronoi, wave, checker, and brick.
3. Run a narrow syntax/load check if Janet is available; report exact public API and path. Do not change atlas/main or commit; parent owns tests.